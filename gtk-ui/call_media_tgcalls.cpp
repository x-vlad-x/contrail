#include "call_media.hpp"

#include "media/openal_adm.hpp"
#include "media/openal_devices.hpp"
#include "video_frame.hpp"

#include <tgcalls/InstanceImpl.h>
#include <tgcalls/StaticThreads.h>
#include <tgcalls/VideoCaptureInterface.h>
#include <tgcalls/v2/InstanceV2Impl.h>
#include <tgcalls/v2/InstanceV2ReferenceImpl.h>
#include <api/video/video_sink_interface.h>
#include <modules/video_capture/video_capture_factory.h>
#include <modules/video_capture/video_capture_options.h>
#include <rtc_base/ref_counted_object.h>
#include <rtc_base/ssl_adapter.h>

#include <glib.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

namespace Contrail::Calls {
namespace {

const auto kRegisterV1 = tgcalls::Register<tgcalls::InstanceImpl>();
const auto kRegisterV2 = tgcalls::Register<tgcalls::InstanceV2Impl>();
const auto kRegisterV2Reference = tgcalls::Register<tgcalls::InstanceV2ReferenceImpl>();

void Post(std::function<void()> callback) {
  g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer data) -> gboolean {
    (*static_cast<std::function<void()> *>(data))();
    return G_SOURCE_REMOVE;
  }, new std::function<void()>(std::move(callback)),
      +[](gpointer data) { delete static_cast<std::function<void()> *>(data); });
}

struct Shared {
  std::atomic<bool> active = true;
  Callbacks callbacks;
  std::mutex soundMutex;
  webrtc::scoped_refptr<media::OpenAlAudioDevice> sound;
  std::string outputDevice;
};

template <typename Callback>
void Notify(const std::shared_ptr<Shared> &shared, Callback callback) {
  Post([shared, callback = std::move(callback)] {
    if (shared->active) callback(shared->callbacks);
  });
}

// The decoder may outpace GTK, especially while the chat scrolls. Each sink holds at most one frame.
class VideoSink final : public rtc::VideoSinkInterface<webrtc::VideoFrame>,
                        public std::enable_shared_from_this<VideoSink> {
 public:
  VideoSink(std::shared_ptr<Shared> shared, bool local) : _shared(std::move(shared)), _local(local) {}
  void setMirrored(bool mirror) { _mirror = mirror; }
  void setEnabled(bool enabled) { _enabled = enabled; ++_generation; }
  void OnFrame(const webrtc::VideoFrame &frame) override {
    if (!_shared->active || !_enabled || _pending.exchange(true)) return;
    const auto generation = _generation.load();
    auto picture = ConvertCallVideoFrame(frame, _mirror);
    Post([self = shared_from_this(), generation, picture = std::move(picture)]() mutable {
      self->_pending = false;
      if (!self->_shared->active || !self->_enabled || self->_generation != generation) return;
      const auto &callback = self->_local ? self->_shared->callbacks.localFrame : self->_shared->callbacks.remoteFrame;
      if (callback) callback(std::move(picture));
    });
  }
 private:
  std::shared_ptr<Shared> _shared;
  bool _local = false;
  std::atomic<bool> _mirror = false;
  std::atomic<bool> _pending = false;
  std::atomic<bool> _enabled = true;
  std::atomic<unsigned> _generation = 0;
};

std::string Hex(const std::string &bytes) {
  constexpr char digits[] = "0123456789abcdef";
  auto result = std::string();
  result.reserve(bytes.size() * 2);
  for (const auto value : bytes) {
    const auto byte = static_cast<unsigned char>(value);
    result.push_back(digits[byte >> 4]); result.push_back(digits[byte & 15]);
  }
  return result;
}

std::string Lower(const std::string &text) {
  auto *lower = g_ascii_strdown(text.c_str(), -1);
  auto result = std::string(lower);
  g_free(lower);
  return result;
}

