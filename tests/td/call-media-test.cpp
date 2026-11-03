#include "call_media.hpp"
#include "video_frame.hpp"
#include "call_tones_decode.hpp"
#include "media/openal_devices.hpp"

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

void Tones() {
  using Tone = Calls::Tone;
  for (const auto tone : {Tone::Incoming, Tone::Outgoing, Tone::Connecting, Tone::Ended, Tone::Busy}) {
    const auto pcm = Calls::DecodeCallTone(tone);
    g_assert_cmpuint(pcm.size(), >, Calls::kToneSampleRate / 5);
    g_assert_cmpuint(pcm.size(), <, Calls::kToneSampleRate * 2 * 30);
    g_assert_cmpuint(pcm.size() % 2, ==, 0);
    g_assert_cmpint(*std::max_element(pcm.begin(), pcm.end()), >, 100);
  }
  std::puts("call tones: PASS");
}

void WaitTone(const std::function<bool()> &condition) {
  auto *loop = g_main_loop_new(nullptr, FALSE);
  auto pending = std::pair(loop, &condition);
  const auto poll = g_timeout_add(1, +[](gpointer data) -> gboolean {
    auto &pending = *static_cast<std::pair<GMainLoop *, const std::function<bool()> *> *>(data);
    if ((*pending.second)()) g_main_loop_quit(pending.first);
    return G_SOURCE_CONTINUE;
  }, &pending);
  const auto timeout = g_timeout_add_seconds(6, +[](gpointer) -> gboolean {
    g_error("The call tone did not finish"); return G_SOURCE_REMOVE;
  }, nullptr);
  g_main_loop_run(loop);
  g_source_remove(poll);
  g_source_remove(timeout);
  g_main_loop_unref(loop);
}

int TonePlayback() {
  using Tone = Calls::Tone;
  auto player = Calls::MakeTonePlayer();
  g_assert_nonnull(player.get());
  player->play(Tone::Ended);
  WaitTone([&] { return player->current() == Tone::Ended; });
  WaitTone([&] { return player->current() == Tone::None; });
  player->play(Tone::Outgoing, true);
  WaitTone([&] { return player->current() == Tone::Outgoing; });
  player->stop();
  WaitTone([&] { return player->current() == Tone::None; });
  std::puts("tone playback: PASS");
  return 0;
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
    if (const auto output = std::getenv("CONTRAIL_CALL_OUTPUT")) {
      const auto id = ::media::OpenAlOutputId(output);
      g_assert_false(id.empty());
      std::printf("output %s -> %s\n", output, id.c_str());
      media[side]->setOutputDevice(output);
    }
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
  for (auto &peer : media) {
    const auto stats = peer->soundStats();
    std::puts(stats.c_str());
    if (std::getenv("CONTRAIL_CALL_OUTPUT") && (stats.find("played=") == std::string::npos
        || stats.find("played=0 ") != std::string::npos)) failed = true;
    peer->stop(); peer.reset();
  }
  Calls::WaitForStoppedMedia();
  g_main_loop_unref(loop);
  std::printf("frames received=%d,%d local=%d\n", frames[0], frames[1], localFrames);
  if (video && (!frames[1] || !localFrames)) failed = true;
  if (switchCamera && (framesBeforeSwitch < 0 || frames[1] <= framesBeforeSwitch)) failed = true;
  std::printf("local pair %s: %s\n", version.c_str(), connected[0] && connected[1] && !failed ? "PASS" : "FAIL");
  std::fflush(stdout);
  return connected[0] && connected[1] && !failed ? 0 : 1;
}

int main(int argc, char **argv) {
  const auto versions = Calls::Versions();
  g_assert_false(versions.empty());
  g_assert_true(std::find(versions.begin(), versions.end(), "9.0.0") != versions.end());
  g_assert_true(std::find(versions.begin(), versions.end(), "13.0.0") != versions.end());
  Conversion();
  Tones();
  if (argc > 1 && std::string(argv[1]) == "tone") return TonePlayback();
  return argc > 1 ? Pair(argv[1], argc > 2 ? std::max(1, std::atoi(argv[2])) : 1, argc > 3, argc > 4) : 0;
}
