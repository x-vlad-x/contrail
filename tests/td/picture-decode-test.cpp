// DecodePictureFile against what gdk-pixbuf gave before: sizes, pixels, EXIF orientation, transparency, bad files.
#include "picture_decode.hpp"

#include <glib.h>
#include <glib/gstdio.h>
#include <png.h>

#include <cstdio>
#include <jpeglib.h>
#include <thread>
#include <vector>

#include <cstdlib>
#include <string>
#include <utility>

namespace Contrail {
namespace {

std::string TempPath(const char *name) {
  static auto *dir = g_dir_make_tmp("picture-decode-XXXXXX", nullptr);
  g_assert_nonnull(dir);
  auto *path = g_build_filename(dir, name, nullptr);
  auto result = std::string(path);
  g_free(path);
  return result;
}

// A picture with a gradient, and a red top-left quarter to see where it turns.
GdkPixbuf *Sample(int width, int height, bool alpha) {
  auto *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, alpha, 8, width, height);
  const auto channels = gdk_pixbuf_get_n_channels(pixbuf), stride = gdk_pixbuf_get_rowstride(pixbuf);
  auto *pixels = gdk_pixbuf_get_pixels(pixbuf);
  for (auto y = 0; y < height; ++y) {
    for (auto x = 0; x < width; ++x) {
      auto *p = pixels + y * stride + x * channels;
      const auto corner = (x < width / 2 && y < height / 2);
      p[0] = corner ? 230 : guchar(x * 255 / width);
      p[1] = corner ? 20 : guchar(y * 255 / height);
      p[2] = corner ? 20 : 128;
      if (alpha) p[3] = guchar(x < width / 2 ? 255 : 64);
    }
  }
  return pixbuf;
}

std::string SaveJpeg(const char *name, int width, int height) {
  auto *pixbuf = Sample(width, height, false);
  const auto path = TempPath(name);
  g_assert_true(gdk_pixbuf_save(pixbuf, path.c_str(), "jpeg", nullptr, "quality", "95", nullptr));
  g_object_unref(pixbuf);
  return path;
}

std::string Read(const std::string &path) {
  gchar *data = nullptr;
  gsize size = 0;
  g_assert_true(g_file_get_contents(path.c_str(), &data, &size, nullptr));
  auto result = std::string(data, size);
  g_free(data);
  return result;
}

void Write(const std::string &path, const std::string &bytes) {
  g_assert_true(g_file_set_contents(path.c_str(), bytes.data(), gssize(bytes.size()), nullptr));
}

// The JPEG with an APP1 marker right after SOI: "Exif\0\0" and these TIFF bytes.
std::string WithExif(const std::string &jpeg, const std::string &tiff) {
  const auto app1 = std::string("Exif\0\0", 6) + tiff;
  const auto length = app1.size() + 2;
  auto marker = std::string("\xff\xe1", 2);
  marker += char(length >> 8), marker += char(length & 0xff);
  return jpeg.substr(0, 2) + marker + app1 + jpeg.substr(2);
}

// The same JPEG with an EXIF APP1 marker right after SOI saying `orientation`, big-endian ("MM") or not.
std::string WithOrientation(const std::string &jpeg, int orientation, bool bigEndian) {
  auto tiff = std::string();
  const auto put16 = [&](unsigned v) {
    if (bigEndian) {
      tiff += char(v >> 8), tiff += char(v & 0xff);
    } else {
      tiff += char(v & 0xff), tiff += char(v >> 8);
    }
  };
  const auto put32 = [&](unsigned v) {
    if (bigEndian) {
      put16(v >> 16), put16(v & 0xffff);
    } else {
      put16(v & 0xffff), put16(v >> 16);
    }
  };
  tiff += bigEndian ? "MM" : "II";
  put16(42);
  put32(8);
  put16(1);                                   // One entry:
  put16(0x0112), put16(3), put32(1);          // Orientation, SHORT, 1 value,
  put16(unsigned(orientation)), put16(0);     // in the first two bytes of the field.
  put32(0);                                   // No next IFD.
  return WithExif(jpeg, tiff);
}

void Pixel(GdkPixbuf *pixbuf, int x, int y, int rgb[3]) {
  const auto *p = gdk_pixbuf_get_pixels(pixbuf) + y * gdk_pixbuf_get_rowstride(pixbuf)
                  + x * gdk_pixbuf_get_n_channels(pixbuf);
  rgb[0] = p[0], rgb[1] = p[1], rgb[2] = p[2];
}

bool IsRed(GdkPixbuf *pixbuf, int x, int y) {
  int rgb[3];
  Pixel(pixbuf, x, y, rgb);
  return rgb[0] > 180 && rgb[1] < 70 && rgb[2] < 70;
}

// The mean difference of each channel, on the same size.
double MeanDifference(GdkPixbuf *a, GdkPixbuf *b) {
  g_assert_cmpint(gdk_pixbuf_get_width(a), ==, gdk_pixbuf_get_width(b));
  g_assert_cmpint(gdk_pixbuf_get_height(a), ==, gdk_pixbuf_get_height(b));
  auto sum = 0.;
  const auto width = gdk_pixbuf_get_width(a), height = gdk_pixbuf_get_height(a);
  for (auto y = 0; y < height; ++y) {
    for (auto x = 0; x < width; ++x) {
      int p[3], q[3];
      Pixel(a, x, y, p);
      Pixel(b, x, y, q);
      for (auto c = 0; c < 3; ++c) sum += std::abs(p[c] - q[c]);
    }
  }
  return sum / (width * height * 3.);
}

void JpegCoversTheTarget() {
  const auto path = SaveJpeg("big.jpg", 2560, 1920);
  auto *decoded = DecodePictureFile(path.c_str(), 720, 720);
  g_assert_nonnull(decoded);
  // What gdk_pixbuf_new_from_file_at_scale gave: 2560 × 0.375 by 1920 × 0.375.
  g_assert_cmpint(gdk_pixbuf_get_width(decoded), ==, 960);
  g_assert_cmpint(gdk_pixbuf_get_height(decoded), ==, 720);
  g_assert_false(gdk_pixbuf_get_has_alpha(decoded));
  auto *reference = gdk_pixbuf_new_from_file_at_scale(path.c_str(), 960, 720, TRUE, nullptr);
  g_assert_nonnull(reference);
  g_assert_cmpfloat(MeanDifference(decoded, reference), <, 3.);
  g_assert_true(IsRed(decoded, 10, 10));
  g_assert_false(IsRed(decoded, 950, 10));
  g_object_unref(reference);
  g_object_unref(decoded);
}

void SmallJpegStaysWhole() {
  const auto path = SaveJpeg("small.jpg", 100, 80);
  for (const auto &[width, height] : {std::pair{720, 720}, std::pair{0, 0}, std::pair{50, 200}}) {
    auto *decoded = DecodePictureFile(path.c_str(), width, height);
    g_assert_nonnull(decoded);
    g_assert_cmpint(gdk_pixbuf_get_width(decoded), ==, 100);
    g_assert_cmpint(gdk_pixbuf_get_height(decoded), ==, 80);
    g_object_unref(decoded);
  }
}

void ExifOrientationTurns() {
  const auto plain = Read(SaveJpeg("plain.jpg", 400, 200));
  for (const auto bigEndian : {false, true}) {
    // 6: turned a quarter clockwise, so the top-left corner comes to the top right, 200 × 400.
    const auto path = TempPath(bigEndian ? "turned-mm.jpg" : "turned-ii.jpg");
    Write(path, WithOrientation(plain, 6, bigEndian));
    auto *decoded = DecodePictureFile(path.c_str(), 100, 100);
    g_assert_nonnull(decoded);
    g_assert_cmpint(gdk_pixbuf_get_width(decoded), ==, 100);
    g_assert_cmpint(gdk_pixbuf_get_height(decoded), ==, 200);
    g_assert_true(IsRed(decoded, 90, 10));
    g_assert_false(IsRed(decoded, 10, 10));
    g_object_unref(decoded);
  }
  // 3: upside down, the red corner at the bottom right; the size stays.
  const auto path = TempPath("upside-down.jpg");
  Write(path, WithOrientation(plain, 3, false));
  auto *decoded = DecodePictureFile(path.c_str(), 0, 0);
  g_assert_nonnull(decoded);
  g_assert_cmpint(gdk_pixbuf_get_width(decoded), ==, 400);
  g_assert_true(IsRed(decoded, 390, 190));
  g_assert_false(IsRed(decoded, 10, 10));
  g_object_unref(decoded);
}

void BrokenExifIsIgnored() {
  const auto plain = Read(SaveJpeg("exif-base.jpg", 400, 200));
  const auto header = std::string("II*\0", 4);
  const auto cases = {
      header + std::string("\x08\0\0\0", 4),                                 // 8 bytes: the IFD isn't there.
      header + std::string("\x08\0\0\0\x05\0", 6),                           // 10: five entries, none present.
      header + std::string("\x08\0\0\0\xff\xff\x12\x01\x03", 9) ,           // 11: an entry cut off.
      header + std::string("\xf0\xff\xff\xff", 4) + std::string(8, '\0'),     // The IFD far past the end.
      header + std::string("\x08\0\0\0\xff\xff", 6) + std::string(30, '\x01'),  // 65535 entries in 30 bytes.
      std::string("XX*\0\x08\0\0\0", 8) + std::string(12, '\0'),              // Neither II nor MM.
  };
  auto index = 0;
  for (const auto &tiff : cases) {
    const auto path = TempPath(("broken-exif-" + std::to_string(index++) + ".jpg").c_str());
    Write(path, WithExif(plain, tiff));
    auto *decoded = DecodePictureFile(path.c_str(), 0, 0);
    g_assert_nonnull(decoded);
    g_assert_cmpint(gdk_pixbuf_get_width(decoded), ==, 400);
    g_assert_true(IsRed(decoded, 10, 10));
    g_object_unref(decoded);
  }
}

void PngKeepsTransparency() {
  auto *sample = Sample(300, 300, true);
  const auto path = TempPath("alpha.png");
  g_assert_true(gdk_pixbuf_save(sample, path.c_str(), "png", nullptr, nullptr));
  auto *decoded = DecodePictureFile(path.c_str(), 100, 100);
  g_assert_nonnull(decoded);
  g_assert_true(gdk_pixbuf_get_has_alpha(decoded));
  g_assert_cmpint(gdk_pixbuf_get_width(decoded), ==, 100);
  const auto *pixels = gdk_pixbuf_get_pixels(decoded);
  const auto stride = gdk_pixbuf_get_rowstride(decoded);
  g_assert_cmpint(pixels[50 * stride + 10 * 4 + 3], ==, 255);
  g_assert_cmpint(std::abs(pixels[50 * stride + 90 * 4 + 3] - 64), <=, 2);
  g_object_unref(decoded);
  // Without alpha it stays three channels.
  auto *opaque = Sample(64, 64, false);
  const auto rgb = TempPath("rgb.png");
  g_assert_true(gdk_pixbuf_save(opaque, rgb.c_str(), "png", nullptr, nullptr));
  decoded = DecodePictureFile(rgb.c_str(), 0, 0);
  g_assert_nonnull(decoded);
  g_assert_false(gdk_pixbuf_get_has_alpha(decoded));
  g_assert_cmpfloat(MeanDifference(decoded, opaque), ==, 0.);
  g_object_unref(decoded);
  g_object_unref(opaque);
  g_object_unref(sample);
}

void WidePngsBecomeEightBits() {
  // 16 bits a sample, linear: black and white stay black and white.
  auto image = png_image();
  image.version = PNG_IMAGE_VERSION;
  image.width = 4;
  image.height = 2;
  image.format = PNG_FORMAT_LINEAR_RGB;
  png_uint_16 pixels[4 * 2 * 3];
  for (auto i = 0; i < 4 * 2 * 3; ++i) pixels[i] = (i < 12) ? 0 : 65535;
  const auto wide = TempPath("wide.png");
  g_assert_true(png_image_write_to_file(&image, wide.c_str(), 0, pixels, 0, nullptr));
  auto *decoded = DecodePictureFile(wide.c_str(), 0, 0);
  g_assert_nonnull(decoded);
  g_assert_cmpint(gdk_pixbuf_get_bits_per_sample(decoded), ==, 8);
  int rgb[3];
  Pixel(decoded, 0, 0, rgb);
  g_assert_cmpint(rgb[0] + rgb[1] + rgb[2], ==, 0);
  Pixel(decoded, 3, 1, rgb);
  g_assert_cmpint(rgb[0] + rgb[1] + rgb[2], ==, 3 * 255);
  g_object_unref(decoded);
  // A palette with a transparent entry: the pixbuf has alpha, 0 there.
  auto mapped = png_image();
  mapped.version = PNG_IMAGE_VERSION;
  mapped.width = 2;
  mapped.height = 1;
  mapped.format = PNG_FORMAT_RGBA_COLORMAP;
  mapped.colormap_entries = 2;
  const png_byte colormap[] = {255, 0, 0, 255, 0, 0, 255, 0};
  const png_byte indices[] = {0, 1};
  const auto palette = TempPath("palette.png");
  g_assert_true(png_image_write_to_file(&mapped, palette.c_str(), 0, indices, 0, colormap));
  decoded = DecodePictureFile(palette.c_str(), 0, 0);
  g_assert_nonnull(decoded);
  g_assert_true(gdk_pixbuf_get_has_alpha(decoded));
  const auto *bytes = gdk_pixbuf_get_pixels(decoded);
  g_assert_cmpint(bytes[3], ==, 255);
  g_assert_cmpint(bytes[0], ==, 255);
  g_assert_cmpint(bytes[7], ==, 0);
  g_object_unref(decoded);
}

// A progressive JPEG, as nine in ten of Telegram's photos are.
std::string SaveProgressiveJpeg(const char *name, int width, int height) {
  auto *sample = Sample(width, height, false);
  const auto path = TempPath(name);
  auto *file = std::fopen(path.c_str(), "wb");
  g_assert_nonnull(file);
  jpeg_compress_struct info{};
  jpeg_error_mgr error{};
  info.err = jpeg_std_error(&error);
  jpeg_create_compress(&info);
  jpeg_stdio_dest(&info, file);
  info.image_width = width;
  info.image_height = height;
  info.input_components = 3;
  info.in_color_space = JCS_RGB;
  jpeg_set_defaults(&info);
  jpeg_set_quality(&info, 90, TRUE);
  jpeg_simple_progression(&info);
  jpeg_start_compress(&info, TRUE);
  while (info.next_scanline < info.image_height) {
    JSAMPROW row = gdk_pixbuf_get_pixels(sample) + std::size_t(info.next_scanline) * gdk_pixbuf_get_rowstride(sample);
    jpeg_write_scanlines(&info, &row, 1);
  }
  jpeg_finish_compress(&info);
  jpeg_destroy_compress(&info);
  std::fclose(file);
  g_object_unref(sample);
  return path;
}

void LargeProgressiveOnesTakeTurns() {
  const auto large = SaveProgressiveJpeg("large-progressive.jpg", 2560, 1920);
  const auto small = SaveProgressiveJpeg("small-progressive.jpg", 640, 480);
  auto sizes = std::vector<std::pair<int, int>>(6);
  auto threads = std::vector<std::thread>();
  for (auto i = 0; i < 6; ++i) {
    threads.emplace_back([&, i] {
      auto *decoded = DecodePictureFile((i % 2 ? small : large).c_str(), 300, 300);
      if (decoded) {
        sizes[i] = {gdk_pixbuf_get_width(decoded), gdk_pixbuf_get_height(decoded)};
        g_object_unref(decoded);
      }
    });
  }
  for (auto &thread : threads) thread.join();
  for (auto i = 0; i < 6; ++i) {
    g_assert_cmpint(sizes[i].first, ==, 400);
    g_assert_cmpint(sizes[i].second, ==, 300);
  }
}

void BadFilesGiveNothing() {
  g_assert_null(DecodePictureFile(TempPath("missing.jpg").c_str(), 100, 100));
  const auto garbage = TempPath("garbage.jpg");
  Write(garbage, std::string("\xff\xd8\xff\xe0 this is not a picture at all", 34));
  g_assert_null(DecodePictureFile(garbage.c_str(), 100, 100));
  const auto text = TempPath("text.png");
  Write(text, "hello");
  g_assert_null(DecodePictureFile(text.c_str(), 100, 100));
}

void TruncatedJpegStillShows() {
  const auto whole = Read(SaveJpeg("whole.jpg", 640, 480));
  const auto path = TempPath("truncated.jpg");
  Write(path, whole.substr(0, whole.size() / 2));
  auto *decoded = DecodePictureFile(path.c_str(), 0, 0);
  g_assert_nonnull(decoded);
  g_assert_cmpint(gdk_pixbuf_get_width(decoded), ==, 640);
  g_assert_true(IsRed(decoded, 10, 10));
  g_object_unref(decoded);
}

}  // namespace
}  // namespace Contrail

int main(int argc, char **argv) {
  using namespace Contrail;
  g_test_init(&argc, &argv, nullptr);
  g_test_add_func("/picture/jpeg-cover", JpegCoversTheTarget);
  g_test_add_func("/picture/jpeg-small", SmallJpegStaysWhole);
  g_test_add_func("/picture/exif", ExifOrientationTurns);
  g_test_add_func("/picture/exif-broken", BrokenExifIsIgnored);
  g_test_add_func("/picture/png", PngKeepsTransparency);
  g_test_add_func("/picture/png-wide", WidePngsBecomeEightBits);
  g_test_add_func("/picture/progressive", LargeProgressiveOnesTakeTurns);
  g_test_add_func("/picture/bad", BadFilesGiveNothing);
  g_test_add_func("/picture/truncated", TruncatedJpegStillShows);
  return g_test_run();
}
