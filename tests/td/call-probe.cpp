// A console Telegram client on TDLib and tgcalls, without Qt: logs in and makes real 1:1 calls with the sound
// module of gtk-ui/media. The stopping point of the move to TDLib: run it on the FLX1s, talk to a person.
//
//   call-probe [DATA_DIRECTORY]      default ~/.local/share/contrail-td/probe
//
// Commands, one per line:
//   contacts                  list the contacts with their ids
//   call ID | call @USERNAME  call
//   accept | decline | hangup
//   mute | unmute
//   route speaker | route earpiece
//   load N                    load N more chats, to see the memory on a real account
//   stats                     memory, threads, the sound module
//   quit
#include "media/openal_adm.hpp"

#include "Instance.h"
#include "InstanceImpl.h"
#include "StaticThreads.h"
#include "v2/InstanceV2Impl.h"
#include "v2/InstanceV2ReferenceImpl.h"

#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>

#include <api/make_ref_counted.h>

#include <execinfo.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace td_api = td::td_api;

namespace {

const auto RegisterV1 = tgcalls::Register<tgcalls::InstanceImpl>();
const auto RegisterV2 = tgcalls::Register<tgcalls::InstanceV2Impl>();
const auto RegisterV2Reference = tgcalls::Register<tgcalls::InstanceV2ReferenceImpl>();

constexpr auto kMinLayer = 65;
constexpr auto kApiId = PROBE_API_ID;
constexpr const char *kApiHash = PROBE_API_HASH;

// Work from the threads of stdin and tgcalls, done on the main one.
class Inbox {
 public:
  void post(std::function<void()> task) {
    auto lock = std::lock_guard(_mutex);
    _tasks.push_back(std::move(task));
  }

  void drain() {
    auto tasks = std::deque<std::function<void()>>();
    {
      auto lock = std::lock_guard(_mutex);
      tasks.swap(_tasks);
    }
    for (auto &task : tasks) task();
  }

 private:
  std::mutex _mutex;
  std::deque<std::function<void()>> _tasks;
};

long Field(const char *path, const char *name) {
  auto in = std::ifstream(path);
  auto line = std::string();
  while (std::getline(in, line)) {
    if (line.rfind(name, 0) == 0) return std::atol(line.c_str() + std::strlen(name));
  }
  return -1;
}

std::string Hex(const std::string &value) {
  static const char digits[] = "0123456789abcdef";
  auto result = std::string();
  for (const auto byte : value) {
    result += digits[uint8_t(byte) / 16];
    result += digits[uint8_t(byte) % 16];
  }
  return result;
}

// The reader thread stays in getline: the terminal itself stops echoing for the password.
void SetEcho(bool enabled) {
  auto state = termios();
  if (tcgetattr(STDIN_FILENO, &state) != 0) return;
  state.c_lflag = enabled ? (state.c_lflag | ECHO) : (state.c_lflag & ~tcflag_t(ECHO));
  tcsetattr(STDIN_FILENO, TCSANOW, &state);
}

// The stack of a crash goes to the terminal: addresses resolve against call-probe.debug from the build.
void PrintStackAndAbort() {
  void *frames[64];
  const auto count = backtrace(frames, 64);
  std::fputs("CRASH, send this stack:\n", stderr);
  backtrace_symbols_fd(frames, count, STDERR_FILENO);
  std::abort();
}

class Probe {
 public:
  explicit Probe(std::string directory) : _directory(std::move(directory)) {
    // TDLib tells why it lost the session only from level 2, and it wipes the database directory: the log lives beside it.
    const auto log = _directory + ".log";
    std::filesystem::create_directories(std::filesystem::path(log).parent_path());
    td::ClientManager::execute(td_api::make_object<td_api::setLogStream>(
        td_api::make_object<td_api::logStreamFile>(log, 16 * 1024 * 1024, false)));
    td::ClientManager::execute(td_api::make_object<td_api::setLogVerbosityLevel>(2));
    std::printf("TDLib log: %s\n", log.c_str());
    _client = _manager.create_client_id();
    send(td_api::make_object<td_api::getOption>("version"));
  }

