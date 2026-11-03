#include "attach_media.hpp"

#include "picture_decode.hpp"
#include "td/session.hpp"
#include "td/strings.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libswscale/swscale.h>
}
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <utility>

namespace Contrail {
namespace {

namespace api = Td::api;

constexpr auto kPhotoQuality = "87";  // As Telegram Desktop saves a photo it sends.
constexpr auto kThumbnailQuality = "80";
constexpr auto kMaxRatio = 20;  // A picture narrower than this goes as a file, as Telegram Desktop decides.
constexpr auto kSniffBytes = 4096;
constexpr auto kFramePackets = 300;  // The first frame of a video comes within these, or the thumbnail is skipped.
constexpr auto kKeepFiles = gint64(7) * 24 * 3600 * G_USEC_PER_SEC;

std::string BaseName(const std::string &path) {
  auto *name = g_path_get_basename(path.c_str());
  auto result = std::string(name);
  g_free(name);
  return result;
}

std::string Extension(const std::string &name) {
  const auto dot = name.rfind('.');
  if (dot == std::string::npos) return {};
  auto result = name.substr(dot + 1);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return result;
}

// By what the file holds, as GIO tells it, not by its name only.
std::string MimeOf(const std::string &path) {
  auto data = std::string(kSniffBytes, '\0');
  auto length = gsize(0);
  if (auto *file = g_fopen(path.c_str(), "rb")) {
    length = fread(data.data(), 1, data.size(), file);
    fclose(file);
  }
  auto uncertain = gboolean(FALSE);
  auto *type = g_content_type_guess(path.c_str(), reinterpret_cast<const guchar *>(data.data()), length, &uncertain);
  auto *mime = type ? g_content_type_get_mime_type(type) : nullptr;
  auto result = std::string(mime ? mime : "application/octet-stream");
  g_free(mime);
  g_free(type);
  return result;
}

std::string UniquePath(const std::string &directory, const char *prefix, const char *extension) {
  static std::atomic<int> counter = 0;
  auto *name = g_strdup_printf("%s-%lld-%d.%s", prefix, (long long)g_get_real_time(), ++counter, extension);
  auto *path = g_build_filename(directory.c_str(), name, nullptr);
  auto result = std::string(path);
  g_free(path);
  g_free(name);
  return result;
}

bool SaveJpeg(GdkPixbuf *pixbuf, const std::string &path, const char *quality) {
  auto *error = static_cast<GError *>(nullptr);
  if (gdk_pixbuf_save(pixbuf, path.c_str(), "jpeg", &error, "quality", quality, nullptr)) return true;
  g_warning("Attach: %s not written: %s", path.c_str(), error ? error->message : "?");
  g_clear_error(&error);
  return false;
}

// At most `side` a side, never larger than it is.
GdkPixbuf *Fit(GdkPixbuf *pixbuf, int side) {
  const auto width = gdk_pixbuf_get_width(pixbuf), height = gdk_pixbuf_get_height(pixbuf);
  const auto largest = std::max(width, height);
  if (largest <= side) return pixbuf;
  const auto scale = double(side) / largest;
  auto *result = gdk_pixbuf_scale_simple(pixbuf, std::max(1, int(std::lround(width * scale))),
                                         std::max(1, int(std::lround(height * scale))), GDK_INTERP_BILINEAR);
  g_object_unref(pixbuf);
  return result;
}

// The photo as Telegram Desktop sends one: upright, at most kPhotoSideLimit a side, a JPEG. False when the picture
// can't be read or isn't one Telegram shows as a photo.
bool PreparePhoto(AttachFile &file, const std::string &directory) {
  auto width = 0, height = 0;
  if (!gdk_pixbuf_get_file_info(file.path.c_str(), &width, &height) || width <= 0 || height <= 0) return false;
  if (std::max(width, height) > kMaxRatio * std::min(width, height)) return false;
  // Decoded no larger than needed: its shorter side covers what the longer one is scaled to.
  const auto scale = std::min(1., double(kPhotoSideLimit) / std::max(width, height));
  const auto shorter = std::max(1, int(std::lround(std::min(width, height) * scale)));
  auto *pixbuf = DecodePictureFile(file.path.c_str(), shorter, shorter);
  if (!pixbuf) return false;
  pixbuf = Fit(pixbuf, kPhotoSideLimit);
  file.width = gdk_pixbuf_get_width(pixbuf);
  file.height = gdk_pixbuf_get_height(pixbuf);
  const auto path = UniquePath(directory, "photo", "jpg");
  const auto saved = SaveJpeg(pixbuf, path, kPhotoQuality);
  g_object_unref(pixbuf);
  if (saved) file.photoPath = path;
  return saved;
}

int Rotation(const AVStream &stream) {
  const auto *data = av_packet_side_data_get(stream.codecpar->coded_side_data, stream.codecpar->nb_coded_side_data,
                                             AV_PKT_DATA_DISPLAYMATRIX);
  if (!data || data->size < 9 * sizeof(std::int32_t)) return 0;
  // The matrix turns it counterclockwise.
  const auto degrees = -av_display_rotation_get(reinterpret_cast<const std::int32_t *>(data->data));
  if (std::isnan(degrees)) return 0;
  return ((int(std::lround(degrees / 90.)) * 90) % 360 + 360) % 360;
}

std::string Tag(AVDictionary *first, AVDictionary *second, const char *key) {
  const auto *entry = av_dict_get(first, key, nullptr, 0);
  if (!entry && second) entry = av_dict_get(second, key, nullptr, 0);
  return entry && entry->value ? std::string(entry->value) : std::string();
}

// A video's first frame, upright and at most kThumbnailSide a side.
GdkPixbuf *FirstFrame(const std::string &path) {
  auto *format = static_cast<AVFormatContext *>(nullptr);
  if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) return nullptr;
  auto *result = static_cast<GdkPixbuf *>(nullptr);
  auto *codec = static_cast<AVCodecContext *>(nullptr);
  auto *packet = av_packet_alloc();
  auto *frame = av_frame_alloc();
  const AVCodec *decoder = nullptr;
  const auto index = (avformat_find_stream_info(format, nullptr) >= 0)
                         ? av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0)
                         : -1;
  if (index >= 0 && decoder && packet && frame && (codec = avcodec_alloc_context3(decoder))
      && avcodec_parameters_to_context(codec, format->streams[index]->codecpar) >= 0
      && avcodec_open2(codec, decoder, nullptr) >= 0) {
    auto got = false;
    for (auto packets = 0; !got && packets < kFramePackets; ++packets) {
      const auto read = av_read_frame(format, packet);
      if (read >= 0 && packet->stream_index != index) {
        av_packet_unref(packet);
        continue;
      }
      // At the end the decoder gives what it holds.
      avcodec_send_packet(codec, read >= 0 ? packet : nullptr);
      av_packet_unref(packet);
      got = avcodec_receive_frame(codec, frame) == 0;
      if (read < 0) break;
    }
    if (got && frame->width > 0 && frame->height > 0) {
      const auto scale = std::min(1., double(kThumbnailSide) / std::max(frame->width, frame->height));
      const auto width = std::max(1, int(std::lround(frame->width * scale)));
      const auto height = std::max(1, int(std::lround(frame->height * scale)));
      auto *scaler = sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format), width, height,
                                    AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
      if (scaler) {
        auto *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);
        std::uint8_t *planes[4] = {gdk_pixbuf_get_pixels(pixbuf), nullptr, nullptr, nullptr};
        const int strides[4] = {gdk_pixbuf_get_rowstride(pixbuf), 0, 0, 0};
        if (sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, planes, strides) == height) {
          result = pixbuf;
        } else {
          g_object_unref(pixbuf);
        }
        sws_freeContext(scaler);
      }
    }
    if (result) {
      // GdkPixbuf turns counterclockwise.
      if (const auto rotation = Rotation(*format->streams[index])) {
        auto *turned = gdk_pixbuf_rotate_simple(result, GdkPixbufRotation((360 - rotation) % 360));
        g_object_unref(result);
        result = turned;
      }
    }
  }
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&codec);
  avformat_close_input(&format);
  return result;
}

