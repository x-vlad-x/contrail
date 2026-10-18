#include "calls.hpp"

#include "main_window.hpp"
#include "layout.hpp"
#include "call_layout.hpp"
#include "userpics.hpp"

#include "calls/calls_call.h"
#include "calls/calls_emoji_fingerprint.h"
#include "calls/calls_instance.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "main/main_session.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "settings.h"
#include "ui/emoji_config.h"
#include "ui/text/format_values.h"
#include "webrtc/webrtc_video_track.h"
#include "webrtc/webrtc_environment.h"
#include "ui/userpic_view.h"
#include "window/notifications_utilities.h"

#include <QtGui/QPainter>

namespace Contrail {
namespace {

QString CallStatus(Calls::Call *call) {
  using State = Calls::Call::State;
  switch (call->state()) {
    case State::Established: return Ui::FormatDurationText(call->getDurationMs() / 1000);
    case State::Starting:
    case State::WaitingIncoming: return call->type() == Calls::Call::Type::Incoming
        ? tr::lng_call_status_incoming(tr::now) : tr::lng_call_status_connecting(tr::now);
    case State::WaitingInit:
    case State::WaitingInitAck: return tr::lng_call_status_connecting(tr::now);
    case State::ExchangingKeys: return tr::lng_call_status_exchanging(tr::now);
    case State::Requesting: return tr::lng_call_status_requesting(tr::now);
    case State::Ringing: return tr::lng_call_status_ringing(tr::now);
    case State::Busy: return tr::lng_call_status_busy(tr::now);
    case State::Failed: return tr::lng_call_status_failed(tr::now);
    case State::HangingUp:
    case State::FailedHangingUp:
    case State::MigrationHangingUp: return tr::lng_call_status_hanging(tr::now);
    case State::Ended:
    case State::EndedByOtherDevice: return tr::lng_call_status_ended(tr::now);
    default: return tr::lng_call_status_waiting(tr::now);
  }
}

GtkWidget *CallButton(const QString &text, const char *css) {
  auto *button = gtk_button_new_with_label(U8(text).c_str());
  gtk_widget_add_css_class(button, "pill");
  gtk_widget_add_css_class(button, css);
  return button;
}

bool PhoneCameraName(const QString &name) {
  return name.contains(u"front"_q, Qt::CaseInsensitive)
      || name.contains(u"back"_q, Qt::CaseInsensitive)
      || name.contains(u"rear"_q, Qt::CaseInsensitive);
}

std::vector<Webrtc::DeviceInfo> CallCameras() {
  auto cameras = Core::App().mediaDevices().devices(Webrtc::DeviceType::Camera);
  std::erase_if(cameras, [](const auto &camera) { return camera.inactive; });
  const auto phone = std::any_of(cameras.begin(), cameras.end(), [](const auto &camera) { return PhoneCameraName(camera.name); });
  if (phone) std::erase_if(cameras, [](const auto &camera) { return !PhoneCameraName(camera.name); });
  return cameras;
}

using OutputKind = AudioRoute::Kind;

const char *OutputIcon(OutputKind kind) {
  switch (kind) {
    case OutputKind::Earpiece: return "call-start-symbolic";
    case OutputKind::Speaker: return "audio-speakers-symbolic";
    case OutputKind::Headphones: return "audio-headphones-symbolic";
    case OutputKind::Bluetooth: return "bluetooth-active-symbolic";
    default: return "audio-card-symbolic";
  }
}

QString OutputName(const AudioRoute::Output &output) {
  if (!output.name.isEmpty()) return output.name;
  // Telegram Desktop has no strings for a phone's own outputs.
  const auto language = Lang::LanguageIdOrDefault(Lang::Id());
  const auto russian = language.startsWith(u"ru"_q), ukrainian = language.startsWith(u"uk"_q);
  switch (output.kind) {
    case OutputKind::Earpiece: return (russian || ukrainian) ? u"Телефон"_q : u"Phone"_q;
    case OutputKind::Speaker: return russian ? u"Динамик"_q : ukrainian ? u"Гучномовець"_q : u"Speaker"_q;
    default: return russian ? u"Наушники"_q : ukrainian ? u"Навушники"_q : u"Headphones"_q;
  }
}

} // namespace

CallView::CallView(not_null<MainWindow *> window)
    : _parent(window)
    , _audio([=] { updateAudioRoute(); }, cWorkingDir() + u"tdata/contrail-call-volumes.ini"_q)
    , _proximity([=](bool near) { setProximityShield(near); })
    , _notification({
          .accept = [=] { crl::on_main(this, [=] {
            if (const auto call = _call.get(); call && !_ending && call->isIncomingWaiting()) call->answer();
          }); },
          .decline = [=] { crl::on_main(this, [=] { hangup(); }); },
          .hangup = [=] { crl::on_main(this, [=] { hangup(); }); },
          .open = [=] { crl::on_main(this, [=] { _parent->present(); present(); }); },
      })
    , _lockScreen({
          .accept = [=] {
            if (const auto call = _call.get(); call && !_ending && call->isIncomingWaiting()) call->answer();
          },
          .hangup = [=] { hangup(); },
          .mute = [=] {
            if (const auto call = _call.get(); call && !_ending) call->setMuted(!call->muted());
          },
          .speaker = [=] { if (!_ending) toggleSpeaker(); },
      })
    , _screenLock([=](bool locked) {
        crl::on_main(this, [=] {
          updateLockScreen();
          // Unlocked during the call: the call window is in front.
          if (!locked && _call && !_ending) { _parent->present(); present(); }
        });
      })
    , _timer([=] { refresh(); }) {
  _window = adw_window_new();
  g_object_ref_sink(_window);
  gtk_widget_add_css_class(_window, "call-window");
  gtk_window_set_title(GTK_WINDOW(_window), U8(tr::lng_call_box_title(tr::now)).c_str());
  gtk_window_set_default_size(GTK_WINDOW(_window), 380, 620);
  gtk_widget_set_size_request(_window, 320, 294);
  gtk_window_set_transient_for(GTK_WINDOW(_window), GTK_WINDOW(window->widget()));
  _signals.connect<gboolean(GtkWindow *)>(_window, "close-request", [=](GtkWindow *) -> gboolean {
    gtk_widget_set_visible(_window, FALSE);
    return TRUE;
  });

  _video = gtk_picture_new();
  _localVideo = gtk_picture_new();
  auto *stage = MakeCallVideoStage(_video, _localVideo, &_localFrame);
  auto *header = adw_header_bar_new();
  adw_header_bar_set_show_title(ADW_HEADER_BAR(header), FALSE);
  gtk_widget_add_css_class(header, "flat");
  gtk_widget_add_css_class(header, "call-header");
  gtk_widget_set_valign(header, GTK_ALIGN_START);
  gtk_overlay_add_overlay(GTK_OVERLAY(stage), header);

  auto *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_valign(info, GTK_ALIGN_START);
  gtk_widget_set_margin_top(info, 52);
  gtk_widget_set_margin_start(info, 16);
  gtk_widget_set_margin_end(info, 16);
  gtk_widget_set_can_target(info, FALSE);
  gtk_widget_add_css_class(info, "call-info");
  _name = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(_name), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(_name, "title-2");
  gtk_box_append(GTK_BOX(info), _name);
  _status = gtk_label_new("");
  ConfigureMessageWrapping(GTK_LABEL(_status));
  gtk_label_set_justify(GTK_LABEL(_status), GTK_JUSTIFY_CENTER);
  gtk_box_append(GTK_BOX(info), _status);
  _fingerprint = gtk_label_new("");
  gtk_widget_add_css_class(_fingerprint, "title-2");
  gtk_box_append(GTK_BOX(info), _fingerprint);
  _remoteMuted = gtk_label_new("");
  ConfigureMessageWrapping(GTK_LABEL(_remoteMuted));
  gtk_label_set_justify(GTK_LABEL(_remoteMuted), GTK_JUSTIFY_CENTER);
  gtk_box_append(GTK_BOX(info), _remoteMuted);
  gtk_overlay_add_overlay(GTK_OVERLAY(stage), info);

  _avatarBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_halign(_avatarBox, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(_avatarBox, GTK_ALIGN_CENTER);
  gtk_widget_set_can_target(_avatarBox, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(stage), _avatarBox);

  auto *controls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_valign(controls, GTK_ALIGN_END);
  gtk_widget_set_halign(controls, GTK_ALIGN_CENTER);
  SetMarginAll(controls, 12);
  gtk_widget_add_css_class(controls, "call-controls");
  _accept = CallButton(tr::lng_call_accept(tr::now), "suggested-action");
  gtk_box_append(GTK_BOX(controls), _accept);
  auto *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  const auto button = [&](const char *icon, const QString &text) {
    auto *result = MakeIconButton(icon, U8(text).c_str());
    gtk_widget_add_css_class(result, "circular");
    gtk_widget_add_css_class(result, "call-control");
    gtk_box_append(GTK_BOX(buttons), result);
    return result;
  };
  _mute = button("audio-input-microphone-symbolic", tr::lng_call_mute_audio(tr::now));
  _camera = button("camera-video-symbolic", tr::lng_call_start_video(tr::now));
  _switchCamera = button("view-refresh-symbolic", tr::lng_settings_call_camera(tr::now));
  _speaker = button("audio-speakers-symbolic", tr::lng_settings_call_section_output(tr::now));
  _hangup = button("call-stop-symbolic", tr::lng_call_end_call(tr::now));
  gtk_widget_add_css_class(_hangup, "destructive-action");
  gtk_box_append(GTK_BOX(controls), buttons);
  gtk_overlay_add_overlay(GTK_OVERLAY(stage), controls);
  // While the phone is held to the ear the screen is black and the cheek can't press the buttons.
  _shield = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(_shield, "call-proximity-shield");
  gtk_widget_set_visible(_shield, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(stage), _shield);
  adw_window_set_content(ADW_WINDOW(_window), stage);

  _signals.clicked(_accept, [=] {
    if (const auto call = _call.get(); call && !_ending) {
      if (call->state() == Calls::Call::Busy) call->redial();
      else call->answer();
    }
  });
  _signals.clicked(_hangup, [=] { hangup(); });
  _signals.clicked(_mute, [=] {
    if (const auto call = _call.get(); call && !_ending) {
      call->setMuted(!call->muted());
      if (g_getenv("CONTRAIL_DEBUG")) g_message("Microphone toggled: %s", call->muted() ? "muted" : "unmuted");
    }
  });
  _signals.clicked(_camera, [=] {
    if (const auto call = _call.get(); call && !_ending && call->state() == Calls::Call::Established) {
      call->toggleCameraSharing(!call->isSharingCamera());
      refresh();
    }
  });
  _signals.clicked(_switchCamera, [=] { switchCamera(); });
  _signals.clicked(_speaker, [=] { if (!_ending) chooseAudioOutput(); });

  Core::App().calls().currentCallValue() | rpl::on_next([=](Calls::Call *call) {
    setCall(call);
  }, _lifetime);
  Core::App().mediaDevices().changes(Webrtc::DeviceType::Camera)
      | rpl::on_next([=] { refreshDevices(); }, _lifetime);
}

CallView::~CallView() {
  invalidate_weak_ptrs(this);
  _proximity.setEnabled(false);
  _lifetime.destroy();
  _callLifetime.destroy();
  stopVideo();
  _audio.end();
  _timer.cancel();
  _signals.clear();
  gtk_window_destroy(GTK_WINDOW(_window));
  g_object_unref(_window);
}

void CallView::choosePhoneCamera() {
  const auto cameras = CallCameras();
  const auto current = Core::App().settings().cameraDeviceId();
  const Webrtc::DeviceInfo *front = nullptr;
  for (const auto &camera : cameras) {
    if (camera.inactive) continue;
    const auto name = camera.name.toLower();
    const auto phoneCamera = PhoneCameraName(name);
    if (camera.id == current && phoneCamera) return;
    if (!front && name.contains(u"front"_q)) front = &camera;
  }
  // Android HAL loopback cameras are usable; the first enumerated MediaTek codec/dpe device is not.
  if (front) Core::App().settings().setCameraDeviceId(front->id);
}

void CallView::start(not_null<UserData *> user, bool video) {
  auto &calls = Core::App().calls();
  if (calls.currentCall()) { present(); return; }
  if (calls.inGroupCall() || user->isSelf() || user->isBot()) return;
  if (user->callsStatus() == UserData::CallsStatus::Private) {
    user->updateFull();
    ShowToast(tr::lng_call_error_not_available(tr::now, lt_user, user->name()));
    return;
  }
  if (user->callsStatus() == UserData::CallsStatus::Disabled) {
    ShowToast(tr::lng_call_error_outdated(tr::now, lt_user, user->name()));
    return;
  }
  choosePhoneCamera();
  calls.startOutgoingCall(user, {.video = video, .isConfirmed = true});
}

void CallView::present() {
  if (_call) gtk_window_present(GTK_WINDOW(_window));
}

void CallView::stopVideo() {
  _videoLifetime.destroy();
  _remoteHasFrame = false;
  gtk_picture_set_paintable(GTK_PICTURE(_video), nullptr);
  gtk_picture_set_paintable(GTK_PICTURE(_localVideo), nullptr);
  gtk_widget_set_visible(_video, FALSE);
  gtk_widget_set_visible(_localFrame, FALSE);
  gtk_widget_set_visible(_avatarBox, TRUE);
}

void CallView::hangup() {
  const auto call = _call.get();
  if (!call) {
    _audio.end();
    _notification.close();
    _ringer.stop();
    _lockScreen.hide();
    gtk_widget_set_visible(_window, FALSE);
    return;
  }
  if (_ending) return;
  _ending = true;
  stopVideo();
  _timer.cancel();
  refresh();
  call->hangup();
}

void CallView::setCall(Calls::Call *call) {
  _callLifetime.destroy();
  stopVideo();
  _timer.cancel();
  if (!call) {
    if (const auto previous = _call.get()) ShowToast(CallStatus(previous));
    _call.reset();
    _audio.end();
    updateProximity();
    _idle.set(false);
    // Contrail has no setting for the calls' output device: each call follows its own route.
    _playbackOutput.clear();
    Core::App().settings().setCallPlaybackDeviceId(QString());
    Core::App().saveSettingsDelayed();
    _parent->setCallStatus(QString(), false);
    _notification.close();
    _ringer.stop();
    _lockScreen.hide();
    _audioStarted = false;
    gtk_widget_set_visible(_window, FALSE);
    return;
  }
  _call = call;
  _audioStarted = false;
  HideCoreWindow();
  _ending = false;
  _localSourceSize = _remoteSourceSize = QSize();
  _playbackOutput.clear();
  Core::App().settings().setCallPlaybackDeviceId(QString());
  choosePhoneCamera();
  ClearBox(_avatarBox);
  gtk_box_append(GTK_BOX(_avatarBox), MakeAvatar(call->user(), 112, true));
  gtk_label_set_text(GTK_LABEL(_name), U8(call->user()->name()).c_str());
  gtk_widget_set_sensitive(_accept, TRUE);
  call->stateValue() | rpl::on_next([=] { refresh(); }, _callLifetime);
  call->mutedValue() | rpl::on_next([=] { refresh(); }, _callLifetime);
  call->remoteAudioStateValue() | rpl::on_next([=] { refresh(); }, _callLifetime);
  call->errors() | rpl::on_next([=](const Calls::Error &error) {
    const auto text = error.type == Calls::ErrorType::NoCamera ? tr::lng_call_error_no_camera(tr::now)
        : error.type == Calls::ErrorType::NoMicrophone ? tr::lng_call_error_audio_io(tr::now)
        : error.details.isEmpty() ? tr::lng_call_status_failed(tr::now) : error.details;
    ShowToast(text);
  }, _callLifetime);
  const auto weak = _call;
  const auto attachVideo = [=](not_null<Webrtc::VideoTrack *> track, GtkWidget *picture) {
    track->stateValue() | rpl::on_next([=](Webrtc::VideoState state) {
      if (!weak || _call != weak || _ending) return;
      if (state != Webrtc::VideoState::Active) {
        gtk_picture_set_paintable(GTK_PICTURE(picture), nullptr);
        gtk_widget_set_visible(picture == _localVideo ? _localFrame : picture, FALSE);
        if (picture == _video) {
          _remoteHasFrame = false;
          gtk_widget_set_visible(_avatarBox, TRUE);
        }
      }
      refreshDevices();
    }, _videoLifetime);
    track->renderNextFrame() | rpl::on_next([=] {
      if (weak && _call == weak && !_ending) showFrame(track, picture);
    }, _videoLifetime);
  };
  attachVideo(call->videoIncoming(), _video);
  attachVideo(call->videoOutgoing(), _localVideo);
  _idle.set(true);
  _timer.callEach(1000);
  _parent->present();
  present();
}

void CallView::refresh() {
  const auto call = _call.get();
  if (!call) return;
  const auto incoming = call->isIncomingWaiting();
  const auto busy = call->state() == Calls::Call::Busy;
  const auto established = call->state() == Calls::Call::Established;
  const auto final = call->state() == Calls::Call::Ended || call->state() == Calls::Call::EndedByOtherDevice
      || call->state() == Calls::Call::Failed;
  const auto finishing = _ending || final || call->state() == Calls::Call::HangingUp
      || call->state() == Calls::Call::FailedHangingUp || call->state() == Calls::Call::MigrationHangingUp;
  if (finishing && !_ending) { _ending = true; stopVideo(); }
  // An incoming call rings on the loudspeaker: the call's own route (the earpiece) only once answered.
  if (!incoming && !finishing && !_audioStarted) {
    _audioStarted = true;
    _audio.begin(call->isSharingCamera());
  }
  if (incoming && !finishing) _ringer.start();
  else _ringer.stop();
  const auto status = CallStatus(call);
  gtk_label_set_text(GTK_LABEL(_status), U8(status).c_str());
  gtk_widget_set_visible(_accept, (incoming || busy) && !finishing);
  gtk_button_set_label(GTK_BUTTON(_accept), U8(busy ? tr::lng_call_redial(tr::now)
      : tr::lng_call_accept(tr::now)).c_str());
  gtk_widget_set_sensitive(_mute, !finishing && !busy);
  gtk_widget_set_sensitive(_camera, established && !finishing);
  gtk_widget_set_sensitive(_hangup, !finishing);
  gtk_widget_set_tooltip_text(_hangup, U8(incoming ? tr::lng_call_decline(tr::now)
      : tr::lng_call_end_call(tr::now)).c_str());
  gtk_widget_set_tooltip_text(_mute, U8(call->muted() ? tr::lng_call_unmute_audio(tr::now)
      : tr::lng_call_mute_audio(tr::now)).c_str());
  gtk_button_set_icon_name(GTK_BUTTON(_mute), call->muted()
      ? "microphone-sensitivity-muted-symbolic" : "audio-input-microphone-symbolic");
  if (call->muted()) gtk_widget_add_css_class(_mute, "active");
  else gtk_widget_remove_css_class(_mute, "active");
  gtk_widget_set_tooltip_text(_camera, U8(call->isSharingCamera() ? tr::lng_call_stop_video(tr::now)
      : tr::lng_call_start_video(tr::now)).c_str());
  if (call->isSharingCamera()) gtk_widget_add_css_class(_camera, "active");
  else gtk_widget_remove_css_class(_camera, "active");
  const auto remoteMuted = call->remoteAudioState() == Calls::Call::RemoteAudioState::Muted;
  gtk_widget_set_visible(_remoteMuted, remoteMuted && !finishing);
  if (remoteMuted) gtk_label_set_text(GTK_LABEL(_remoteMuted),
      U8(tr::lng_call_microphone_off(tr::now, lt_user, call->user()->name())).c_str());
  gtk_widget_set_visible(_fingerprint, call->isKeyShaForFingerprintReady() && !finishing);
  if (call->isKeyShaForFingerprintReady() && !finishing) {
    QString text;
    for (const auto emoji : Calls::ComputeEmojiFingerprint(call)) text += emoji->text() + ' ';
    gtk_label_set_text(GTK_LABEL(_fingerprint), U8(text.trimmed()).c_str());
    gtk_widget_set_tooltip_text(_fingerprint,
        U8(tr::lng_call_fingerprint_tooltip(tr::now, lt_user, call->user()->name())).c_str());
  }
  refreshDevices();
  updateProximity();
  updateNotification();
  updateLockScreen();
  _parent->setCallStatus(call->user()->name() + u" · "_q + status, !final);
  if (final) {
    _timer.cancel();
    crl::on_main(call, [=] { if (call->ratingInPanel()) call->finishRating(); });
  }
}

void CallView::updateNotification() {
  const auto call = _call.get();
  if (!call || _ending) {
    _notification.close();
  } else if (call->isIncomingWaiting()) {
    Ui::PeerUserpicView userpic;
    _notification.showIncoming(call->user()->name(), tr::lng_call_incoming(tr::now),
        Window::Notifications::GenerateUserpic(call->user(), userpic));
  } else {
    _notification.showOngoing(call->user()->name(), CallStatus(call));
  }
}

void CallView::updateLockScreen() {
  const auto call = _call.get();
  if (!call || _ending || !_screenLock.locked()) {
    _lockScreen.hide();
    return;
  }
  if (!_lockScreen.shown()) _lockScreen.show(call->user(), call->user()->name());
  const auto incoming = call->isIncomingWaiting();
  const auto output = _audio.current();
  _lockScreen.update(incoming ? tr::lng_call_incoming(tr::now) : CallStatus(call), incoming, call->muted(),
      output && output->kind == OutputKind::Speaker);
  _lockScreen.setShield(_proximity.near());
}

// Over the lock screen: between the earpiece and the loudspeaker only.
void CallView::toggleSpeaker() {
  const auto current = _audio.current();
  const auto speaker = current && current->kind == OutputKind::Speaker;
  for (const auto &output : _audio.outputs()) {
    if (output.kind == (speaker ? OutputKind::Earpiece : OutputKind::Speaker)) {
      _audio.select(output.id);
      return;
    }
  }
}

void CallView::showFrame(not_null<Webrtc::VideoTrack *> track, GtkWidget *picture) {
  if (!_call || _ending || track->state() != Webrtc::VideoState::Active) return;
  // Hidden Qt panels leave the track in its default full-resolution ARGB format. Reading with
  // false would turn that conversion off and break the core's software renderer on later frames.
  const auto data = track->frameWithInfo(true);
  auto image = data.original;
  if (data.format == Webrtc::FrameFormat::YUV420 && data.yuv420) {
    const auto &yuv = *data.yuv420;
    const unsigned char *planes[3] = {static_cast<const unsigned char *>(yuv.y.data),
        static_cast<const unsigned char *>(yuv.u.data), static_cast<const unsigned char *>(yuv.v.data)};
    const int strides[3] = {yuv.y.stride, yuv.u.stride, yuv.v.stride};
    auto &converter = picture == _localVideo ? _outgoingConverter : _incomingConverter;
    image = converter.fromYuv420(planes, strides, yuv.size.width(), yuv.size.height());
  }
  auto front = false;
  if (picture == _localVideo) {
    const auto current = Core::App().settings().cameraDeviceId();
    for (const auto &camera : Core::App().mediaDevices().devices(Webrtc::DeviceType::Camera)) {
      if (camera.id == current) front = camera.name.contains(u"front"_q, Qt::CaseInsensitive);
    }
  }
  image = OrientVideoImage(image, data.rotation, front);
  auto &lastSize = picture == _localVideo ? _localSourceSize : _remoteSourceSize;
  if (!image.isNull() && lastSize != image.size()) {
    lastSize = image.size();
    if (g_getenv("CONTRAIL_DEBUG")) g_message("%s video: %dx%d, rotation=%d",
        picture == _localVideo ? "Local" : "Remote", image.width(), image.height(), data.rotation);
  }
  if (auto *texture = ToTexture(image)) {
    gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(texture));
    g_object_unref(texture);
    gtk_widget_set_visible(picture == _localVideo ? _localFrame : picture, TRUE);
    if (picture == _video) { _remoteHasFrame = true; gtk_widget_set_visible(_avatarBox, FALSE); }
  }
  track->markFrameShown();
}

void CallView::refreshDevices() {
  const auto call = _call.get();
  const auto cameras = CallCameras();
  const auto count = std::count_if(cameras.begin(), cameras.end(), [](const auto &camera) { return !camera.inactive; });
  gtk_widget_set_sensitive(_switchCamera, call && !_ending && call->isSharingCamera() && count > 1);
  gtk_widget_set_sensitive(_speaker, call && !_ending && _audio.outputs().size() > 1);
}

void CallView::switchCamera() {
  const auto call = _call.get();
  if (!call || _ending || !call->isSharingCamera()) return;
  const auto cameras = CallCameras();
  const auto current = Core::App().settings().cameraDeviceId();
  const auto from = std::find_if(cameras.begin(), cameras.end(), [&](const auto &camera) { return camera.id == current; });
  const auto front = from != cameras.end() && from->name.contains(u"front"_q, Qt::CaseInsensitive);
  const auto target = std::find_if(cameras.begin(), cameras.end(), [&](const auto &camera) {
    return !camera.inactive && (front
        ? camera.name.contains(u"back"_q, Qt::CaseInsensitive) || camera.name.contains(u"rear"_q, Qt::CaseInsensitive)
        : camera.name.contains(u"front"_q, Qt::CaseInsensitive));
  });
  QString chosen;
  if (target != cameras.end() && target->id != current) chosen = target->id;
  else {
    auto i = from == cameras.end() ? 0 : int(std::distance(cameras.begin(), from)) + 1;
    for (auto tried = 0; tried < int(cameras.size()); ++tried, ++i) {
      const auto &camera = cameras[i % cameras.size()];
      if (!camera.inactive && camera.id != current) { chosen = camera.id; break; }
    }
  }
  if (!chosen.isEmpty()) {
    Core::App().settings().setCameraDeviceId(chosen);
    Core::App().saveSettingsDelayed();
  }
}

void CallView::updateAudioRoute() {
  if (!_speaker) return;
  const auto output = _audio.current();
  gtk_button_set_icon_name(GTK_BUTTON(_speaker), OutputIcon(output ? output->kind : OutputKind::Speaker));
  gtk_widget_set_tooltip_text(_speaker, U8(tr::lng_settings_call_section_output(tr::now)
      + (output ? u'\n' + OutputName(*output) : QString())).c_str());
  if (output && output->kind == OutputKind::Speaker) gtk_widget_add_css_class(_speaker, "active");
  else gtk_widget_remove_css_class(_speaker, "active");
  refreshDevices();
  if (_call && output && output->device != _playbackOutput) {
    // The core plays the call on the chosen device; a phone port is switched on the device itself.
    auto &devices = Core::App().mediaDevices();
    devices.validateDevices(Webrtc::DeviceType::Playback);
    QString id;
    for (const auto &device : devices.devices(Webrtc::DeviceType::Playback)) {
      if (!device.inactive && (device.id == output->device || device.name == output->device)) {
        id = device.id;
        break;
      }
    }
    auto &settings = Core::App().settings();
    if (settings.callPlaybackDeviceId() != id) settings.setCallPlaybackDeviceId(id);
    if (!id.isEmpty()) _playbackOutput = output->device;
    if (g_getenv("CONTRAIL_DEBUG")) {
      g_message("Call audio: %s (%d outputs), playback device \"%s\"", U8(OutputName(*output)).c_str(),
          int(_audio.outputs().size()), U8(id).c_str());
    }
  }
  updateProximity();
  updateLockScreen();
}

void CallView::chooseAudioOutput() {
  const auto &outputs = _audio.outputs();
  const auto current = _audio.current();
  if (outputs.size() < 2) return;
  // Only the earpiece and the loudspeaker: the button switches between them, as in the phone apps.
  if (outputs.size() == 2 && current && !outputs[0].port.isEmpty() && !outputs[1].port.isEmpty()) {
    _audio.select(outputs[0].id == current->id ? outputs[1].id : outputs[0].id);
    return;
  }
  auto *popover = gtk_popover_new();
  auto *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  for (const auto &output : outputs) {
    auto *row = gtk_button_new();
    gtk_widget_add_css_class(row, "flat");
    auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(OutputIcon(output.kind)));
    auto *label = gtk_label_new(U8(OutputName(output)).c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), label);
    auto *check = gtk_image_new_from_icon_name("object-select-symbolic");
    gtk_widget_set_opacity(check, current && current->id == output.id ? 1. : 0.);
    gtk_box_append(GTK_BOX(box), check);
    gtk_button_set_child(GTK_BUTTON(row), box);
    const auto id = output.id;
    OnClicked(row, [=] {
      gtk_popover_popdown(GTK_POPOVER(popover));
      if (_call && !_ending) _audio.select(id);
    });
    gtk_box_append(GTK_BOX(list), row);
  }
  gtk_popover_set_child(GTK_POPOVER(popover), list);
  gtk_widget_set_parent(popover, _speaker);
  DestroyPopoverOnClose(popover);
  gtk_popover_popup(GTK_POPOVER(popover));
}

