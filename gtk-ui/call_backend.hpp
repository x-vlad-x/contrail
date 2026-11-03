// One application-wide 1:1 call. TDLib negotiates it; Media carries sound and pictures.
#pragma once

#include "call_media.hpp"
#include "td/client.hpp"

#include <chrono>
#include <deque>

namespace Contrail {

class CallBackend final {
 public:
  enum class Stage { None, Requesting, Incoming, Ringing, Exchanging, Connecting, Established,
                     Reconnecting, HangingUp, Ended, Busy, Failed };
  struct Snapshot {
    int id = 0;
    std::int64_t user = 0;
    Stage stage = Stage::None;
    bool outgoing = false;
    bool video = false;
    bool muted = false;
    bool remoteMuted = false;
    bool remoteVideo = false;
    bool needRating = false;
    int bars = 0;
    std::string camera;
    std::string emojis;
    std::string error;
  };
  using Factory = std::function<std::unique_ptr<Calls::Media>(Calls::Parameters, Calls::Callbacks)>;
  explicit CallBackend(Td::Client &client, Factory factory = Calls::StartMedia);
  ~CallBackend();
  CallBackend(const CallBackend &) = delete;
  CallBackend &operator=(const CallBackend &) = delete;

  void start(std::int64_t user, bool video = false);
  void accept();
  void hangup(bool disconnected = false);
  void setMuted(bool muted);
  void setCamera(std::string id);
  void setOutputDevice(const std::string &id);
  void rate(int rating, const std::string &comment);
  void setChanged(std::function<void()> callback) { _changed = std::move(callback); }
  void setFrames(std::function<void(Calls::Frame, bool local)> callback) { _frame = std::move(callback); }
  [[nodiscard]] const Snapshot &snapshot() const { return _call; }
  [[nodiscard]] bool active() const;
  [[nodiscard]] int duration() const;
  [[nodiscard]] std::string soundStats() const;

 private:
  void updated(Td::api::Update &update);
  void call(const Td::api::call &call);
  void ready(const Td::api::callStateReady &ready);
  void startMedia(Calls::Parameters parameters);
  void stop();
  void fail(const std::string &message);
  void changed();
  [[nodiscard]] Td::api::object_ptr<Td::api::callProtocol> protocol() const;

  Td::Client &_client;
  Factory _factory;
  Td::Lifetime _lifetime;
  Td::Lifetime _callLifetime;
  Td::Lifetime _mediaLifetime;
  Snapshot _call;
  std::unique_ptr<Calls::Media> _media;
  std::deque<std::string> _earlySignaling;
  std::size_t _earlySignalingBytes = 0;
  std::function<void()> _changed;
  std::function<void(Calls::Frame, bool)> _frame;
  std::chrono::steady_clock::time_point _establishedAt;
  int _duration = 0;
  bool _established = false;
  bool _creating = false;
  bool _startingMedia = false;
  bool _cancelCreate = false;
  std::string _outputDevice;
  double _connectTimeout = 30.;
  double _packetTimeout = 10.;
};

}  // namespace Contrail
