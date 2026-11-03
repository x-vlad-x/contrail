#include "call_backend.hpp"
#include "td/session.hpp"

#include <algorithm>
#include <glib.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <unistd.h>

namespace Contrail {
namespace api = Td::api;

CallBackend::CallBackend(Td::Client &client, Factory factory) : _client(client), _factory(std::move(factory)) {
  _client.subscribe([this](api::Update &update) { updated(update); }, _lifetime);
}

CallBackend::~CallBackend() {
  _changed = nullptr;
  _frame = nullptr;
  if (active()) hangup();
  stop();
}

bool CallBackend::active() const {
  using S = Stage;
  return _creating || (_call.stage != S::None && _call.stage != S::Ended && _call.stage != S::Busy
      && _call.stage != S::Failed);
}

int CallBackend::duration() const {
  return _established ? int(std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - _establishedAt).count()) : _duration;
}

std::string CallBackend::soundStats() const { return _media ? _media->soundStats() : std::string(); }

api::object_ptr<api::callProtocol> CallBackend::protocol() const {
  auto versions = Calls::Versions();
  // A scenario also works in a build without the optional media library.
  if (versions.empty() && g_getenv("CONTRAIL_SCENARIO")) versions = {"13.0.0"};
  return api::make_object<api::callProtocol>(true, true, 65, Calls::MaxLayer(), std::move(versions));
}

void CallBackend::start(std::int64_t user, bool video) {
  if (active() || !user) return;
  if (Calls::Versions().empty() && !g_getenv("CONTRAIL_SCENARIO")) {
    fail("This build has no call media support.");
    return;
  }
  stop();
  _callLifetime.reset();
  _duration = 0;
  _debugInfo.clear();
  _call = {.user = user, .stage = Stage::Requesting, .outgoing = true, .video = video};
  _creating = true;
  _cancelCreate = false;
  changed();
  _client.request<api::callId>(api::make_object<api::createCall>(user, protocol(), video), _callLifetime,
      [this](api::callId &result) {
        _creating = false;
        if (!_call.id) _call.id = result.id_;
        if (_cancelCreate) hangup();
        else changed();
      }, [this](api::error &error) { _creating = false; fail(error.message_); });
}

void CallBackend::accept() {
  if (_call.stage != Stage::Incoming) return;
  _call.stage = Stage::Exchanging;
  changed();
  _client.request<api::ok>(api::make_object<api::acceptCall>(_call.id, protocol()), _callLifetime,
      [](api::ok &) {}, [this](api::error &error) { fail(error.message_); });
}

void CallBackend::hangup(bool disconnected) {
  if (_creating && !_call.id) { _cancelCreate = true; return; }
  if (!active() || !_call.id || _call.stage == Stage::HangingUp) return;
  const auto relay = _media ? _media->preferredRelay() : 0;
  const auto seconds = duration();
  _call.stage = Stage::HangingUp;
  stop();
  changed();
  _client.request<api::ok>(api::make_object<api::discardCall>(
      _call.id, disconnected, std::string(), seconds, _call.video, relay), _callLifetime,
      [](api::ok &) {}, [this](api::error &error) { fail(error.message_); });
}

void CallBackend::setMuted(bool muted) {
  if (!active()) return;
  _call.muted = muted;
  if (_media) _media->setMuted(muted);
  changed();
}

void CallBackend::setCamera(std::string id) {
  if (!_media || !active()) return;
  _call.error.clear();
  _call.camera = std::move(id);
  if (!_call.camera.empty()) _call.video = true;
  _media->setCamera(_call.camera);
  if (_call.camera.empty() && _frame) _frame({}, true);
  changed();
}

void CallBackend::setOutputDevice(const std::string &id) {
  _outputDevice = id;
  if (_media) _media->setOutputDevice(id);
}

void CallBackend::rate(int rating, const std::string &comment) {
  if (!_call.needRating || rating < 1 || rating > 5) return;
  _call.needRating = false;
  _client.send(api::make_object<api::sendCallRating>(api::make_object<api::inputCallDiscarded>(_call.id),
      rating, comment, std::vector<api::object_ptr<api::CallProblem>>()));
  changed();
}

