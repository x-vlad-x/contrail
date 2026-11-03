// attach_media.hpp: what a file is sent as, why one is refused, the photo and the video's thumbnail made for it,
// albums of up to ten and their caption, what goes to TDLib.
#include "attach_media.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
}
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Contrail {
namespace {

namespace api = Td::api;

std::string Directory() {
  static auto *dir = g_dir_make_tmp("attach-test-XXXXXX", nullptr);
  g_assert_nonnull(dir);
  return dir;
}

std::string PathOf(const char *name) {
  return Directory() + "/" + name;
}

std::string Picture(const char *name, int width, int height, const char *type) {
  const auto path = PathOf(name);
  auto *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);
  gdk_pixbuf_fill(pixbuf, 0x3080c0ff);
  g_assert_true(gdk_pixbuf_save(pixbuf, path.c_str(), type, nullptr, nullptr));
  g_object_unref(pixbuf);
  return path;
}

std::string Text(const char *name, const std::string &content) {
  const auto path = PathOf(name);
  g_assert_true(g_file_set_contents(path.c_str(), content.data(), gssize(content.size()), nullptr));
  return path;
}

// Two seconds of a tone, 8 kHz mono.
std::string Wave(const char *name) {
  auto data = std::string();
  const auto put = [&](std::uint32_t value, int bytes) {
    for (auto i = 0; i < bytes; ++i) data.push_back(char((value >> (8 * i)) & 0xff));
  };
  const auto samples = 16000u;
  data += "RIFF";
  put(36 + samples * 2, 4);
  data += "WAVEfmt ";
  put(16, 4), put(1, 2), put(1, 2), put(8000, 4), put(16000, 4), put(2, 2), put(16, 2);
  data += "data";
  put(samples * 2, 4);
  for (auto i = 0u; i < samples; ++i) put(std::uint16_t(i % 40 < 20 ? 4000 : -4000), 2);
  return Text(name, data);
}

// A second of 160x96 MPEG-4 video in MP4, its matrix turning it a quarter clockwise as a phone held upright does.
std::string Video(const char *name) {
  const auto path = PathOf(name);
  auto *format = static_cast<AVFormatContext *>(nullptr);
  g_assert_cmpint(avformat_alloc_output_context2(&format, nullptr, "mp4", path.c_str()), >=, 0);
  const auto *encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
  g_assert_nonnull(encoder);
  auto *stream = avformat_new_stream(format, nullptr);
  auto *codec = avcodec_alloc_context3(encoder);
  codec->width = 160;
  codec->height = 96;
  codec->pix_fmt = AV_PIX_FMT_YUV420P;
  codec->time_base = {1, 10};
  codec->framerate = {10, 1};
  if (format->oformat->flags & AVFMT_GLOBALHEADER) codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  g_assert_cmpint(avcodec_open2(codec, encoder, nullptr), >=, 0);
  g_assert_cmpint(avcodec_parameters_from_context(stream->codecpar, codec), >=, 0);
  stream->time_base = codec->time_base;
  auto *side = av_packet_side_data_new(&stream->codecpar->coded_side_data, &stream->codecpar->nb_coded_side_data,
                                       AV_PKT_DATA_DISPLAYMATRIX, 9 * sizeof(std::int32_t), 0);
  g_assert_nonnull(side);
  // The matrix turns counterclockwise: -90 is a quarter clockwise.
  av_display_rotation_set(reinterpret_cast<std::int32_t *>(side->data), -90);
  g_assert_cmpint(avio_open(&format->pb, path.c_str(), AVIO_FLAG_WRITE), >=, 0);
  g_assert_cmpint(avformat_write_header(format, nullptr), >=, 0);
  auto *frame = av_frame_alloc();
  frame->format = codec->pix_fmt;
  frame->width = codec->width;
  frame->height = codec->height;
  g_assert_cmpint(av_frame_get_buffer(frame, 0), >=, 0);
  auto *packet = av_packet_alloc();
  const auto drain = [&] {
    while (avcodec_receive_packet(codec, packet) == 0) {
      av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
      packet->stream_index = stream->index;
      g_assert_cmpint(av_interleaved_write_frame(format, packet), >=, 0);
    }
  };
  for (auto i = 0; i < 10; ++i) {
    g_assert_cmpint(av_frame_make_writable(frame), >=, 0);
    for (auto y = 0; y < codec->height; ++y) {
      for (auto x = 0; x < codec->width; ++x) frame->data[0][y * frame->linesize[0] + x] = std::uint8_t(x + i * 3);
    }
    for (auto y = 0; y < codec->height / 2; ++y) {
      for (auto x = 0; x < codec->width / 2; ++x) {
        frame->data[1][y * frame->linesize[1] + x] = 128;
        frame->data[2][y * frame->linesize[2] + x] = 128;
      }
    }
    frame->pts = i;
    g_assert_cmpint(avcodec_send_frame(codec, frame), >=, 0);
    drain();
  }
  avcodec_send_frame(codec, nullptr);
  drain();
  g_assert_cmpint(av_write_trailer(format), >=, 0);
  av_packet_free(&packet);
  av_frame_free(&frame);
  avcodec_free_context(&codec);
  avio_closep(&format->pb);
  avformat_free_context(format);
  return path;
}

AttachFile Of(AttachType type, const char *name = "f", bool ogg = false) {
  auto file = AttachFile();
  file.type = type;
  file.name = name;
  file.path = PathOf(name);
  file.ogg = ogg;
  return file;
}

void TestRefusal() {
  g_assert_cmpstr(AttachRefusal(Text("note.txt", "hello"), false).c_str(), ==, "");
  const auto empty = AttachRefusal(Text("empty.txt", ""), false);
  g_assert_true(empty.find("empty.txt") != std::string::npos && empty.find("empty") != std::string::npos);
  g_assert_cmpstr(AttachRefusal(Directory(), false).c_str(), !=, "");
  g_assert_cmpstr(AttachRefusal(PathOf("missing.txt"), false).c_str(), !=, "");
}

void TestPhoto() {
  // Larger than Telegram's photos: made smaller as a JPEG, the proportions kept.
  const auto large = PrepareAttachFile(Picture("large.png", 3200, 1600, "png"), Directory());
  g_assert_cmpint(int(large.type), ==, int(AttachType::Photo));
  g_assert_cmpint(large.width, ==, 2560);
  g_assert_cmpint(large.height, ==, 1280);
  g_assert_cmpstr(large.mime.c_str(), ==, "image/png");
  auto width = 0, height = 0;
  g_assert_nonnull(gdk_pixbuf_get_file_info(large.photoPath.c_str(), &width, &height));
  g_assert_cmpint(width, ==, 2560);
  g_assert_cmpint(height, ==, 1280);
  // Small ones keep their size.
  const auto small = PrepareAttachFile(Picture("small.jpg", 640, 480, "jpeg"), Directory());
  g_assert_cmpint(int(small.type), ==, int(AttachType::Photo));
  g_assert_cmpint(small.width, ==, 640);
  g_assert_cmpint(small.height, ==, 480);
  // Narrower than 1:20 is a file, as Telegram Desktop decides.
  const auto narrow = PrepareAttachFile(Picture("narrow.png", 2100, 100, "png"), Directory());
  g_assert_cmpint(int(narrow.type), ==, int(AttachType::File));
  g_assert_true(narrow.photoPath.empty());
}

void TestOthers() {
  const auto music = PrepareAttachFile(Wave("tone.wav"), Directory());
  g_assert_cmpint(int(music.type), ==, int(AttachType::Music));
  g_assert_cmpint(music.duration, ==, 2);
  g_assert_false(music.ogg);
  const auto text = PrepareAttachFile(Text("readme.md", "# Notes\n"), Directory());
  g_assert_cmpint(int(text.type), ==, int(AttachType::File));
  g_assert_cmpstr(text.name.c_str(), ==, "readme.md");
  g_assert_cmpint(int(text.size), ==, 8);
}

void TestVideo() {
  const auto video = PrepareAttachFile(Video("clip.mp4"), Directory());
  g_assert_cmpint(int(video.type), ==, int(AttachType::Video));
  g_assert_cmpstr(video.mime.c_str(), ==, "video/mp4");
  g_assert_true(video.streaming);
  g_assert_cmpint(video.duration, ==, 1);
  // Upright: the matrix turns it.
  g_assert_cmpint(video.width, ==, 96);
  g_assert_cmpint(video.height, ==, 160);
  g_assert_false(video.thumbnailPath.empty());
  g_assert_cmpint(video.thumbnailWidth, ==, 96);
  g_assert_cmpint(video.thumbnailHeight, ==, 160);
}

void TestAlbums() {
  // Twelve photos and videos: ten and two.
  auto media = std::vector<AttachFile>();
  for (auto i = 0; i < 12; ++i) media.push_back(Of(i % 3 ? AttachType::Photo : AttachType::Video));
  auto groups = DivideAttachments(media, true, true);
  g_assert_cmpuint(groups.size(), ==, 2);
  g_assert_cmpuint(groups[0].files.size(), ==, 10);
  g_assert_cmpint(int(groups[0].album), ==, int(AlbumKind::PhotoVideo));
  g_assert_cmpuint(groups[1].files.size(), ==, 2);
  // A file between photos breaks the album; one left alone is no album.
  groups = DivideAttachments({Of(AttachType::Photo), Of(AttachType::File), Of(AttachType::Photo), Of(AttachType::Photo)},
                             true, true);
  g_assert_cmpuint(groups.size(), ==, 3);
  g_assert_cmpint(int(groups[0].album), ==, int(AlbumKind::None));
  g_assert_cmpint(int(groups[1].album), ==, int(AlbumKind::None));
  g_assert_cmpint(int(groups[2].album), ==, int(AlbumKind::PhotoVideo));
  // As documents, photos and files go together.
  groups = DivideAttachments({Of(AttachType::Photo), Of(AttachType::File), Of(AttachType::Video)}, false, true);
  g_assert_cmpuint(groups.size(), ==, 1);
  g_assert_cmpint(int(groups[0].album), ==, int(AlbumKind::File));
  // Not grouped: one by one, in order.
  groups = DivideAttachments({Of(AttachType::Photo, "a"), Of(AttachType::Photo, "b")}, true, false);
  g_assert_cmpuint(groups.size(), ==, 2);
  g_assert_cmpstr(groups[1].files[0].name.c_str(), ==, "b");
  // Music goes in albums of its own; Ogg on its own; a GIF on its own.
  groups = DivideAttachments({Of(AttachType::Music), Of(AttachType::Music), Of(AttachType::Music, "o", true),
                              Of(AttachType::Music), Of(AttachType::Animation), Of(AttachType::Photo)},
                             true, true);
  g_assert_cmpuint(groups.size(), ==, 5);
  g_assert_cmpint(int(groups[0].album), ==, int(AlbumKind::Music));
  g_assert_cmpuint(groups[0].files.size(), ==, 2);
  g_assert_cmpstr(groups[1].files[0].name.c_str(), ==, "o");
  g_assert_cmpint(int(groups[2].album), ==, int(AlbumKind::None));
  g_assert_cmpint(int(groups[3].files[0].type), ==, int(AttachType::Animation));
  // The caption: on the first photo of an album, on the last of other albums.
  g_assert_cmpuint(CaptionIndex(AttachGroup{.files = {Of(AttachType::Photo), Of(AttachType::Photo)}, .album = AlbumKind::PhotoVideo}),
                   ==, 0);
  g_assert_cmpuint(CaptionIndex(AttachGroup{.files = {Of(AttachType::File), Of(AttachType::File)}, .album = AlbumKind::File}),
                   ==, 1);
}

void TestContent() {
  auto photo = Of(AttachType::Photo);
  photo.photoPath = PathOf("prepared.jpg");
  photo.width = 800;
  photo.height = 600;
  auto content = AttachContent(photo, true, "Caption");
  g_assert_cmpint(content->get_id(), ==, api::inputMessagePhoto::ID);
  const auto &sent = static_cast<api::inputMessagePhoto &>(*content);
  g_assert_cmpstr(static_cast<const api::inputFileLocal &>(*sent.photo_->photo_).path_.c_str(), ==, photo.photoPath.c_str());
  g_assert_cmpstr(sent.caption_->text_.c_str(), ==, "Caption");
  // As documents, the file itself, and TDLib mustn't make it a photo again.
  content = AttachContent(photo, false, "");
  g_assert_cmpint(content->get_id(), ==, api::inputMessageDocument::ID);
  const auto &document = static_cast<api::inputMessageDocument &>(*content);
  g_assert_true(document.document_->disable_content_type_detection_);
  g_assert_cmpstr(static_cast<const api::inputFileLocal &>(*document.document_->document_).path_.c_str(), ==, photo.path.c_str());
  // Music is music either way.
  g_assert_cmpint(AttachContent(Of(AttachType::Music), false, "")->get_id(), ==, api::inputMessageAudio::ID);
  auto video = Of(AttachType::Video);
  video.thumbnailPath = PathOf("thumbnail.jpg");
  content = AttachContent(video, true, "");
  g_assert_cmpint(content->get_id(), ==, api::inputMessageVideo::ID);
  g_assert_nonnull(static_cast<api::inputMessageVideo &>(*content).video_->thumbnail_.get());
  g_assert_cmpint(AttachContent(Of(AttachType::Animation), true, "")->get_id(), ==, api::inputMessageAnimation::ID);
}

void TestCaptionLength() {
  g_assert_cmpint(CaptionLength("abc"), ==, 3);
  g_assert_cmpint(CaptionLength("Привет"), ==, 6);
  // Outside the basic plane: two UTF-16 units, as Telegram counts.
  g_assert_cmpint(CaptionLength("a\xF0\x9F\x98\x80"), ==, 3);
}

}  // namespace
}  // namespace Contrail

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, nullptr);
  g_test_add_func("/attach/refusal", Contrail::TestRefusal);
  g_test_add_func("/attach/photo", Contrail::TestPhoto);
  g_test_add_func("/attach/others", Contrail::TestOthers);
  g_test_add_func("/attach/video", Contrail::TestVideo);
  g_test_add_func("/attach/albums", Contrail::TestAlbums);
  g_test_add_func("/attach/content", Contrail::TestContent);
  g_test_add_func("/attach/caption-length", Contrail::TestCaptionLength);
  return g_test_run();
}
