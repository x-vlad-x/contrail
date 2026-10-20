#pragma once

#include "app.hpp"

#include "base/weak_ptr.h"

#include <rpl/event_stream.h>
#include <rpl/lifetime.h>
#include <rpl/variable.h>

namespace Data {
class Thread;
} // namespace Data

namespace Contrail {

// A voice message being recorded. It belongs to the application, not to the window: closing the
// window leaves it recording, and the chat shows it again, ready to be sent or deleted. Only sending
// it, deleting it, or going to another chat ends it.
class VoiceRecorder final : public base::has_weak_ptr {
public:
  struct State {
    crl::time duration = 0;
    int level = 0;
    friend bool operator==(const State &, const State &) = default;
  };

  // False when there is no microphone.
  bool start(not_null<Data::Thread *> thread, FullMsgId replyTo);
  // Sends or deletes it. The reply it was started with is the one it goes with.
  void finish(bool send);

  [[nodiscard]] bool active() const { return _active; }
  [[nodiscard]] Data::Thread *thread() const { return _thread.get(); }
  [[nodiscard]] crl::time started() const { return _started; }
  [[nodiscard]] rpl::producer<State> state() const { return _state.value(); }
  // The microphone failed: the recording is gone.
  [[nodiscard]] rpl::producer<> failed() const { return _failed.events(); }

private:
  bool _active = false;
  base::weak_ptr<Data::Thread> _thread;
  FullMsgId _replyTo;
  crl::time _started = 0;
  rpl::variable<State> _state;
  rpl::event_stream<> _failed;
  rpl::lifetime _lifetime;
};

[[nodiscard]] VoiceRecorder &Recorder();

}  // namespace Contrail
