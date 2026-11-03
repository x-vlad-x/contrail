#include "voice_record_bar.hpp"

#include "chat_view.hpp"
#include "main_window.hpp"
#include "media/audio_player.hpp"
#include "media/voice_recorder.hpp"
#include "td/strings.hpp"

#include <adwaita.h>

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace Contrail {
namespace {

constexpr auto kCancelDistance = 120.;  // Slide left to delete.
constexpr auto kLockDistance = 90.;     // Slide up to record on without holding.
constexpr auto kMinHold = gint64(400) * 1000;
constexpr auto kHintMargin = 16;  // "Slide to cancel" right of the time, left of the microphone.
constexpr auto kLockMargin = 16;

// "0:03,4": seconds and tenths, with the locale's point.
std::string RecordTime(double seconds) {
  const auto tenths = std::int64_t(seconds * 10);
  const auto whole = tenths / 10;
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%lld:%02lld%s%lld", (long long)(whole / 60), (long long)(whole % 60),
                std::localeconv()->decimal_point, (long long)(tenths % 10));
  return buffer;
}

}  // namespace

VoiceRecordBar::VoiceRecordBar(ChatView &chat, GtkWidget *composer, GtkWidget *composerStack, GtkWidget *sendButton,
                               GtkWidget *toBottom)
    : _chat(chat), _composer(composer), _composerStack(composerStack), _toBottom(toBottom) {
  // The microphone, in the send button's place.
  _mic = MakeIconButton("audio-input-microphone-symbolic", Tr("lng_shortcuts_record_voice_message").c_str(), "flat");
  gtk_widget_add_css_class(_mic, "circular");
  // Only the keyboard gets here (the gesture takes presses): it records without holding.
  _signals.clicked(_mic, [this] {
    start();
    lock();
  });
  auto *field = gtk_widget_get_parent(sendButton);
  _sendStack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(_sendStack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_widget_set_valign(_sendStack, gtk_widget_get_valign(sendButton));
  g_object_ref(sendButton);
  gtk_box_remove(GTK_BOX(field), sendButton);
  gtk_stack_add_named(GTK_STACK(_sendStack), _mic, "mic");
  gtk_stack_add_named(GTK_STACK(_sendStack), sendButton, "send");
  g_object_unref(sendButton);
  gtk_box_append(GTK_BOX(field), _sendStack);

  // While the finger holds: "slide to cancel" and the microphone under it; locked: Delete and Send.
  _bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(_bar, "composer");
  gtk_widget_add_css_class(_bar, "record-bar");
  _cancel = MakeIconButton("user-trash-symbolic", Tr("lng_record_lock_delete").c_str(), "flat");
  gtk_widget_add_css_class(_cancel, "circular");
  gtk_widget_set_valign(_cancel, GTK_ALIGN_CENTER);
  _signals.clicked(_cancel, [this] { finish(false); });
  gtk_box_append(GTK_BOX(_bar), _cancel);
  _dot = gtk_label_new("●");
  gtk_widget_add_css_class(_dot, "recording-dot");
  gtk_widget_set_margin_start(_dot, 6);
  gtk_box_append(GTK_BOX(_bar), _dot);
  _time = gtk_label_new(RecordTime(0).c_str());
  gtk_widget_add_css_class(_time, "numeric");
  gtk_label_set_xalign(GTK_LABEL(_time), 0);
  gtk_label_set_width_chars(GTK_LABEL(_time), 6);
  gtk_box_append(GTK_BOX(_bar), _time);
  _hint = gtk_label_new(("‹  " + Tr("lng_record_cancel_recording")).c_str());
  gtk_widget_add_css_class(_hint, "dim-label");
  gtk_widget_set_hexpand(_hint, TRUE);
  gtk_label_set_xalign(GTK_LABEL(_hint), 1);
  gtk_widget_set_margin_end(_hint, kHintMargin);
  gtk_label_set_ellipsize(GTK_LABEL(_hint), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(_bar), _hint);
  _send = MakeIconButton("contrail-send-symbolic", Tr("lng_send_button").c_str(), "suggested-action");
  gtk_widget_add_css_class(_send, "circular");
  gtk_widget_set_valign(_send, GTK_ALIGN_CENTER);
  _signals.clicked(_send, [this] { finish(true); });
  gtk_box_append(GTK_BOX(_bar), _send);
  _micIndicator = gtk_image_new_from_icon_name("audio-input-microphone-symbolic");
  gtk_widget_add_css_class(_micIndicator, "record-mic");
  gtk_widget_set_valign(_micIndicator, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(_bar), _micIndicator);
  gtk_stack_add_named(GTK_STACK(_composerStack), _bar, "record");

  // Slide up to lock: above the microphone while the finger holds.
  _lockHint = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class(_lockHint, "osd");
  gtk_widget_add_css_class(_lockHint, "record-lock");
  gtk_box_append(GTK_BOX(_lockHint), gtk_image_new_from_icon_name("changes-prevent-symbolic"));
  gtk_box_append(GTK_BOX(_lockHint), gtk_image_new_from_icon_name("go-up-symbolic"));
  gtk_widget_set_tooltip_text(_lockHint, Tr("lng_record_lock").c_str());
  gtk_widget_set_halign(_lockHint, GTK_ALIGN_END);
  gtk_widget_set_valign(_lockHint, GTK_ALIGN_END);
  gtk_widget_set_margin_end(_lockHint, 12);
  gtk_widget_set_can_target(_lockHint, FALSE);
  gtk_widget_set_visible(_lockHint, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(gtk_widget_get_parent(_toBottom)), _lockHint);
  setupGesture(composer);

  // Recorded on while the window was closed: ready to be sent or deleted. Another chat opened ends it.
  if (auto *recorder = this->recorder(); recorder && recorder->active()) {
    if (recorder->chat() == _chat.chat() && recorder->topic() == _chat.topic()) {
      _recording = true;
      _locked = true;
      attach();
    } else {
      recorder->finish(false);
    }
  }
}

VoiceRecordBar::~VoiceRecordBar() {
  // A voice message being recorded goes on without the chat.
  _recordLifetime.reset();
  _signals.clear();
}

VoiceRecorder *VoiceRecordBar::recorder() const {
  return _chat.window().recorder();
}

void VoiceRecordBar::setFieldEmpty(bool empty) {
  _fieldEmpty = empty;
  gtk_stack_set_visible_child_name(GTK_STACK(_sendStack), empty ? "mic" : "send");
}

void VoiceRecordBar::composerShown(bool writable) {
  if (!_recording) return;
  if (writable) {
    gtk_stack_set_visible_child_name(GTK_STACK(_composerStack), "record");
  } else {
    finish(false);
  }
}

bool VoiceRecordBar::cancel() {
  if (!_recording) return false;
  finish(false);
  return true;
}

// The gesture is on the whole composer, which stays on the screen when the microphone is replaced with the bar under
// the finger.
void VoiceRecordBar::setupGesture(GtkWidget *composer) {
  auto *gesture = gtk_gesture_drag_new();
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(gesture), GTK_PHASE_CAPTURE);
  _signals.connect<void(GtkGestureDrag *, double, double)>(
      gesture, "drag-begin", [this](GtkGestureDrag *drag, double x, double y) {
        graphene_rect_t bounds;
        const auto point = GRAPHENE_POINT_INIT(float(x), float(y));
        const auto onMic = !_recording && gtk_widget_get_mapped(_mic) && gtk_widget_get_sensitive(_mic)
                           && gtk_widget_compute_bounds(_mic, _composer, &bounds)
                           && graphene_rect_contains_point(&bounds, &point);
        if (!onMic) {
          gtk_gesture_set_state(GTK_GESTURE(drag), GTK_EVENT_SEQUENCE_DENIED);
          return;
        }
        gtk_gesture_set_state(GTK_GESTURE(drag), GTK_EVENT_SEQUENCE_CLAIMED);
        start();
        _holding = _recording;
        showState();
      });
  _signals.connect<void(GtkGestureDrag *, double, double)>(gesture, "drag-update",
                                                           [this](GtkGestureDrag *, double dx, double dy) {
                                                             if (_holding) drag(dx, dy);
                                                           });
  _signals.connect<void(GtkGestureDrag *, double, double)>(gesture, "drag-end",
                                                           [this](GtkGestureDrag *, double, double) { release(); });
  _signals.connect<void(GtkGesture *, GdkEventSequence *)>(gesture, "cancel", [this](GtkGesture *, GdkEventSequence *) {
    if (std::exchange(_holding, false)) finish(false);
  });
  gtk_widget_add_controller(composer, GTK_EVENT_CONTROLLER(gesture));
}