  int run() {
    auto input = std::thread([this] { readInput(); });
    input.detach();
    while (!_closed) {
      auto response = _manager.receive(0.05);
      if (response.object) handle(response.request_id, std::move(response.object));
      _inbox.drain();
    }
    // TDLib may close with no last updateCall, when the session is lost: the call must not outlive the client.
    const auto hadCall = _callId || _instance;
    if (hadCall) finish("the client closed");
    restoreRoute();
    std::fflush(nullptr);
    // tgcalls takes a call apart on its own threads after stop() and reset() return, with no signal when it is done,
    // and static destructors at exit run under those threads. teardown-probe pair reproduces this crash.
    if (hadCall) std::this_thread::sleep_for(std::chrono::seconds(2));
    _exit(0);
  }

 private:
  using Object = td_api::object_ptr<td_api::Object>;
  using Handler = std::function<void(Object)>;

  void send(td_api::object_ptr<td_api::Function> request, Handler handler = {}) {
    const auto id = ++_lastRequest;
    if (handler) _handlers.emplace(id, std::move(handler));
    _manager.send(_client, id, std::move(request));
  }

  static void reportError(const char *what, const Object &object) {
    if (object->get_id() == td_api::error::ID) {
      const auto &error = static_cast<const td_api::error &>(*object);
      std::printf("%s: error %d %s\n", what, int(error.code_), error.message_.c_str());
    }
  }

  Handler checked(const char *what) {
    return [what](Object object) { reportError(what, object); };
  }

  void handle(uint64_t requestId, Object object) {
    if (requestId) {
      const auto i = _handlers.find(requestId);
      if (i == _handlers.end()) return;
      auto handler = std::move(i->second);
      _handlers.erase(i);
      handler(std::move(object));
      return;
    }
    switch (object->get_id()) {
      case td_api::updateAuthorizationState::ID:
        authorization(*static_cast<td_api::updateAuthorizationState &>(*object).authorization_state_);
        break;
      case td_api::updateUser::ID: {
        const auto &user = *static_cast<td_api::updateUser &>(*object).user_;
        _names[user.id_] = user.first_name_ + (user.last_name_.empty() ? "" : " " + user.last_name_);
        break;
      }
      case td_api::updateCall::ID:
        call(std::move(static_cast<td_api::updateCall &>(*object).call_));
        break;
      case td_api::updateNewCallSignalingData::ID: {
        auto &update = static_cast<td_api::updateNewCallSignalingData &>(*object);
        if (update.call_id_ != _callId) break;
        auto data = std::vector<uint8_t>(update.data_.begin(), update.data_.end());
        if (_instance) {
          _instance->receiveSignalingData(data);
        } else {
          _earlySignaling.push_back(std::move(data));
        }
        break;
      }
      case td_api::updateOption::ID: {
        auto &update = static_cast<td_api::updateOption &>(*object);
        if (update.value_->get_id() == td_api::optionValueInteger::ID) {
          const auto value = static_cast<td_api::optionValueInteger &>(*update.value_).value_;
          if (update.name_ == "call_connect_timeout_ms") _connectTimeoutMs = value;
          if (update.name_ == "call_packet_timeout_ms") _packetTimeoutMs = value;
        }
        break;
      }
      default:
        break;
    }
  }

