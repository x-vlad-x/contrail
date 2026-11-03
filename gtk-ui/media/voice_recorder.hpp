#pragma once

#include "td/store.hpp"

#include <glib.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace Contrail {

// A voice message being recorded, as 0.3.1's: Opus in Ogg at 48 kHz, its waveform in Telegram's 100 values of 5 bits.
// It belongs to the window, not to the chat: the chat closed (or the window), it records on, and the chat shows it
// again, ready to be sent or deleted. Only sending it, deleting it, or opening another chat ends it. The microphone
// is open only while it records.
class VoiceRecorder final {
 public:
  struct State {
    double duration = 0.;  // Seconds.
    int level = 0;         // The loudest sample of the last moment, 0 to 32767.
  };

  explicit VoiceRecorder(Td::Store &store);
  ~VoiceRecorder();
  VoiceRecorder(const VoiceRecorder &) = delete;
  VoiceRecorder &operator=(const VoiceRecorder &) = delete;

  // False when it records already or there is no microphone.
  bool start(Td::ChatId chat, std::int32_t topic, std::int64_t replyTo);
  // Sent with the reply it was started with, if it is a second long at least; deleted otherwise.
  void finish(bool send);

  [[nodiscard]] bool active() const { return _shared != nullptr; }
  [[nodiscard]] Td::ChatId chat() const { return _chat; }
  [[nodiscard]] std::int32_t topic() const { return _topic; }
  [[nodiscard]] const State &state() const { return _state; }
  void onState(std::function<void(const State &)> handler, const Td::Lifetime &lifetime);
  // The microphone failed: the recording is gone.
  void onFailed(std::function<void()> handler, const Td::Lifetime &lifetime);

  // For the tests: Telegram's waveform of these loudest samples, one for every 20 ms.
  [[nodiscard]] static std::string Waveform(const std::vector<std::uint16_t> &peaks);
  [[nodiscard]] std::string debugState() const;

  struct Shared;  // What the recording thread and this one share.

 private:
  void tick();
  void stopThread();
  void chatAction(bool recording);

  Td::Store &_store;
  std::shared_ptr<Shared> _shared;
  std::thread _thread;
  guint _timer = 0;
  Td::ChatId _chat = 0;
  std::int32_t _topic = 0;
  std::int64_t _replyTo = 0;
  std::string _path;
  State _state;
  std::int64_t _lastAction = 0;  // When Telegram was last told that it records.
  std::string _lastSent;         // For the debug state: the file, its length and waveform sent last.
  template <typename Handler>
  struct Subscriber {
    Handler handler;
    std::weak_ptr<void> token;
  };
  std::vector<Subscriber<std::function<void(const State &)>>> _stateHandlers;
  std::vector<Subscriber<std::function<void()>>> _failedHandlers;
};

}  // namespace Contrail