void PrepareThumbnail(AttachFile &file, const std::string &directory) {
  auto *frame = FirstFrame(file.path);
  if (!frame) return;
  const auto path = UniquePath(directory, "thumbnail", "jpg");
  if (SaveJpeg(frame, path, kThumbnailQuality)) {
    file.thumbnailPath = path;
    file.thumbnailWidth = gdk_pixbuf_get_width(frame);
    file.thumbnailHeight = gdk_pixbuf_get_height(frame);
  }
  g_object_unref(frame);
}

std::string SizeLimitText(std::int64_t limit) {
  return std::to_string(limit / (1024 * 1024)) + " MB";
}

api::object_ptr<api::InputFile> Local(const std::string &path) {
  return api::make_object<api::inputFileLocal>(path);
}

api::object_ptr<api::inputThumbnail> Thumbnail(const AttachFile &file) {
  if (file.thumbnailPath.empty()) return nullptr;
  return api::make_object<api::inputThumbnail>(Local(file.thumbnailPath), file.thumbnailWidth, file.thumbnailHeight);
}

}  // namespace

std::string AttachRefusal(const std::string &path, bool premium) {
  auto name = BaseName(path);
  GStatBuf info;
  const auto exists = g_stat(path.c_str(), &info) == 0;
  if (!exists || !S_ISREG(info.st_mode) || access(path.c_str(), R_OK) != 0 || info.st_size <= 0) {
    return Tr("lng_send_image_empty", {{"name", name.empty() ? path : name}});
  }
  const auto size = std::int64_t(info.st_size);
  if (size > kFileSizePremiumLimit || (size > kFileSizeLimit && !premium)) {
    return name + ": " + Tr("lng_file_size_limit1", {{"size", SizeLimitText(premium ? kFileSizePremiumLimit : kFileSizeLimit)}});
  }
  return {};
}