bool PhoneCamera(const Camera &camera) {
  const auto name = Lower(camera.name);
  return name.find("front") != std::string::npos || name.find("back") != std::string::npos
      || name.find("rear") != std::string::npos;
}

// Stopping returns before WebRTC destroys its transports. Retain the wrapper until its final callback, then
// destroy it on the main loop; static thread pools must stay alive until that asynchronous teardown settles.
std::atomic<int> kStopping = 0;
std::chrono::steady_clock::time_point kLastStopped;

class TgcallsMedia final : public Media {
 public:
  TgcallsMedia() : _shared(std::make_shared<Shared>()) {}
  ~TgcallsMedia() override { stop(); }
  bool start(Parameters parameters, Callbacks callbacks) {
    _shared->callbacks = std::move(callbacks);
    _incoming = std::make_shared<VideoSink>(_shared, false);
    _outgoing = std::make_shared<VideoSink>(_shared, true);
    if (parameters.key.size() != tgcalls::EncryptionKey::kSize) return false;
    auto key = std::make_shared<std::array<std::uint8_t, tgcalls::EncryptionKey::kSize>>();
    std::memcpy(key->data(), parameters.key.data(), key->size());
    auto descriptor = tgcalls::Descriptor{
        .version = parameters.version,
        .config = {.initializationTimeout = parameters.connectTimeout, .receiveTimeout = parameters.packetTimeout,
            .enableP2P = parameters.allowP2p && !g_getenv("CONTRAIL_CALL_RELAY"), .allowTCP = true,
            .enableAEC = false, .enableNS = true, .enableAGC = true, .enableVolumeControl = true,
            .statsLogPath = {.data = parameters.statsLogPath}, .maxApiLayer = parameters.maxLayer,
            .customParameters = parameters.customParameters},
        .encryptionKey = tgcalls::EncryptionKey(key, parameters.outgoing),
        .mediaDevicesConfig = {.inputVolume = 1.f, .outputVolume = 1.f},
        .stateUpdated = [shared = _shared](tgcalls::State state) {
          const auto value = state == tgcalls::State::Established ? State::Established
              : state == tgcalls::State::Reconnecting ? State::Reconnecting
              : state == tgcalls::State::Failed ? State::Failed : State::Connecting;
          Notify(shared, [value](const Callbacks &callbacks) { if (callbacks.state) callbacks.state(value); });
        },
        .signalBarsUpdated = [shared = _shared](int bars) {
          Notify(shared, [bars](const Callbacks &callbacks) { if (callbacks.signalBars) callbacks.signalBars(bars); });
        },
        .remoteMediaStateUpdated = [shared = _shared, sink = _incoming](tgcalls::AudioState audio, tgcalls::VideoState video) {
          sink->setEnabled(video == tgcalls::VideoState::Active);
          Notify(shared, [audio, video](const Callbacks &callbacks) {
            if (callbacks.remoteMedia) callbacks.remoteMedia(audio == tgcalls::AudioState::Muted,
                video == tgcalls::VideoState::Active);
            if (video != tgcalls::VideoState::Active && callbacks.videoStopped) callbacks.videoStopped();
          });
        },
        .signalingDataEmitted = [shared = _shared](const std::vector<std::uint8_t> &data) {
          Notify(shared, [bytes = std::string(data.begin(), data.end())](const Callbacks &callbacks) {
            if (callbacks.signaling) callbacks.signaling(bytes);
          });
        },
        .createAudioDeviceModule = [shared = _shared](webrtc::TaskQueueFactory *factory) {
          auto device = rtc::make_ref_counted<media::OpenAlAudioDevice>(factory);
          {
            const auto lock = std::lock_guard(shared->soundMutex);
            device->setPlayoutDeviceId(shared->outputDevice);
            shared->sound = device;
          }
          return webrtc::scoped_refptr<webrtc::AudioDeviceModule>(device);
        }};
    auto ids = std::set<std::int64_t>();
    for (const auto &server : parameters.servers) if (server.reflector) ids.insert(server.id);
    for (const auto &server : parameters.servers) {
      if (server.port <= 0 || server.port > 65535) continue;
      const auto port = static_cast<std::uint16_t>(server.port);
      if (server.reflector) {
        if (server.peerTag.size() != 16) continue;
        if (!server.tcp) {
          auto endpoint = tgcalls::Endpoint{.endpointId = server.id, .host = {server.ipv4, server.ipv6},
              .port = port, .type = tgcalls::EndpointType::UdpRelay};
          std::memcpy(endpoint.peerTag, server.peerTag.data(), 16);
          descriptor.endpoints.push_back(std::move(endpoint));
        }
        const auto id = static_cast<std::uint8_t>(std::distance(ids.begin(), ids.find(server.id)) + 1);
        for (const auto &host : {server.ipv4, server.ipv6}) if (!host.empty()) {
          descriptor.rtcServers.push_back({.id = id, .host = host, .port = port, .login = "reflector",
              .password = Hex(server.peerTag), .isTurn = true, .isTcp = server.tcp});
        }
      } else for (const auto &host : {server.ipv4, server.ipv6}) if (!host.empty()) {
        if (server.stun) descriptor.rtcServers.push_back({.host = host, .port = port});
        if (server.turn && !server.username.empty() && !server.password.empty()) {
          descriptor.rtcServers.push_back({.host = host, .port = port, .login = server.username,
              .password = server.password, .isTurn = true});
        }
      }
    }
    if (!parameters.proxy.host.empty() && parameters.proxy.port > 0 && parameters.proxy.port <= 65535) {
      descriptor.proxy = std::make_unique<tgcalls::Proxy>(tgcalls::Proxy{.host = parameters.proxy.host,
          .port = static_cast<std::uint16_t>(parameters.proxy.port), .login = parameters.proxy.username,
          .password = parameters.proxy.password});
    }
    _instance = tgcalls::Meta::Create(parameters.version, std::move(descriptor));
    if (!_instance) return false;
    _instance->setIncomingVideoOutput(_incoming);
    if (parameters.video && !parameters.camera.empty()) setCamera(parameters.camera);
    return true;
  }
  void receiveSignaling(const std::string &data) override {
    if (_instance) _instance->receiveSignalingData(std::vector<std::uint8_t>(data.begin(), data.end()));
  }
  void setMuted(bool muted) override { if (_instance) _instance->setMuteMicrophone(muted); }
  void setOutputDevice(const std::string &description) override {
    const auto device = media::OpenAlOutputId(description);
    {
      const auto lock = std::lock_guard(_shared->soundMutex);
      _shared->outputDevice = device;
      if (_shared->sound) _shared->sound->setPlayoutDeviceId(device);
    }
    if (_instance) _instance->setAudioOutputDevice(device);
  }
  void setCamera(const std::string &id) override {
    if (!_instance) return;
    _outgoing->setEnabled(!id.empty());
    if (id.empty()) {
      if (_capture) _capture->setState(tgcalls::VideoState::Inactive);
      _instance->setVideoCapture(nullptr);
      _capture.reset();
      return;
    }
    const auto cameras = Cameras();
    const auto camera = std::find_if(cameras.begin(), cameras.end(), [&](const auto &camera) { return camera.id == id; });
    _outgoing->setMirrored(camera != cameras.end() && Lower(camera->name).find("front") != std::string::npos);
    if (!_capture) {
      _capture = tgcalls::VideoCaptureInterface::Create(tgcalls::Threads::getThreads(), id);
      _capture->setOutput(_outgoing);
      _capture->setOnFatalError([shared = _shared, sink = _outgoing] {
        sink->setEnabled(false);
        Notify(shared, [](const Callbacks &callbacks) { if (callbacks.cameraFailed) callbacks.cameraFailed(); });
      });
      _capture->setState(tgcalls::VideoState::Active);
      _instance->setVideoCapture(_capture);
    } else _capture->switchToDevice(id, false);
  }
  std::int64_t preferredRelay() override { return _instance ? _instance->getPreferredRelayId() : 0; }
  std::string lastError() override { return _instance ? _instance->getLastError() : std::string(); }
  std::string debugInfo() override { return _instance ? _instance->getDebugInfo() : std::string(); }
  std::string soundStats() override {
    const auto lock = std::lock_guard(_shared->soundMutex);
    if (!_shared->sound) return {};
    const auto stats = _shared->sound->stats();
    return "played=" + std::to_string(stats.buffersPlayed) + " underruns=" + std::to_string(stats.underruns)
        + " queue=" + std::to_string(stats.keepReadyCount) + " latency=" + std::to_string(stats.playoutLatencyMs)
        + " recorded=" + std::to_string(stats.buffersRecorded);
  }
  void stop() override {
    _shared->active = false;
    if (!_instance) return;
    _instance->setIncomingVideoOutput({});
    if (_capture) { _capture->setState(tgcalls::VideoState::Inactive); _capture.reset(); }
    auto retired = std::make_shared<std::unique_ptr<tgcalls::Instance>>(std::move(_instance));
    ++kStopping;
    (*retired)->stop([retired](tgcalls::FinalState) {
      Post([retired] {
        retired->reset();
        kLastStopped = std::chrono::steady_clock::now();
        --kStopping;
      });
    });
  }
 private:
  std::shared_ptr<Shared> _shared;
  std::unique_ptr<tgcalls::Instance> _instance;
  std::shared_ptr<tgcalls::VideoCaptureInterface> _capture;
  std::shared_ptr<VideoSink> _incoming;
  std::shared_ptr<VideoSink> _outgoing;
};
}  // namespace