void CallView::updateProximity() {
  const auto call = _call.get();
  const auto output = _audio.current();
  _proximity.setEnabled(call && !_ending && !call->isIncomingWaiting() && call->state() != Calls::Call::Busy
      && output && output->kind == OutputKind::Earpiece);
}

// As Phosh does for GNOME Calls: the whole screen black, the display itself stays on.
void CallView::setProximityShield(bool near) {
  _lockScreen.setShield(near);
  gtk_widget_set_visible(_shield, near);
  if (near) {
    _shieldFullscreen = !gtk_window_is_fullscreen(GTK_WINDOW(_window));
    if (_shieldFullscreen) gtk_window_fullscreen(GTK_WINDOW(_window));
    gtk_window_present(GTK_WINDOW(_window));
  } else if (_shieldFullscreen) {
    _shieldFullscreen = false;
    gtk_window_unfullscreen(GTK_WINDOW(_window));
  }
}

void CallView::preview(bool incoming, bool video, const QString &name) {
  if (_call) return;
  _ending = false;
  stopVideo();
  ClearBox(_avatarBox);
  gtk_box_append(GTK_BOX(_avatarBox), adw_avatar_new(112, U8(name).c_str(), TRUE));
  gtk_label_set_text(GTK_LABEL(_name), U8(name).c_str());
  gtk_label_set_text(GTK_LABEL(_status), U8(incoming ? tr::lng_call_status_incoming(tr::now)
      : tr::lng_call_status_connecting(tr::now)).c_str());
  gtk_widget_set_visible(_remoteMuted, FALSE);
  gtk_widget_set_visible(_fingerprint, FALSE);
  gtk_widget_set_visible(_accept, incoming);
  gtk_button_set_label(GTK_BUTTON(_accept), U8(tr::lng_call_accept(tr::now)).c_str());
  // Their handlers do nothing without a call; the others act on the devices.
  for (auto *button : {_accept, _mute, _camera}) gtk_widget_set_sensitive(button, TRUE);
  for (auto *button : {_switchCamera, _speaker}) gtk_widget_set_sensitive(button, FALSE);
  gtk_widget_set_sensitive(_hangup, TRUE);
  gtk_widget_set_tooltip_text(_hangup, U8(tr::lng_close(tr::now)).c_str());
  if (video) {
    auto image = QImage(720, 1280, QImage::Format_RGB32);
    image.fill(QColor(28, 53, 65));
    auto painter = QPainter(&image);
    for (auto y = 0; y < 1280; y += 80) {
      for (auto x = 0; x < 720; x += 80) {
        if ((x / 80 + y / 80) % 2) painter.fillRect(x, y, 80, 80, QColor(47, 91, 106));
      }
    }
    painter.end();
    if (auto *texture = ToTexture(image)) {
      gtk_picture_set_paintable(GTK_PICTURE(_video), GDK_PAINTABLE(texture));
      gtk_picture_set_paintable(GTK_PICTURE(_localVideo), GDK_PAINTABLE(texture));
      g_object_unref(texture);
    }
    gtk_widget_set_visible(_video, TRUE);
    gtk_widget_set_visible(_localFrame, TRUE);
    gtk_widget_set_visible(_avatarBox, FALSE);
  }
  if (incoming) {
    _notification.showIncoming(name, tr::lng_call_incoming(tr::now), QImage());
    _ringer.start();
    if (_screenLock.locked()) {
      _lockScreen.show(nullptr, name);
      _lockScreen.update(tr::lng_call_incoming(tr::now), true, false, false);
    }
  }
  gtk_window_present(GTK_WINDOW(_window));
}

