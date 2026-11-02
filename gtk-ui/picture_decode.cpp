#include "picture_decode.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <jpeglib.h>
#include <png.h>

namespace Contrail {
namespace {

// Larger pictures are left to gdk-pixbuf, which has limits of its own. A progressive JPEG keeps all its coefficients
// whatever the scale (6 bytes a pixel), a PNG is decoded whole: 40 Mpx is ~240 MiB.
// Telegram's photos are at most 2560 px on a side.
constexpr auto kMaxPixels = std::uint64_t(40) << 20;
// A progressive JPEG keeps all its coefficients while it is decoded, whatever the scale: 15 MB for a 2560×1920
// photo, and nine in ten of Telegram's photos are progressive; a PNG is decoded whole. Those over 2 Mpx are decoded
// one at a time, so a fling over a page of them doesn't stack these up; the smaller ones go on beside.
constexpr auto kLargePixels = std::uint64_t(2) << 20;
GMutex LargeDecode;
std::atomic<int> LargeNow = 0;
std::atomic<int> LargeMost = 0;

enum class Turn { Taken, Busy, Cancelled };

// The turn of a large decode: waited for, or only taken if free (DecodeOptions::waitForLarge). Given back with
// GiveLargeTurn() when Taken.
[[nodiscard]] Turn TakeLargeTurn(const DecodeOptions &options) {
  if (options.waitForLarge) {
    g_mutex_lock(&LargeDecode);
  } else if (!g_mutex_trylock(&LargeDecode)) {
    if (options.deferred) *options.deferred = true;
    return Turn::Busy;
  }
  // The picture may have gone far while this waited.
  if (g_cancellable_is_cancelled(options.cancellable)) {
    g_mutex_unlock(&LargeDecode);
    return Turn::Cancelled;
  }
  const auto now = ++LargeNow;
  for (auto most = LargeMost.load(); now > most && !LargeMost.compare_exchange_weak(most, now);) {
  }
  return Turn::Taken;
}

void GiveLargeTurn() {
  --LargeNow;
  g_mutex_unlock(&LargeDecode);
}

struct Target {
  int width = 0;
  int height = 0;
};

// How much of a width×height picture is needed to cover the target, 1 for all of it.
[[nodiscard]] double CoverScale(int width, int height, Target target) {
  if (target.width <= 0 || target.height <= 0 || width <= target.width || height <= target.height) return 1.;
  return std::max(double(target.width) / width, double(target.height) / height);
}

// A side scaled to cover: a hair below a whole number is that number (719.9999 is 720, which covers 720).
[[nodiscard]] int Scaled(int full, double scale) {
  return std::max(int(std::ceil(full * scale - 0.01)), 1);
}

// The pixbuf cut down to cover the target. `pixbuf` is taken over. One pixel over is kept as it is: libjpeg rounds
// its n/8 sizes up, and resampling the whole picture for that pixel would only blur it.
[[nodiscard]] GdkPixbuf *Fit(GdkPixbuf *pixbuf, int fullWidth, int fullHeight, Target target) {
  const auto scale = CoverScale(fullWidth, fullHeight, target);
  const auto width = Scaled(fullWidth, scale), height = Scaled(fullHeight, scale);
  if (gdk_pixbuf_get_width(pixbuf) <= width + 1 && gdk_pixbuf_get_height(pixbuf) <= height + 1) return pixbuf;
  // Usually no more than twice as large here (libjpeg took the rest); bilinear averages the source pixels anyway.
  auto *scaled = gdk_pixbuf_scale_simple(pixbuf, width, height, GDK_INTERP_BILINEAR);
  g_object_unref(pixbuf);
  return scaled;
}

// --- JPEG ----------------------------------------------------------------------

// The EXIF orientation (1–8) in an APP1 marker, 1 if it has none.
[[nodiscard]] int ExifOrientation(const JOCTET *data, unsigned length) {
  constexpr auto kHeader = 6u;  // "Exif\0\0"
  if (length < kHeader + 8 || std::memcmp(data, "Exif\0\0", kHeader) != 0) return 1;
  const auto *tiff = data + kHeader;
  const auto size = length - kHeader;
  const auto little = (tiff[0] == 'I' && tiff[1] == 'I');
  if (!little && !(tiff[0] == 'M' && tiff[1] == 'M')) return 1;
  const auto u16 = [&](unsigned at) {
    return little ? unsigned(tiff[at] | (tiff[at + 1] << 8)) : unsigned((tiff[at] << 8) | tiff[at + 1]);
  };
  const auto u32 = [&](unsigned at) {
    return little ? (std::uint32_t(u16(at + 2)) << 16) | u16(at) : (std::uint32_t(u16(at)) << 16) | u16(at + 2);
  };
  if (u16(2) != 42) return 1;
  const auto ifd = std::uint64_t(u32(4));
  if (ifd + 2 > size) return 1;
  const auto count = u16(unsigned(ifd));
  for (auto i = 0u; i < count; ++i) {
    const auto entry = ifd + 2 + std::uint64_t(i) * 12;
    if (entry + 12 > size) break;
    const auto at = unsigned(entry);
    if (u16(at) == 0x0112) {  // Orientation, a SHORT.
      const auto value = (u16(at + 2) == 3) ? u16(at + 8) : 0u;
      return (value >= 1 && value <= 8) ? int(value) : 1;
    }
  }
  return 1;
}

struct JpegError {
  jpeg_error_mgr manager;
  std::jmp_buf jump;
};

void JpegFail(j_common_ptr info) {
  std::longjmp(reinterpret_cast<JpegError *>(info->err)->jump, 1);
}

void JpegQuiet(j_common_ptr, int) {}  // Warnings: a truncated file still gives its picture, gray below.

// Everything the decoder touches after setjmp lives here, outside the jump's way: no C++ object in between.
struct Jpeg {
  jpeg_decompress_struct info{};
  JpegError error{};
  FILE *file = nullptr;
  GdkPixbuf *pixbuf = nullptr;
  int orientation = 1;
  int width = 0;  // Of the whole picture, as stored.
  int height = 0;
  bool large = false;    // The large turn is held.
  bool stopped = false;  // Deferred or cancelled: not a bad file.
};

[[nodiscard]] bool ReadJpeg(Jpeg &jpeg, Target target, const DecodeOptions &options) {
  jpeg.info.err = jpeg_std_error(&jpeg.error.manager);
  jpeg.error.manager.error_exit = JpegFail;
  jpeg.error.manager.emit_message = JpegQuiet;
  if (setjmp(jpeg.error.jump)) return false;
  jpeg_create_decompress(&jpeg.info);
  jpeg_stdio_src(&jpeg.info, jpeg.file);
  jpeg_save_markers(&jpeg.info, JPEG_APP0 + 1, 0xffff);
  jpeg_read_header(&jpeg.info, TRUE);
  for (auto *marker = jpeg.info.marker_list; marker; marker = marker->next) {
    if (marker->marker == JPEG_APP0 + 1) {
      jpeg.orientation = ExifOrientation(marker->data, marker->data_length);
      if (jpeg.orientation != 1) break;
    }
  }
  // CMYK and the like: libjpeg doesn't turn them into RGB.
  if (jpeg.info.jpeg_color_space != JCS_YCbCr && jpeg.info.jpeg_color_space != JCS_GRAYSCALE
      && jpeg.info.jpeg_color_space != JCS_RGB) {
    return false;
  }
  const auto width = jpeg.width = int(jpeg.info.image_width);
  const auto height = jpeg.height = int(jpeg.info.image_height);
  if (std::uint64_t(width) * height > kMaxPixels) return false;
  // The target is on the screen, turned; the file is not.
  if (jpeg.orientation >= 5) std::swap(target.width, target.height);
  // The smallest n/8 of the size still covering the target.
  const auto scale = CoverScale(width, height, target);
  jpeg.info.scale_num = std::clamp(unsigned(std::ceil(scale * 8 - 1e-9)), 1u, 8u);
  jpeg.info.scale_denom = 8;
  jpeg.info.out_color_space = JCS_EXT_RGB;
  if (jpeg.info.progressive_mode && std::uint64_t(width) * height > kLargePixels) {
    jpeg.large = (TakeLargeTurn(options) == Turn::Taken);
    jpeg.stopped = !jpeg.large;
    if (jpeg.stopped) return false;
  }
  jpeg_start_decompress(&jpeg.info);
  jpeg.pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, int(jpeg.info.output_width),
                               int(jpeg.info.output_height));
  if (!jpeg.pixbuf) return false;
  auto *pixels = gdk_pixbuf_get_pixels(jpeg.pixbuf);
  const auto stride = gdk_pixbuf_get_rowstride(jpeg.pixbuf);
  while (jpeg.info.output_scanline < jpeg.info.output_height) {
    JSAMPROW row = pixels + std::size_t(jpeg.info.output_scanline) * stride;
    jpeg_read_scanlines(&jpeg.info, &row, 1);
  }
  jpeg_finish_decompress(&jpeg.info);
  return true;
}

[[nodiscard]] GdkPixbuf *DecodeJpeg(const char *path, Target target, const DecodeOptions &options, bool &stopped) {
  auto jpeg = Jpeg();
  jpeg.file = std::fopen(path, "rbe");
  if (!jpeg.file) return nullptr;
  const auto read = ReadJpeg(jpeg, target, options);
  jpeg_destroy_decompress(&jpeg.info);  // Its memory goes before the next large one starts.
  if (jpeg.large) GiveLargeTurn();
  std::fclose(jpeg.file);
  stopped = jpeg.stopped;
  if (!read) {
    g_clear_object(&jpeg.pixbuf);
    return nullptr;
  }
  const auto turned = (jpeg.orientation >= 5);
  auto *fitted = Fit(jpeg.pixbuf, jpeg.width, jpeg.height, turned ? Target{target.height, target.width} : target);
  if (!fitted || jpeg.orientation == 1) return fitted;
  // gdk-pixbuf turns it as it would have turned its own decoding.
  char value[2] = {char('0' + jpeg.orientation), 0};
  gdk_pixbuf_set_option(fitted, "orientation", value);
  auto *result = gdk_pixbuf_apply_embedded_orientation(fitted);
  g_object_unref(fitted);
  return result;
}

// --- PNG -----------------------------------------------------------------------

[[nodiscard]] GdkPixbuf *DecodePng(const char *path, Target target, const DecodeOptions &options, bool &stopped) {
  auto image = png_image();
  image.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_file(&image, path)) return nullptr;
  const auto alpha = (image.format & PNG_FORMAT_FLAG_ALPHA) != 0;  // A palette's transparency too.
  const auto pixels = std::uint64_t(image.width) * image.height;
  if (pixels > kMaxPixels) {
    png_image_free(&image);
    return nullptr;
  }
  const auto large = (pixels > kLargePixels);
  if (large && TakeLargeTurn(options) != Turn::Taken) {
    png_image_free(&image);
    stopped = true;
    return nullptr;
  }
  image.format = alpha ? PNG_FORMAT_RGBA : PNG_FORMAT_RGB;
  auto *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, alpha, 8, int(image.width), int(image.height));
  const auto read = pixbuf
                    && png_image_finish_read(&image, nullptr, gdk_pixbuf_get_pixels(pixbuf),
                                             gdk_pixbuf_get_rowstride(pixbuf), nullptr);
  png_image_free(&image);  // Safe after libpng's own cleanup on a failed read.
  if (large) GiveLargeTurn();
  if (!read) {
    g_clear_object(&pixbuf);
    return nullptr;
  }
  return Fit(pixbuf, int(image.width), int(image.height), target);
}