void VoiceRecordBar::start() {
  if (_recording || !_chat.canRecordVoice()) return;
  auto *recorder = this->recorder();
  if (!recorder || !recorder->start(_chat.chat(), _chat.topic(), _chat.replyToId())) {
    ShowToast(Tr("lng_record_audio_problem"));
    return;
  }
  // What plays would be recorded too.
  if (auto *player = _chat.window().player(); player && player->state().status == AudioPlayer::Status::Playing) {
    player->playPause(player->state().track);
  }
  _recording = true;
  _locked = false;
  _started = g_get_monotonic_time();
  TriggerFeedback("button-pressed");
  attach();
}

// The bar follows the recorder, which may have been started before this chat was built (the window was closed).
void VoiceRecordBar::attach() {
  _recordLifetime.reset();
  gtk_stack_set_visible_child_name(GTK_STACK(_composerStack), "record");
  showState();
  auto *recorder = this->recorder();
  const auto shown = [this](const VoiceRecorder::State &state) {
    if (!_recording) return;
    gtk_label_set_text(GTK_LABEL(_time), RecordTime(state.duration).c_str());
    // The dot breathes with the voice.
    gtk_widget_set_opacity(_dot, 0.35 + 0.65 * std::min(1., state.level / 6000.));
  };
  shown(recorder->state());
  recorder->onState(shown, _recordLifetime);
  recorder->onFailed([this] {
    if (!_recording) return;
    _recording = _holding = _locked = false;
    showState();
    _chat.recordingEnded(false);
    ShowToast(Tr("lng_record_audio_problem"));
  }, _recordLifetime);
}