void CallBackend::updated(api::Update &update) {
  if (update.get_id() == api::updateCall::ID) {
    const auto &value = static_cast<api::updateCall &>(update).call_;
    if (value && value->state_) call(*value);
  } else if (update.get_id() == api::updateNewCallSignalingData::ID) {
    const auto &value = static_cast<api::updateNewCallSignalingData &>(update);
    if (value.call_id_ != _call.id || !active() || _call.stage == Stage::HangingUp) return;
    if (_media) _media->receiveSignaling(value.data_);
    // A peer can signal before Ready. Bound the queue while key exchange is pending.
    else if (_earlySignaling.size() < 128 && value.data_.size() <= 1024 * 1024 - _earlySignalingBytes) {
      _earlySignalingBytes += value.data_.size();
      _earlySignaling.push_back(value.data_);
    }
    else { hangup(true); fail("Too much early call signaling."); }
  } else if (update.get_id() == api::updateOption::ID) {
    const auto &value = static_cast<api::updateOption &>(update);
    if (!value.value_ || value.value_->get_id() != api::optionValueInteger::ID) return;
    const auto ms = static_cast<const api::optionValueInteger &>(*value.value_).value_;
    if (ms <= 0) return;
    if (value.name_ == "call_connect_timeout_ms") _connectTimeout = ms / 1000.;
    if (value.name_ == "call_packet_timeout_ms") _packetTimeout = ms / 1000.;
  } else if (update.get_id() == api::updateAuthorizationState::ID) {
    const auto &state = static_cast<api::updateAuthorizationState &>(update).authorization_state_;
    if (state && (state->get_id() == api::authorizationStateLoggingOut::ID
        || state->get_id() == api::authorizationStateClosing::ID || state->get_id() == api::authorizationStateClosed::ID)) {
      _creating = false;
      stop();
      _call.stage = Stage::Ended;
      changed();
    }
  }
}

void CallBackend::call(const api::call &source) {
  const auto state = source.state_->get_id();
  if (source.id_ != _call.id) {
    if (active() && !(_creating && !_call.id && source.is_outgoing_ && source.user_id_ == _call.user)) {
      if (!source.is_outgoing_ && state == api::callStatePending::ID) {
        _client.send(api::make_object<api::discardCall>(source.id_, false, std::string(), 0, false, 0));
      }
      return;
    }
    // Calls already finished on another device must never open the call window.
    if (state != api::callStatePending::ID && !(_creating && source.is_outgoing_)) return;
    stop();
    if (!_creating) _callLifetime.reset();
    _duration = 0;
    _debugInfo.clear();
    _call = {.id = source.id_, .user = source.user_id_, .outgoing = source.is_outgoing_, .video = source.is_video_};
  }
  switch (state) {
    case api::callStatePending::ID: {
      if (_call.stage == Stage::HangingUp) break;
      const auto &pending = static_cast<const api::callStatePending &>(*source.state_);
      _call.stage = !source.is_outgoing_ ? Stage::Incoming : pending.is_received_ ? Stage::Ringing : Stage::Requesting;
      break;
    }
    case api::callStateExchangingKeys::ID:
      if (_call.stage != Stage::HangingUp) _call.stage = Stage::Exchanging;
      break;
    case api::callStateReady::ID:
      if (_call.stage != Stage::HangingUp && !_media && !_startingMedia) ready(static_cast<const api::callStateReady &>(*source.state_));
      break;
    case api::callStateHangingUp::ID:
      _call.stage = Stage::HangingUp;
      stop();
      break;
    case api::callStateDiscarded::ID: {
      const auto &discarded = static_cast<const api::callStateDiscarded &>(*source.state_);
      stop();
      _creating = false;
      if (discarded.need_debug_information_ && !_debugInfo.empty()) {
        _client.send(api::make_object<api::sendCallDebugInformation>(
            api::make_object<api::inputCallDiscarded>(_call.id), _debugInfo));
        _debugInfo.clear();
      }
      _call.needRating = discarded.need_rating_;
      _call.stage = discarded.reason_ && discarded.reason_->get_id() == api::callDiscardReasonDeclined::ID
          && _call.outgoing ? Stage::Busy : Stage::Ended;
      break;
    }
    case api::callStateError::ID:
      _creating = false;
      fail(static_cast<const api::callStateError &>(*source.state_).error_->message_);
      return;
    default: break;
  }
  changed();
}