  void authorization(td_api::AuthorizationState &state) {
    switch (state.get_id()) {
      case td_api::authorizationStateWaitTdlibParameters::ID: {
        auto request = td_api::make_object<td_api::setTdlibParameters>();
        request->database_directory_ = _directory;
        request->use_file_database_ = true;
        request->use_chat_info_database_ = true;
        request->use_message_database_ = true;
        request->use_secret_chats_ = false;
        request->api_id_ = kApiId;
        request->api_hash_ = kApiHash;
        request->system_language_code_ = "en";
        request->device_model_ = "FLX1s";
        request->system_version_ = "FuriOS";
        request->application_version_ = "0.4.0-td.0";
        send(std::move(request), checked("parameters"));
        break;
      }
      case td_api::authorizationStateWaitPhoneNumber::ID:
        _prompt = Prompt::Phone;
        std::puts("Phone number (international format):");
        break;
      case td_api::authorizationStateWaitCode::ID:
        _prompt = Prompt::Code;
        std::puts("Login code:");
        break;
      case td_api::authorizationStateWaitPassword::ID:
        _prompt = Prompt::Password;
        std::puts("Cloud password (not shown):");
        SetEcho(false);
        break;
      case td_api::authorizationStateReady::ID:
        _prompt = Prompt::None;
        std::puts("Logged in. Commands: contacts, call ID, call @USERNAME, accept, decline, hangup, mute, unmute, "
                  "route speaker, route earpiece, load N, stats, quit");
        break;
      case td_api::authorizationStateLoggingOut::ID:
        std::puts("LOGGED OUT: the session is lost, TDLib deletes its database. The reason is in the TDLib log.");
        break;
      case td_api::authorizationStateClosing::ID:
        std::puts("Closing...");
        break;
      case td_api::authorizationStateClosed::ID:
        std::puts("Closed.");
        _closed = true;
        break;
      default:
        std::printf("Authorization state %d is not handled by the probe.\n", int(state.get_id()));
        break;
    }
    std::fflush(stdout);
  }

  void readInput() {
    auto line = std::string();
    while (true) {
      if (!std::getline(std::cin, line)) break;
      _inbox.post([this, line] { command(line); });
    }
    _inbox.post([this] {
      _prompt = Prompt::None;  // Otherwise "quit" would go to TDLib as the answer to a login prompt.
      command("quit");
    });
  }

  void command(const std::string &line) {
    if (_prompt != Prompt::None) {
      const auto prompt = _prompt;
      _prompt = Prompt::None;
      // TDLib stays in the same state after a refused answer: ask again.
      const auto answered = [this, prompt](Object object) {
        if (object->get_id() != td_api::error::ID) return;
        reportError("login", object);
        _prompt = prompt;
        if (prompt == Prompt::Password) SetEcho(false);
        std::puts("Try again:");
        std::fflush(stdout);
      };
      switch (prompt) {
        case Prompt::Phone:
          send(td_api::make_object<td_api::setAuthenticationPhoneNumber>(line, nullptr), answered);
          break;
        case Prompt::Code:
          send(td_api::make_object<td_api::checkAuthenticationCode>(line), answered);
          break;
        default:
          SetEcho(true);
          send(td_api::make_object<td_api::checkAuthenticationPassword>(line), answered);
          break;
      }
      return;
    }
    auto stream = std::istringstream(line);
    auto name = std::string();
    auto argument = std::string();
    stream >> name >> argument;
    if (name == "contacts") {
      contacts();
    } else if (name == "call") {
      dial(argument);
    } else if (name == "accept") {
      if (_callId) send(td_api::make_object<td_api::acceptCall>(_callId, protocol()), checked("accept"));
    } else if (name == "decline" || name == "hangup") {
      hangup(false);
    } else if (name == "mute" || name == "unmute") {
      if (_instance) _instance->setMuteMicrophone(name == "mute");
    } else if (name == "route") {
      route(argument);
    } else if (name == "load") {
      send(td_api::make_object<td_api::loadChats>(td_api::make_object<td_api::chatListMain>(),
                                                  argument.empty() ? 100 : std::atoi(argument.c_str())),
           checked("load"));
    } else if (name == "stats") {
      stats();
    } else if (name == "quit") {
      if (_quitting) return;
      _quitting = true;
      hangup(false);
      restoreRoute();
      send(td_api::make_object<td_api::close>());
    } else if (!name.empty()) {
      std::puts("Unknown command.");
    }
    std::fflush(stdout);
  }

  void contacts() {
    send(td_api::make_object<td_api::getContacts>(), [this](Object object) {
      if (object->get_id() != td_api::users::ID) return reportError("contacts", object);
      for (const auto id : static_cast<td_api::users &>(*object).user_ids_) {
        std::printf("%lld %s\n", (long long)id, _names[id].c_str());
      }
      std::fflush(stdout);
    });
  }

