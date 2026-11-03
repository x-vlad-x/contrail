#include "call_media.hpp"
#include "video_frame.hpp"

#include <api/video/i420_buffer.h>
#include <api/video/video_frame.h>
#include <glib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>

using namespace Contrail;

void Conversion() {
  auto source = webrtc::I420Buffer::Create(4, 2);
  const std::uint8_t values[] = {16, 48, 96, 128, 144, 176, 208, 235};
  for (auto y = 0; y < 2; ++y) std::memcpy(source->MutableDataY() + y * source->StrideY(), values + 4 * y, 4);
  std::memset(source->MutableDataU(), 128, source->StrideU());
  std::memset(source->MutableDataV(), 128, source->StrideV());
  const auto make = [&](webrtc::VideoRotation rotation) {
    return webrtc::VideoFrame::Builder().set_video_frame_buffer(source).set_rotation(rotation).build();
  };
  const auto normal = ConvertCallVideoFrame(make(webrtc::kVideoRotation_0), false);
  const auto mirror = ConvertCallVideoFrame(make(webrtc::kVideoRotation_0), true);
  g_assert_cmpint(mirror.width, ==, 4);
  const auto pixel = [](const Calls::Frame &frame, int x, int y) { return frame.pixels->data() + y * frame.stride + x * 4; };
  for (auto y = 0; y < 2; ++y) for (auto x = 0; x < 4; ++x) {
    g_assert_true(std::memcmp(pixel(normal, x, y), pixel(mirror, 3 - x, y), 4) == 0);
    g_assert_cmpint(pixel(normal, x, y)[3], ==, 255);
  }
  const auto rotated = ConvertCallVideoFrame(make(webrtc::kVideoRotation_90), false);
  g_assert_cmpint(rotated.width, ==, 2);
  g_assert_cmpint(rotated.height, ==, 4);
  for (auto y = 0; y < 2; ++y) for (auto x = 0; x < 4; ++x) {
    g_assert_true(std::memcmp(pixel(normal, x, y), pixel(rotated, 1 - y, x), 4) == 0);
  }
  std::puts("video conversion: PASS");
}

int Pair(const std::string &version, int seconds, bool video, bool switchCamera) {
  auto media = std::array<std::unique_ptr<Calls::Media>, 2>();
  auto connected = std::array<bool, 2>{};
  auto failed = false;
  auto frames = std::array<int, 2>{};
  auto localFrames = 0;
  guint finishTimer = 0;
  guint switchTimer = 0;
  int framesBeforeSwitch = -1;
  const auto cameras = video ? Calls::Cameras() : std::vector<Calls::Camera>();
  if (video) g_assert_false(cameras.empty());
  if (switchCamera) g_assert_cmpuint(cameras.size(), >=, 2);
  auto *loop = g_main_loop_new(nullptr, FALSE);
  struct Switch { std::unique_ptr<Calls::Media> *media; const std::string *camera; int *before; int *frames; guint *timer; };
  auto change = Switch{&media[0], cameras.size() > 1 ? &cameras[1].id : nullptr, &framesBeforeSwitch, &frames[1], &switchTimer};
  for (auto side = 0; side != 2; ++side) {
    auto parameters = Calls::Parameters{.version = version, .key = std::string(256, 'k'), .outgoing = side == 0,
        .allowP2p = true, .maxLayer = Calls::MaxLayer(), .video = video && side == 0,
        .camera = video && side == 0 ? cameras.front().id : std::string()};
    media[side] = Calls::StartMedia(std::move(parameters), {
        .state = [&, side](Calls::State state) {
          std::printf("peer %d state %d\n", side, int(state));
          if (state == Calls::State::Established) connected[side] = true;
          if (state == Calls::State::Failed) failed = true;
          if (failed) g_main_loop_quit(loop);
          else if (connected[0] && connected[1] && !finishTimer) {
            if (switchCamera) switchTimer = g_timeout_add_seconds(std::max(1, seconds / 2), +[](gpointer data) -> gboolean {
              auto &change = *static_cast<Switch *>(data);
              *change.before = *change.frames;
              *change.timer = 0;
              (*change.media)->setCamera(*change.camera);
              return G_SOURCE_REMOVE;
            }, &change);
            finishTimer = g_timeout_add_seconds(seconds, +[](gpointer pointer) -> gboolean {
              g_main_loop_quit(static_cast<GMainLoop *>(pointer)); return G_SOURCE_CONTINUE;
            }, loop);
          }
        },
        .signaling = [&, side](std::string data) {
          if (media[1 - side]) media[1 - side]->receiveSignaling(data);
        },
        .remoteFrame = [&, side](Calls::Frame frame) { if (frame.pixels) ++frames[side]; },
        .localFrame = [&](Calls::Frame frame) { if (frame.pixels) ++localFrames; },
        .cameraFailed = [&] { failed = true; g_main_loop_quit(loop); }});
    g_assert_nonnull(media[side].get());
    media[side]->setMuted(true);
  }
  auto watch = std::pair(loop, &finishTimer);
  const auto timer = g_timeout_add_seconds(15, +[](gpointer pointer) -> gboolean {
    auto &watch = *static_cast<std::pair<GMainLoop *, guint *> *>(pointer);
    if (!*watch.second) g_main_loop_quit(watch.first);
    return G_SOURCE_CONTINUE;
  }, &watch);
  g_main_loop_run(loop);
  g_source_remove(timer);
  if (finishTimer) g_source_remove(finishTimer);
  if (switchTimer) g_source_remove(switchTimer);
  for (auto &peer : media) { std::puts(peer->soundStats().c_str()); peer->stop(); peer.reset(); }
  Calls::WaitForStoppedMedia();
  g_main_loop_unref(loop);
  std::printf("frames received=%d,%d local=%d\n", frames[0], frames[1], localFrames);
  if (video && (!frames[1] || !localFrames)) failed = true;
  if (switchCamera && (framesBeforeSwitch < 0 || frames[1] <= framesBeforeSwitch)) failed = true;
  std::printf("local pair %s: %s\n", version.c_str(), connected[0] && connected[1] && !failed ? "PASS" : "FAIL");
  return connected[0] && connected[1] && !failed ? 0 : 1;
}

int main(int argc, char **argv) {
  const auto versions = Calls::Versions();
  g_assert_false(versions.empty());
  g_assert_true(std::find(versions.begin(), versions.end(), "9.0.0") != versions.end());
  g_assert_true(std::find(versions.begin(), versions.end(), "13.0.0") != versions.end());
  Conversion();
  return argc > 1 ? Pair(argv[1], argc > 2 ? std::max(1, std::atoi(argv[2])) : 1, argc > 3, argc > 4) : 0;
}
