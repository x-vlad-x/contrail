#pragma once

#include "td/store.hpp"

#include <glib.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Contrail {

// Voice messages and music, one track at a time, as 0.3.1's player: FFmpeg decodes, OpenAL plays. The file is read
// while TDLib downloads it: a read past what is here waits, and a seek far ahead moves the download there. The
// thread and the sound device live only while a track plays or is paused, and the device is paused with it.
class AudioPlayer final {
 public:
  struct Track {
    Td::ChatId chat = 0;
    std::int64_t message = 0;
    Td::FileId file = 0;
    int duration = 0;  // Seconds, from the message: the length until the file tells its own.
    bool voice = false;
    [[nodiscard]] bool same(const Track &other) const {
      return chat == other.chat && message == other.message && file == other.file;
    }
  };
  enum class Status { Stopped, Loading, Playing, Paused };
  struct State {
    Track track;
    Status status = Status::Stopped;
    double position = 0.;  // Seconds.
    double length = 0.;
    double buffered = 1.;  // The share of the file downloaded from its start, 1 once it is whole.
    bool waiting = false;  // Plays, but has reached the end of what is downloaded.
  };
  // What plays when a track ends; nothing more when it gives nothing.
  using Next = std::function<std::optional<Track>(const Track &finished)>;

  explicit AudioPlayer(Td::Store &store);
  ~AudioPlayer();
  AudioPlayer(const AudioPlayer &) = delete;
  AudioPlayer &operator=(const AudioPlayer &) = delete;

  // Pauses or plays the track; another one stops for it.
  void playPause(const Track &track);
  // Plays the track from there: the current one seeks, another one starts.
  void seek(const Track &track, double fraction);
  // A drag along the bar: the track playing pauses until it is let go (seek()) or the drag is cancelled.
  void startSeeking(const Track &track);
  void cancelSeeking(const Track &track);
  void stop();

  [[nodiscard]] const State &state() const { return _state; }
  // Playing, paused or loading.
  [[nodiscard]] bool current(const Track &track) const {
    return _state.status != Status::Stopped && _state.track.same(track);
  }
  void onState(std::function<void(const State &)> handler, const Td::Lifetime &lifetime);
  // The chat that is open says what comes next; it lets go with its lifetime.
  void setNext(Next next, const Td::Lifetime &lifetime);
  // For the debug state: "status=playing message=N position=S length=S buffered=F waiting=0 thread=1".
  [[nodiscard]] std::string debugState() const;

  struct Shared;  // What the playing thread and this one share.

 private:
  void start(const Track &track, double from);
  void stopThread();
  void tick();
  void feed(const Td::File &file);
  void finished();
  void setPaused(bool paused);
  void notify();

  Td::Store &_store;
  State _state;
  std::shared_ptr<Shared> _shared;
  std::thread _thread;
  guint _timer = 0;
  std::int64_t _asked = -1;  // The offset a download was last asked from.
  bool _resumeAfterSeeking = false;
  Next _next;
  std::weak_ptr<void> _nextToken;
  struct Subscriber {
    std::function<void(const State &)> handler;
    std::weak_ptr<void> token;
  };
  std::vector<Subscriber> _subscribers;
  Td::Lifetime _lifetime;
};

}  // namespace Contrail
