#include "call_media.hpp"

#include <glib.h>

namespace Contrail::Calls {
namespace {

struct ScenarioState {
  Callbacks callbacks;
  std::string camera;
  bool active = true;
  bool connected = false;
  bool muted = false;
  bool remoteVideo = false;

  Frame frame(bool local) const {
    if (!local && !remoteVideo) return {};
    constexpr auto width = 160, height = 240;
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(width * height * 4);
    for (auto y = 0; y != height; ++y) for (auto x = 0; x != width; ++x) {
      const auto offset = (y * width + x) * 4;
      (*pixels)[offset] = std::uint8_t(x);
      (*pixels)[offset + 1] = std::uint8_t(y);
      (*pixels)[offset + 2] = local ? (camera == "scenario-back" ? 200 : 100) : 40;
      (*pixels)[offset + 3] = 255;
    }
    return {.width = width, .height = height, .stride = width * 4, .pixels = std::move(pixels)};
  }
};

class ScenarioMedia final : public Media {
 public:
  ScenarioMedia(Parameters parameters, Callbacks callbacks) : _state(std::make_shared<ScenarioState>()) {
    _state->callbacks = std::move(callbacks);
    _state->camera = std::move(parameters.camera);
    _state->remoteVideo = parameters.video;
    if (parameters.video && _state->camera.empty()) _state->camera = "scenario-front";
    _source = g_timeout_add_full(G_PRIORITY_DEFAULT, 100, +[](gpointer data) -> gboolean {
      // A callback may end the call and destroy its Media, including the source that invoked it.
      const auto self = *static_cast<std::shared_ptr<ScenarioState> *>(data);
      if (!self->active) return G_SOURCE_REMOVE;
      if (!self->connected) {
        self->connected = true;
        if (self->callbacks.state) self->callbacks.state(State::Established);
        if (!self->active) return G_SOURCE_REMOVE;
        if (self->callbacks.signalBars) self->callbacks.signalBars(4);
        if (!self->active) return G_SOURCE_REMOVE;
        if (self->callbacks.signaling) self->callbacks.signaling("scenario-media");
      }
      if (!self->active) return G_SOURCE_REMOVE;
      if (self->callbacks.remoteFrame) self->callbacks.remoteFrame(self->frame(false));
      if (!self->active) return G_SOURCE_REMOVE;
      if (!self->camera.empty() && self->callbacks.localFrame) self->callbacks.localFrame(self->frame(true));
      return G_SOURCE_CONTINUE;
    }, new std::shared_ptr<ScenarioState>(_state), +[](gpointer data) {
      delete static_cast<std::shared_ptr<ScenarioState> *>(data);
    });
  }
  ~ScenarioMedia() override { stop(); }
  void receiveSignaling(const std::string &data) override {
    const auto self = _state;
    if (data == "reconnecting" && self->callbacks.state) self->callbacks.state(State::Reconnecting);
    else if (data == "established" && self->callbacks.state) self->callbacks.state(State::Established);
    else if (data == "failed" && self->callbacks.state) self->callbacks.state(State::Failed);
    else if (data == "camera-failed" && self->callbacks.cameraFailed) self->callbacks.cameraFailed();
    else if (data == "remote-muted" && self->callbacks.remoteMedia) self->callbacks.remoteMedia(true, self->remoteVideo);
    else if (data == "remote-video-off" && self->callbacks.videoStopped) {
      self->remoteVideo = false;
      self->callbacks.videoStopped();
    }
  }
  void setMuted(bool muted) override { _state->muted = muted; }
  void setOutputDevice(const std::string &) override {}
  void setCamera(const std::string &id) override { _state->camera = id; }
  std::int64_t preferredRelay() override { return 42; }
  std::string lastError() override { return {}; }
  std::string debugInfo() override { return "{\"scenario\":true}"; }
  std::string soundStats() override { return "scenario"; }
  void stop() override { _state->active = false; if (_source) g_source_remove(_source); _source = 0; }
 private:
  std::shared_ptr<ScenarioState> _state;
  guint _source = 0;
};
}  // namespace

std::unique_ptr<Media> MakeScenarioMedia(Parameters parameters, Callbacks callbacks) {
  return std::make_unique<ScenarioMedia>(std::move(parameters), std::move(callbacks));
}
}  // namespace Contrail::Calls