std::unique_ptr<Media> StartMedia(Parameters parameters, Callbacks callbacks) {
  // Initialize on the main thread before WebRTC's static worker pool: OpenSSL's exit cleanup must run after
  // those workers and their thread-local error/random state have gone away.
  static const auto initialized = rtc::InitializeSSL();
  if (!initialized) return nullptr;
  auto media = std::make_unique<TgcallsMedia>();
  return media->start(std::move(parameters), std::move(callbacks)) ? std::move(media) : nullptr;
}

std::vector<std::string> Versions() {
  auto versions = tgcalls::Meta::Versions();
  std::sort(versions.begin(), versions.end(), [](const auto &a, const auto &b) {
    return strverscmp(a.c_str(), b.c_str()) > 0;
  });
  return versions;
}
int MaxLayer() { return tgcalls::Meta::MaxLayer(); }

std::vector<Camera> Cameras() {
  auto options = webrtc::VideoCaptureOptions();
  options.set_allow_v4l2(true);
  auto info = std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo>(webrtc::VideoCaptureFactory::CreateDeviceInfo(&options));
  if (!info) return {};
  auto cameras = std::vector<Camera>();
  for (std::uint32_t i = 0; i != info->NumberOfDevices(); ++i) {
    char name[256] = {}, id[256] = {};
    if (!info->GetDeviceName(i, name, sizeof(name), id, sizeof(id))) cameras.push_back({id, name});
  }
  if (std::any_of(cameras.begin(), cameras.end(), PhoneCamera)) {
    std::erase_if(cameras, [](const auto &camera) { return !PhoneCamera(camera); });
    std::stable_sort(cameras.begin(), cameras.end(), [](const auto &a, const auto &b) {
      return (Lower(a.name).find("front") != std::string::npos) > (Lower(b.name).find("front") != std::string::npos);
    });
  }
  return cameras;
}

void WaitForStoppedMedia() {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (kStopping && std::chrono::steady_clock::now() < deadline) {
    g_main_context_iteration(nullptr, FALSE);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  // WebRTC posts transport destruction after the wrapper's final callback. Keep its static pools alive then too.
  if (kLastStopped != std::chrono::steady_clock::time_point()) {
    std::this_thread::sleep_until(kLastStopped + std::chrono::seconds(2));
  }
}
}  // namespace Contrail::Calls