  void dial(const std::string &target) {
    if (_callId) return (void)std::puts("A call is already going on.");
    if (target.empty()) return (void)std::puts("call ID or call @USERNAME");
    if (target[0] != '@') return dialUser(std::atoll(target.c_str()));
    send(td_api::make_object<td_api::searchPublicChat>(target.substr(1)), [this](Object object) {
      if (object->get_id() != td_api::chat::ID) return reportError("search", object);
      const auto &type = *static_cast<td_api::chat &>(*object).type_;
      if (type.get_id() != td_api::chatTypePrivate::ID) return (void)std::puts("Not a user.");
      dialUser(static_cast<const td_api::chatTypePrivate &>(type).user_id_);
    });
  }

  void dialUser(int64_t user) {
    send(td_api::make_object<td_api::createCall>(user, protocol(), false), checked("call"));
  }

  [[nodiscard]] td_api::object_ptr<td_api::callProtocol> protocol() {
    auto versions = tgcalls::Meta::Versions();
    std::reverse(versions.begin(), versions.end());
    return td_api::make_object<td_api::callProtocol>(true, true, kMinLayer, tgcalls::Meta::MaxLayer(),
                                                     std::move(versions));
  }

  void hangup(bool disconnected) {
    if (!_callId) return;
    const auto duration = _establishedAt ? int(std::chrono::duration_cast<std::chrono::seconds>(
                                                   std::chrono::steady_clock::now() - *_establishedAt)
                                                   .count())
                                         : 0;
    const auto relay = _instance ? _instance->getPreferredRelayId() : 0;
    send(td_api::make_object<td_api::discardCall>(_callId, disconnected, std::string(), duration, false, relay),
         checked("hangup"));
  }

  void call(td_api::object_ptr<td_api::call> call) {
    const auto stateId = call->state_->get_id();
    if (_callId && call->id_ != _callId) {
      // Busy: this probe holds one call at a time.
      if (stateId == td_api::callStatePending::ID && !call->is_outgoing_) {
        send(td_api::make_object<td_api::discardCall>(call->id_, false, std::string(), 0, false, 0));
      }
      return;
    }
    switch (stateId) {
      case td_api::callStatePending::ID:
        if (!_callId) {
          _callId = call->id_;
          _outgoing = call->is_outgoing_;
          std::printf(_outgoing ? "Calling %s...\n" : "INCOMING call from %s: accept or decline\n",
                      _names[call->user_id_].c_str());
        }
        break;
      case td_api::callStateExchangingKeys::ID:
        std::puts("Exchanging keys...");
        break;
      case td_api::callStateReady::ID:
        if (!_instance) start(static_cast<td_api::callStateReady &>(*call->state_));
        break;
      case td_api::callStateHangingUp::ID:
        std::puts("Hanging up...");
        break;
      case td_api::callStateDiscarded::ID:
        finish("discarded");
        break;
      case td_api::callStateError::ID: {
        const auto &error = *static_cast<td_api::callStateError &>(*call->state_).error_;
        std::printf("Call error %d %s\n", int(error.code_), error.message_.c_str());
        finish("error");
        break;
      }
      default:
        break;
    }
    std::fflush(stdout);
  }