// --- anything else -------------------------------------------------------------

[[nodiscard]] GdkPixbuf *DecodeWithPixbuf(const char *path, Target target) {
  auto width = 0, height = 0;
  if (!gdk_pixbuf_get_file_info(path, &width, &height)) return nullptr;
  const auto scale = CoverScale(width, height, target);
  auto *pixbuf = (scale < 1.) ? gdk_pixbuf_new_from_file_at_scale(path, Scaled(width, scale), Scaled(height, scale),
                                                                  TRUE, nullptr)
                              : gdk_pixbuf_new_from_file(path, nullptr);
  if (!pixbuf) return nullptr;
  // Files keep their camera's rotation; Telegram's own photos have none and this returns the same picture.
  auto *turned = gdk_pixbuf_apply_embedded_orientation(pixbuf);
  g_object_unref(pixbuf);
  return turned;
}

enum class Format { Jpeg, Png, Other };

[[nodiscard]] Format Sniff(const char *path) {
  unsigned char head[8] = {};
  auto *file = std::fopen(path, "rbe");
  if (!file) return Format::Other;
  const auto read = std::fread(head, 1, sizeof(head), file);
  std::fclose(file);
  if (read >= 3 && head[0] == 0xff && head[1] == 0xd8 && head[2] == 0xff) return Format::Jpeg;
  if (read == 8 && std::memcmp(head, "\x89PNG\r\n\x1a\n", 8) == 0) return Format::Png;
  return Format::Other;
}

}  // namespace