AttachFile PrepareAttachFile(const std::string &path, const std::string &directory) {
  auto file = AttachFile();
  file.path = path;
  file.name = BaseName(path);
  file.mime = MimeOf(path);
  GStatBuf info;
  if (g_stat(path.c_str(), &info) == 0) file.size = info.st_size;
  const auto extension = Extension(file.name);
  if (file.mime == "image/gif") {
    auto width = 0, height = 0;
    if (gdk_pixbuf_get_file_info(path.c_str(), &width, &height) && width > 0 && height > 0) {
      file.type = AttachType::Animation;
      file.width = width;
      file.height = height;
    }
  } else if (file.mime.starts_with("image/")) {
    if (file.size <= kPhotoSizeLimit && PreparePhoto(file, directory)) file.type = AttachType::Photo;
  } else if (file.mime.starts_with("video/") || file.mime.starts_with("audio/") || file.mime == "application/ogg") {
    auto media = MediaInfo();
    if (ReadMediaInfo(path, media)) {
      file.duration = media.duration;
      if (media.video && file.mime.starts_with("video/") && media.width > 0 && media.height > 0) {
        file.type = AttachType::Video;
        file.width = media.width;
        file.height = media.height;
        file.streaming = file.mime == "video/mp4" || file.mime == "video/quicktime";
        PrepareThumbnail(file, directory);
      } else if (media.audio && !media.video) {
        file.type = AttachType::Music;
        file.title = media.title;
        file.performer = media.performer;
        file.ogg = file.mime == "audio/ogg" || file.mime == "application/ogg" || file.mime == "audio/x-opus+ogg"
                   || extension == "ogg" || extension == "oga" || extension == "opus";
      }
    }
  }
  return file;
}

std::string AttachDirectory() {
  auto *directory = g_build_filename(Td::DataDirectory().c_str(), "send", nullptr);
  g_mkdir_with_parents(directory, 0700);
  if (auto *dir = g_dir_open(directory, 0, nullptr)) {
    const auto now = g_get_real_time();
    while (const auto *name = g_dir_read_name(dir)) {
      auto *path = g_build_filename(directory, name, nullptr);
      GStatBuf info;
      if (g_stat(path, &info) == 0 && now - gint64(info.st_mtime) * G_USEC_PER_SEC > kKeepFiles) g_remove(path);
      g_free(path);
    }
    g_dir_close(dir);
  }
  auto result = std::string(directory);
  g_free(directory);
  return result;
}