  void start(td_api::callStateReady &ready) {
    if (ready.protocol_->library_versions_.empty() || ready.encryption_key_.size() != tgcalls::EncryptionKey::kSize) {
      std::puts("The call has no usable version or key.");
      return hangup(true);
    }
    const auto version = ready.protocol_->library_versions_.front();
    std::printf("Ready: tgcalls %s, p2p %s, %zu servers, emojis:", version.c_str(), ready.allow_p2p_ ? "on" : "off",
                ready.servers_.size());
    for (const auto &emoji : ready.emojis_) std::printf(" %s", emoji.c_str());
    std::puts("");

    auto key = std::make_shared<std::array<uint8_t, tgcalls::EncryptionKey::kSize>>();
    std::memcpy(key->data(), ready.encryption_key_.data(), key->size());

    auto descriptor = tgcalls::Descriptor{
        .version = version,
        .config =
            tgcalls::Config{
                .initializationTimeout = _connectTimeoutMs / 1000.,
                .receiveTimeout = _packetTimeoutMs / 1000.,
                .dataSaving = tgcalls::DataSaving::Never,
                .enableP2P = ready.allow_p2p_,
                .enableAEC = false,
                .enableNS = true,
                .enableAGC = true,
                .enableVolumeControl = true,
                .maxApiLayer = ready.protocol_->max_layer_,
            },
        .encryptionKey = tgcalls::EncryptionKey(key, _outgoing),
        .mediaDevicesConfig = tgcalls::MediaDevicesConfig{.inputVolume = 1.f, .outputVolume = 1.f},
        .stateUpdated = [this](tgcalls::State state) { _inbox.post([this, state] { instanceState(state); }); },
        .signalBarsUpdated = [this](int bars) {
          _inbox.post([this, bars] {
            if (bars != _bars) std::printf("Signal bars %d\n", _bars = bars), std::fflush(stdout);
          });
        },
        .remoteMediaStateUpdated = [this](tgcalls::AudioState audio, tgcalls::VideoState) {
          _inbox.post([audio] {
            std::puts(audio == tgcalls::AudioState::Muted ? "The other side muted." : "The other side speaks.");
            std::fflush(stdout);
          });
        },
        .signalingDataEmitted = [this](const std::vector<uint8_t> &data) {
          auto bytes = std::string(data.begin(), data.end());
          _inbox.post([this, bytes = std::move(bytes)] {
            if (_callId) {
              send(td_api::make_object<td_api::sendCallSignalingData>(_callId, bytes), checked("signaling"));
            }
          });
        },
        .createAudioDeviceModule = [this](webrtc::TaskQueueFactory *factory) {
          auto device = rtc::make_ref_counted<media::OpenAlAudioDevice>(factory);
          _sound = device.get();
          return webrtc::scoped_refptr<webrtc::AudioDeviceModule>(device);
        },
    };

    // As tdesktop's Call::createAndStartController does with the phone connections.
    auto ids = std::set<int64_t>();
    for (const auto &server : ready.servers_) {
      if (server->type_->get_id() == td_api::callServerTypeTelegramReflector::ID) ids.insert(server->id_);
    }
    for (const auto &server : ready.servers_) {
      const auto port = uint16_t(server->port_);
      if (server->type_->get_id() == td_api::callServerTypeTelegramReflector::ID) {
        const auto &type = static_cast<const td_api::callServerTypeTelegramReflector &>(*server->type_);
        if (type.peer_tag_.size() == 16 && !type.is_tcp_) {
          auto endpoint = tgcalls::Endpoint{
              .endpointId = server->id_,
              .host = {.ipv4 = server->ip_address_, .ipv6 = server->ipv6_address_},
              .port = port,
              .type = tgcalls::EndpointType::UdpRelay,
          };
          std::memcpy(endpoint.peerTag, type.peer_tag_.data(), 16);
          descriptor.endpoints.push_back(std::move(endpoint));
        }
        const auto id = uint8_t(std::distance(ids.begin(), ids.find(server->id_)) + 1);
        for (const auto &host : {server->ip_address_, server->ipv6_address_}) {
          descriptor.rtcServers.push_back({.id = id,
                                           .host = host,
                                           .port = port,
                                           .login = "reflector",
                                           .password = Hex(type.peer_tag_),
                                           .isTurn = true,
                                           .isTcp = type.is_tcp_});
        }
      } else if (server->type_->get_id() == td_api::callServerTypeWebrtc::ID) {
        const auto &type = static_cast<const td_api::callServerTypeWebrtc &>(*server->type_);
        for (const auto &host : {server->ip_address_, server->ipv6_address_}) {
          if (host.empty()) continue;
          if (type.supports_stun_) descriptor.rtcServers.push_back({.host = host, .port = port, .isTurn = false});
          if (type.supports_turn_ && !type.username_.empty() && !type.password_.empty()) {
            descriptor.rtcServers.push_back(
                {.host = host, .port = port, .login = type.username_, .password = type.password_, .isTurn = true});
          }
        }
      }
    }

    _instance = tgcalls::Meta::Create(version, std::move(descriptor));
    if (!_instance) {
      std::printf("tgcalls does not know version %s.\n", version.c_str());
      return hangup(true);
    }
    for (const auto &data : _earlySignaling) _instance->receiveSignalingData(data);
    _earlySignaling.clear();
  }