void VoiceRecordBar::lock() {
  if (!_recording) return;
  _holding = false;
  _locked = true;
  TriggerFeedback("button-pressed");
  showState();
}

void VoiceRecordBar::showState() {
  const auto holding = _recording && !_locked;
  gtk_widget_set_visible(_cancel, _locked);
  gtk_widget_set_visible(_send, _locked);
  gtk_widget_set_visible(_micIndicator, holding);
  gtk_widget_set_opacity(_hint, _locked ? 0. : 1.);
  gtk_widget_set_margin_end(_hint, kHintMargin);
  gtk_widget_set_visible(_lockHint, holding);
  gtk_widget_set_margin_bottom(_lockHint, kLockMargin);
  gtk_widget_set_opacity(_dot, 1.);
  if (holding) gtk_widget_set_visible(_toBottom, FALSE);
}

void VoiceRecordBar::drag(double dx, double dy) {
  if (dx < -kCancelDistance) {
    _holding = false;
    finish(false);
    TriggerFeedback("button-pressed");
    return;
  }
  if (dy < -kLockDistance) {
    lock();
    return;
  }
  // "Slide to cancel" follows the finger and fades; the lock rises with it.
  const auto left = std::clamp(-dx, 0., kCancelDistance);
  gtk_widget_set_margin_end(_hint, kHintMargin + int(left));
  gtk_widget_set_opacity(_hint, 1. - 0.75 * left / kCancelDistance);
  const auto up = std::clamp(-dy, 0., kLockDistance);
  gtk_widget_set_margin_bottom(_lockHint, kLockMargin + int(up / 2));
}

void VoiceRecordBar::release() {
  if (!std::exchange(_holding, false)) return;
  if (g_get_monotonic_time() - _started < kMinHold) {
    // A tap: Telegram explains that the button is to be held.
    finish(false);
    ShowToast(Tr("lng_record_hold_tip"));
  } else {
    finish(true);
  }
}

void VoiceRecordBar::finish(bool send) {
  if (!_recording) return;
  _recording = _holding = _locked = false;
  _recordLifetime.reset();
  showState();
  if (auto *recorder = this->recorder()) recorder->finish(send);
  _chat.recordingEnded(send);
}

void VoiceRecordBar::debug(const std::string &what) {
  const auto gap = what.find(' ');
  const auto name = what.substr(0, gap);
  const auto rest = gap == std::string::npos ? std::string() : what.substr(gap + 1);
  if (name == "state") {
    const auto *recorder = this->recorder();
    // With a point whatever the locale: the tests read it.
    char hint[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(hint, sizeof(hint), "%.2f", gtk_widget_get_opacity(_hint));
    g_message("voice: recording=%d holding=%d locked=%d mic=%d bar=%d time=%s hint=%s lock=%d %s", int(_recording),
              int(_holding), int(_locked),
              int(g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(_sendStack)), "mic") == 0),
              int(g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(_composerStack)), "record") == 0),
              gtk_label_get_text(GTK_LABEL(_time)), hint, int(gtk_widget_get_visible(_lockHint)),
              recorder ? recorder->debugState().c_str() : "");
  } else if (name == "start") {
    start();
    lock();
  } else if (name == "hold") {
    // As a finger on the microphone.
    if (!_fieldEmpty) return;
    start();
    _holding = _recording;
    showState();
  } else if (name == "drag") {
    char *end = nullptr;
    const auto dx = g_ascii_strtod(rest.c_str(), &end);
    const auto dy = g_ascii_strtod(end, nullptr);
    if (_holding) drag(dx, dy);
  } else if (name == "release") {
    release();
  } else if (name == "send" || name == "delete") {
    finish(name == "send");
  }
}

}  // namespace Contrail