bool ReadMediaInfo(const std::string &path, MediaInfo &info) {
  auto *format = static_cast<AVFormatContext *>(nullptr);
  if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) return false;
  if (avformat_find_stream_info(format, nullptr) < 0) {
    avformat_close_input(&format);
    return false;
  }
  if (format->duration != AV_NOPTS_VALUE && format->duration > 0) {
    info.duration = int(std::lround(double(format->duration) / AV_TIME_BASE));
  }
  const auto video = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  const auto audio = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  // A cover picture in music is a "video" stream of one frame.
  if (video >= 0 && !(format->streams[video]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
    const auto &stream = *format->streams[video];
    info.video = true;
    info.rotation = Rotation(stream);
    const auto turned = info.rotation == 90 || info.rotation == 270;
    info.width = turned ? stream.codecpar->height : stream.codecpar->width;
    info.height = turned ? stream.codecpar->width : stream.codecpar->height;
  }
  if (audio >= 0) {
    info.audio = true;
    auto *metadata = format->streams[audio]->metadata;
    info.title = Tag(format->metadata, metadata, "title");
    info.performer = Tag(format->metadata, metadata, "artist");
  }
  avformat_close_input(&format);
  return info.video || info.audio;
}

std::vector<AttachGroup> DivideAttachments(std::vector<AttachFile> files, bool compress, bool group) {
  const auto kindOf = [&](const AttachFile &file) {
    if (file.type == AttachType::Music) return file.ogg ? AlbumKind::None : AlbumKind::Music;
    if (!compress) return AlbumKind::File;
    switch (file.type) {
      case AttachType::Photo:
      case AttachType::Video: return AlbumKind::PhotoVideo;
      case AttachType::Animation: return AlbumKind::None;  // A GIF goes on its own.
      default: return AlbumKind::File;
    }
  };
  auto result = std::vector<AttachGroup>();
  auto current = AttachGroup();
  const auto flush = [&] {
    if (current.files.empty()) return;
    if (current.files.size() == 1) current.album = AlbumKind::None;
    result.push_back(std::move(current));
    current = AttachGroup();
  };
  for (auto &file : files) {
    const auto kind = kindOf(file);
    if (!group || kind == AlbumKind::None) {
      flush();
      result.push_back(AttachGroup{.files = {std::move(file)}, .album = AlbumKind::None});
      continue;
    }
    if (current.album != kind || current.files.size() >= std::size_t(kMaxAlbumItems)) flush();
    current.album = kind;
    current.files.push_back(std::move(file));
  }
  flush();
  return result;
}

std::size_t CaptionIndex(const AttachGroup &group) {
  return (group.album == AlbumKind::PhotoVideo || group.files.empty()) ? 0 : group.files.size() - 1;
}

api::object_ptr<api::InputMessageContent> AttachContent(const AttachFile &file, bool compress,
                                                        const std::string &caption) {
  auto text = api::make_object<api::formattedText>(caption, std::vector<api::object_ptr<api::textEntity>>());
  if (file.type == AttachType::Music) {
    auto audio = api::make_object<api::inputAudio>(Local(file.path), nullptr, file.duration, file.title, file.performer);
    return api::make_object<api::inputMessageAudio>(std::move(audio), std::move(text));
  }
  if (compress && file.type == AttachType::Photo && !file.photoPath.empty()) {
    auto photo = api::make_object<api::inputPhoto>(Local(file.photoPath), nullptr, nullptr, std::vector<std::int32_t>(),
                                                   file.width, file.height);
    return api::make_object<api::inputMessagePhoto>(std::move(photo), std::move(text), false, nullptr, false);
  }
  if (compress && file.type == AttachType::Video) {
    auto video = api::make_object<api::inputVideo>(Local(file.path), Thumbnail(file), nullptr, 0,
                                                   std::vector<std::int32_t>(), file.duration, file.width, file.height,
                                                   file.streaming);
    return api::make_object<api::inputMessageVideo>(std::move(video), std::move(text), false, nullptr, false);
  }
  if (compress && file.type == AttachType::Animation) {
    auto animation = api::make_object<api::inputAnimation>(Local(file.path), nullptr, std::vector<std::int32_t>(),
                                                           file.duration, file.width, file.height);
    return api::make_object<api::inputMessageAnimation>(std::move(animation), std::move(text), false, false);
  }
  // The type is decided here: TDLib mustn't make a video or a song of a file sent as a file.
  auto document = api::make_object<api::inputDocument>(Local(file.path), Thumbnail(file), true);
  return api::make_object<api::inputMessageDocument>(std::move(document), std::move(text));
}

int CaptionLength(const std::string &text) {
  auto length = 0;
  for (const auto *p = text.c_str(); *p; p = g_utf8_next_char(p)) {
    length += (g_utf8_get_char(p) >= 0x10000) ? 2 : 1;
  }
  return length;
}

}  // namespace Contrail