void CallView::debugLocalCall(const QString &action) {
  auto &calls = Core::App().calls();
  if (action == u"end"_q || action == u"camera"_q || action == u"mute"_q || action == u"audio"_q
      || action == u"near"_q || action == u"far"_q) {
    const auto call = calls.currentCall();
    if (call && call->state() == Calls::Call::WaitingUserConfirmation && !call->id()) {
      if (action == u"camera"_q) switchCamera();
      else if (action == u"audio"_q) chooseAudioOutput();
      else if (action == u"near"_q || action == u"far"_q) _proximity.debugSetNear(action == u"near"_q);
      else if (action == u"mute"_q) {
        g_signal_emit_by_name(_mute, "clicked");
        g_message("Local call regression: microphone %s", call->muted() ? "muted" : "unmuted");
      }
      else {
        hangup();
        g_message("Local call regression: ended unconfirmed call (no network call)");
      }
    }
  } else if (!calls.currentCall() && _parent->session()) {
    // The core only starts signalling after confirmation. Exercise its real Qt panel and GTK
    // teardown with our own user, without confirming or contacting another account.
    choosePhoneCamera();
    calls.startOutgoingCall(_parent->session()->user(), {.video = action == u"video"_q, .isConfirmed = false});
  }
}

} // namespace Contrail
