#include "voice_recorder.hpp"

#include "api/api_common.h"
#include "api/api_send_progress.h"
#include "apiwrap.h"
#include "data/data_thread.h"
#include "history/history.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/audio/media_audio_capture.h"
#include "media/audio/media_audio_capture_common.h"
#include "media/player/media_player_instance.h"

namespace Contrail {

VoiceRecorder &Recorder() {
  static VoiceRecorder recorder;
  return recorder;
}

bool VoiceRecorder::start(not_null<Data::Thread *> thread, FullMsgId replyTo) {
  const auto capture = ::Media::Capture::instance();
  if (_active || !capture->available()) {
    return false;
  }
  _active = true;
  _thread = thread;
  _replyTo = replyTo;
  _started = crl::now();
  _state = State();
  Media::Player::instance()->pause(AudioMsgId::Type::Voice);
  // start() ends the previous stream of updates: subscribe after it.
  capture->start();
  capture->updated() | rpl::on_next_error([=](const ::Media::Capture::Update &update) {
    _state = State{update.samples * crl::time(1000) / ::Media::Player::kDefaultFrequency, update.level};
  }, [=](::Media::Capture::Error) {
    finish(false);
    _failed.fire({});
  }, _lifetime);
  const auto history = thread->owningHistory();
  history->session().sendProgressManager().update(history, thread->topicRootId(), Api::SendProgressType::RecordVoice);
  return true;
}

void VoiceRecorder::finish(bool send) {
  if (!_active) {
    return;
  }
  _active = false;
  _lifetime.destroy();
  const auto thread = _thread.get();
  _thread = nullptr;
  if (!send || !thread) {
    ::Media::Capture::instance()->stop();
    if (thread) {
      const auto history = thread->owningHistory();
      history->session().sendProgressManager().cancel(history, thread->topicRootId(), Api::SendProgressType::RecordVoice);
    }
    return;
  }
  auto action = Api::SendAction(thread);
  if (_replyTo) {
    action.replyTo.messageId = _replyTo;
  }
  const auto weak = base::make_weak(thread);
  ::Media::Capture::instance()->stop(crl::guard(this, [=](::Media::Capture::Result &&result) {
    if (!weak || result.bytes.isEmpty() || result.duration < 1000) {
      return;
    }
    action.history->session().api().sendVoiceMessage(result.bytes, result.waveform, result.duration, false, action);
  }));
}

}  // namespace Contrail
