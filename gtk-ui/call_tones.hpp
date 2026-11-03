#pragma once

#include <memory>
#include <string>

namespace Contrail::Calls {
enum class Tone { None, Incoming, Outgoing, Connecting, Ended, Busy };

class TonePlayer {
 public:
  virtual ~TonePlayer() = default;
  virtual void play(Tone tone, bool loop = false) = 0;
  virtual void stop() = 0;
  virtual void setOutputDevice(const std::string &device) = 0;
  [[nodiscard]] virtual Tone current() const = 0;
};
[[nodiscard]] std::unique_ptr<TonePlayer> MakeTonePlayer();
} // namespace Contrail::Calls