GdkPixbuf *DecodePictureFile(const char *path, int width, int height, const DecodeOptions &options) {
  if (g_cancellable_is_cancelled(options.cancellable)) return nullptr;
  const auto target = Target{width, height};
  GdkPixbuf *result = nullptr;
  auto stopped = false;
  switch (Sniff(path)) {
    case Format::Jpeg: result = DecodeJpeg(path, target, options, stopped); break;
    case Format::Png: result = DecodePng(path, target, options, stopped); break;
    case Format::Other: break;
  }
  return (result || stopped || !options.pixbufFallback) ? result : DecodeWithPixbuf(path, target);
}

int LargeDecodesAtOnce() {
  return LargeMost.load();
}

int LargeDecodesNow() {
  return LargeNow.load();
}

GdkTexture *TextureFromPixbuf(GdkPixbuf *pixbuf) {
  const auto width = gdk_pixbuf_get_width(pixbuf), height = gdk_pixbuf_get_height(pixbuf);
  const auto stride = gdk_pixbuf_get_rowstride(pixbuf);
  auto *bytes = g_bytes_new_with_free_func(gdk_pixbuf_read_pixels(pixbuf), gdk_pixbuf_get_byte_length(pixbuf),
                                           g_object_unref, pixbuf);
  const auto format = gdk_pixbuf_get_has_alpha(pixbuf) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8;
  auto *texture = gdk_memory_texture_new(width, height, format, bytes, stride);
  g_bytes_unref(bytes);
  return texture;
}

}  // namespace Contrail