  void instanceState(tgcalls::State state) {
    switch (state) {
      case tgcalls::State::Established:
        if (!_establishedAt) _establishedAt = std::chrono::steady_clock::now();
        std::puts("CONNECTED");
        break;
      case tgcalls::State::Reconnecting:
        std::puts("Reconnecting...");
        break;
      case tgcalls::State::Failed:
        std::puts("Connection FAILED");
        if (_instance) std::printf("  %s\n", _instance->getLastError().c_str());
        hangup(true);
        break;
      default:
        break;
    }
    std::fflush(stdout);
  }

  void finish(const char *why) {
    std::printf("Call ended: %s\n", why);
    if (_sound) printSound();
    _sound = nullptr;
    if (_instance) {
      if (_instance) std::printf("Debug: %s\n", _instance->getDebugInfo().c_str());
      _instance->stop([](tgcalls::FinalState) {});
      _instance.reset();
    }
    _callId = 0;
    _bars = -1;
    _establishedAt.reset();
    _earlySignaling.clear();
  }

  void route(const std::string &where) {
    const auto port = (where == "speaker") ? "output-speaker" : (where == "earpiece") ? "output-earpiece" : nullptr;
    if (!port) return (void)std::puts("route speaker or route earpiece");
    _routeChanged = true;
    std::system((std::string("pactl set-sink-port @DEFAULT_SINK@ ") + port).c_str());
  }

  void restoreRoute() {
    if (_routeChanged) std::system("pactl set-sink-port @DEFAULT_SINK@ output-speaker");
    _routeChanged = false;
  }

  void printSound() {
    const auto sound = _sound->stats();
    std::printf("Sound: played=%lld underruns=%lld keep=%d largest_take=%d latency=%dms recorded=%lld\n",
                (long long)sound.buffersPlayed, (long long)sound.underruns, sound.keepReadyCount, sound.largestTake,
                sound.playoutLatencyMs, (long long)sound.buffersRecorded);
  }

  void stats() {
    const auto anonymous = Field("/proc/self/smaps_rollup", "Anonymous:");
    const auto swap = Field("/proc/self/smaps_rollup", "Swap:");
    std::printf("Memory: own %ld KiB (anonymous %ld + swap %ld), PSS %ld KiB, RSS %ld KiB, threads %ld\n",
                anonymous + swap, anonymous, swap, Field("/proc/self/smaps_rollup", "Pss:"),
                Field("/proc/self/smaps_rollup", "Rss:"), Field("/proc/self/status", "Threads:"));
    if (_sound) printSound();
    if (_instance) std::printf("%s\n", _instance->getDebugInfo().c_str());
  }

  enum class Prompt { None, Phone, Code, Password };

  std::string _directory;
  td::ClientManager _manager;
  td::ClientManager::ClientId _client = 0;
  uint64_t _lastRequest = 0;
  std::map<uint64_t, Handler> _handlers;
  std::map<int64_t, std::string> _names;
  Inbox _inbox;
  Prompt _prompt = Prompt::None;
  bool _closed = false;
  bool _quitting = false;

  int32_t _callId = 0;
  bool _outgoing = false;
  int _bars = -1;
  int64_t _connectTimeoutMs = 30000;
  int64_t _packetTimeoutMs = 10000;
  std::unique_ptr<tgcalls::Instance> _instance;
  media::OpenAlAudioDevice *_sound = nullptr;
  std::vector<std::vector<uint8_t>> _earlySignaling;
  std::optional<std::chrono::steady_clock::time_point> _establishedAt;
  bool _routeChanged = false;
};

}  // namespace

int main(int argc, char **argv) {
  std::set_terminate(PrintStackAndAbort);
  const auto home = std::getenv("HOME");
  auto probe = Probe((argc > 1) ? argv[1] : (std::string(home ? home : ".") + "/.local/share/contrail-td/probe"));
  return probe.run();
}