void CallBackend::ready(const api::callStateReady &ready) {
  if (!ready.protocol_ || ready.protocol_->library_versions_.empty() || ready.encryption_key_.size() != 256) {
    hangup(true);
    fail("The call has no usable protocol or encryption key.");
    return;
  }
  auto parameters = Calls::Parameters{
      .version = ready.protocol_->library_versions_.front(), .key = ready.encryption_key_,
      .outgoing = _call.outgoing, .allowP2p = ready.allow_p2p_, .maxLayer = ready.protocol_->max_layer_,
      .customParameters = ready.custom_parameters_, .video = _call.video,
      .connectTimeout = _connectTimeout, .packetTimeout = _packetTimeout};
  if (g_getenv("CONTRAIL_DEBUG") && !g_getenv("CONTRAIL_SCENARIO")) {
    const auto directory = Td::DataDirectory();
    if (g_mkdir_with_parents(directory.c_str(), 0700) == 0) {
      const auto path = directory + "/last-call.json";
      const auto file = g_open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
      if (file >= 0) { close(file); parameters.statsLogPath = path; }
    }
    g_message("call-media: protocol=%s p2p=%d stats=%s", parameters.version.c_str(),
        parameters.allowP2p && !g_getenv("CONTRAIL_CALL_RELAY"), parameters.statsLogPath.c_str());
  }
  for (const auto &source : ready.servers_) {
    if (!source || !source->type_) continue;
    auto server = Calls::Server{.id = source->id_, .ipv4 = source->ip_address_, .ipv6 = source->ipv6_address_,
        .port = source->port_};
    if (source->type_->get_id() == api::callServerTypeTelegramReflector::ID) {
      const auto &type = static_cast<const api::callServerTypeTelegramReflector &>(*source->type_);
      server.reflector = true; server.peerTag = type.peer_tag_; server.tcp = type.is_tcp_;
    } else if (source->type_->get_id() == api::callServerTypeWebrtc::ID) {
      const auto &type = static_cast<const api::callServerTypeWebrtc &>(*source->type_);
      server.stun = type.supports_stun_; server.turn = type.supports_turn_;
      server.username = type.username_; server.password = type.password_;
    }
    parameters.servers.push_back(std::move(server));
  }
  _call.emojis.clear();
  for (const auto &emoji : ready.emojis_) { if (!_call.emojis.empty()) _call.emojis += ' '; _call.emojis += emoji; }
  if (_call.video) {
    if (g_getenv("CONTRAIL_SCENARIO")) parameters.camera = _call.camera = "scenario-front";
    else {
      const auto cameras = Calls::Cameras();
      if (!cameras.empty()) parameters.camera = _call.camera = cameras.front().id;
    }
  }
  _call.stage = Stage::Connecting;
  _startingMedia = true;
  _client.request<api::addedProxies>(api::make_object<api::getProxies>(), _mediaLifetime,
      [this, parameters = std::make_shared<Calls::Parameters>(std::move(parameters))](api::addedProxies &list) {
        for (const auto &entry : list.proxies_) {
          if (!entry->is_enabled_ || !entry->proxy_ || !entry->proxy_->type_
              || entry->proxy_->type_->get_id() != api::proxyTypeSocks5::ID) continue;
          const auto &proxy = *entry->proxy_;
          const auto &type = static_cast<const api::proxyTypeSocks5 &>(*proxy.type_);
          parameters->proxy = {.host = proxy.server_, .port = proxy.port_, .username = type.username_,
              .password = type.password_};
          break;
        }
        startMedia(std::move(*parameters));
      }, [this](api::error &error) { hangup(true); fail(error.message_); });
}

void CallBackend::startMedia(Calls::Parameters parameters) {
  _startingMedia = false;
  const auto token = _mediaLifetime.token();
  auto callbacks = Calls::Callbacks{
      .state = [this, token](Calls::State state) {
        if (token.expired() || !active() || _call.stage == Stage::HangingUp) return;
        if (state == Calls::State::Failed) { hangup(true); fail("Call connection failed."); return; }
        _call.stage = state == Calls::State::Established ? Stage::Established
            : state == Calls::State::Reconnecting ? Stage::Reconnecting : Stage::Connecting;
        if (state == Calls::State::Established && !_established) {
          _established = true; _establishedAt = std::chrono::steady_clock::now();
        }
        changed();
      },
      .signalBars = [this, token](int bars) { if (!token.expired()) { _call.bars = std::clamp(bars, 0, 4); changed(); } },
      .remoteMedia = [this, token](bool muted, bool video) {
        if (!token.expired()) { _call.remoteMuted = muted; _call.remoteVideo = video; changed(); }
      },
      .signaling = [this, token](std::string data) {
        if (!token.expired() && active()) _client.send(api::make_object<api::sendCallSignalingData>(_call.id, data));
      },
      .remoteFrame = [this, token](Calls::Frame frame) { if (!token.expired() && _frame) _frame(std::move(frame), false); },
      .localFrame = [this, token](Calls::Frame frame) { if (!token.expired() && _frame) _frame(std::move(frame), true); },
      .cameraFailed = [this, token] {
        if (token.expired()) return;
        _call.camera.clear();
        _call.error = "CAMERA_UNAVAILABLE";
        if (_frame) _frame({}, true);
        changed();
      },
      .videoStopped = [this, token] { if (!token.expired() && _frame) _frame({}, false); }};
  auto media = _factory(std::move(parameters), std::move(callbacks));
  if (token.expired()) { if (media) media->stop(); return; }
  _media = std::move(media);
  if (!_media) { hangup(true); fail("Unsupported call media protocol."); return; }
  _media->setMuted(_call.muted);
  _media->setOutputDevice(_outputDevice);
  auto earlySignaling = std::move(_earlySignaling);
  _earlySignaling.clear();
  _earlySignalingBytes = 0;
  for (const auto &data : earlySignaling) {
    if (!_media) break;
    _media->receiveSignaling(data);
  }
}

void CallBackend::stop() {
  _duration = duration();
  _established = false;
  _mediaLifetime.reset();
  _startingMedia = false;
  if (_media) { _debugInfo = _media->debugInfo(); _media->stop(); _media.reset(); }
  _earlySignaling.clear();
  _earlySignalingBytes = 0;
}

void CallBackend::fail(const std::string &message) {
  _creating = false;
  stop();
  _call.stage = Stage::Failed;
  _call.error = message;
  changed();
}

void CallBackend::changed() { if (_changed) _changed(); }

}  // namespace Contrail
