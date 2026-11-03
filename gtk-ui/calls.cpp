#include "calls.hpp"

#include "main_window.hpp"
#include "layout.hpp"
#include "call_layout.hpp"
#include "td/session.hpp"
#include "td/strings.hpp"
#include "userpics.hpp"

#include <algorithm>
#include <cstdio>

namespace Contrail {
namespace {
using S = CallBackend::Stage;
using OutputKind = AudioRoute::Kind;

GtkWidget *CallButton(const std::string &text, const char *css) {
  auto *button = gtk_button_new_with_label(text.c_str());
  gtk_widget_add_css_class(button, "pill");
  gtk_widget_add_css_class(button, css);
  return button;
}

const char *OutputIcon(OutputKind kind) {
  switch (kind) {
    case OutputKind::Earpiece: return "call-start-symbolic";
    case OutputKind::Speaker: return "audio-speakers-symbolic";
    case OutputKind::Headphones: return "audio-headphones-symbolic";
    case OutputKind::Bluetooth: return "bluetooth-active-symbolic";
    default: return "audio-card-symbolic";
  }
}

std::string OutputName(const AudioRoute::Output &output) {
  if (!output.name.empty()) return output.name;
  switch (output.kind) {
    case OutputKind::Earpiece: return Translated("Phone", "Телефон", "Телефон");
    case OutputKind::Speaker: return Translated("Speaker", "Динамик", "Гучномовець");
    default: return Translated("Headphones", "Наушники", "Навушники");
  }
}

std::string CallErrorText(const std::string &error, const std::string &user) {
  if (error == "CAMERA_UNAVAILABLE") return Tr("lng_call_error_no_camera");
  if (error == "USER_PRIVACY_RESTRICTED" || error == "USER_IS_BLOCKED" || error == "CALL_PEER_INVALID") {
    return Tr("lng_call_error_not_available", {{"user", user}});
  }
  if (error == "PARTICIPANT_VERSION_OUTDATED" || error == "Unsupported call media protocol.") {
    return Tr("lng_call_error_outdated", {{"user", user}});
  }
  if (error == "Call connection failed.") return Tr("lng_call_status_failed");
  if (error == "This build has no call media support.") {
    return Translated("This build has no call support.", "В этой сборке нет поддержки звонков.",
        "У цій збірці немає підтримки дзвінків.");
  }
  return error;
}

std::vector<Calls::Camera> Cameras() {
  if (g_getenv("CONTRAIL_SCENARIO")) return {{"scenario-front", "Front"}, {"scenario-back", "Back"}};
  return Calls::Cameras();
}
} // namespace

CallView::CallView(MainWindow &window, Td::Store &store)
    : _parent(window), _store(store)
    , _backend(store.client(), g_getenv("CONTRAIL_SCENARIO") ? Calls::MakeScenarioMedia : Calls::StartMedia)
    , _audio([this] { updateAudioRoute(); }, Td::DataDirectory() + "/call-volumes.ini")
    , _proximity([this](bool near) { setProximityShield(near); })
    , _notification({.accept = [this] { _backend.accept(); }, .decline = [this] { _backend.hangup(); },
          .hangup = [this] { _backend.hangup(); }, .open = [this] { _parent.present(); present(); }})
    , _lockScreen({.accept = [this] { _backend.accept(); }, .hangup = [this] { _backend.hangup(); },
          .mute = [this] { _backend.setMuted(!_backend.snapshot().muted); }, .speaker = [this] { toggleSpeaker(); }})
    , _screenLock([this](bool locked) {
        updateLockScreen();
        if (!locked && _backend.active()) { _parent.present(); present(); }
      }) {
  _window = adw_window_new();
  g_object_ref_sink(_window);
  gtk_widget_add_css_class(_window, "call-window");
  gtk_window_set_title(GTK_WINDOW(_window), Tr("lng_call_box_title").c_str());
  gtk_window_set_default_size(GTK_WINDOW(_window), 380, 620);
  gtk_widget_set_size_request(_window, 320, 294);
  gtk_window_set_transient_for(GTK_WINDOW(_window), GTK_WINDOW(window.widget()));
  _signals.connect<gboolean(GtkWindow *)>(_window, "close-request", [=, this](GtkWindow *) -> gboolean {
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
  _accept = CallButton(Tr("lng_call_accept"), "suggested-action");
  gtk_box_append(GTK_BOX(controls), _accept);
  auto *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  const auto button = [&](const char *icon, const std::string &text) {
    auto *result = MakeIconButton(icon, text.c_str());
    gtk_widget_add_css_class(result, "circular");
    gtk_widget_add_css_class(result, "call-control");
    gtk_box_append(GTK_BOX(buttons), result);
    return result;
  };
  _mute = button("audio-input-microphone-symbolic", Tr("lng_call_mute_audio"));
  _camera = button("camera-video-symbolic", Tr("lng_call_start_video"));
  _switchCamera = button("view-refresh-symbolic", Tr("lng_settings_call_camera"));
  _speaker = button("audio-speakers-symbolic", Tr("lng_settings_call_section_output"));
  _hangup = button("call-stop-symbolic", Tr("lng_call_end_call"));
  gtk_widget_add_css_class(_hangup, "destructive-action");
  gtk_box_append(GTK_BOX(controls), buttons);
  gtk_overlay_add_overlay(GTK_OVERLAY(stage), controls);
  // While the phone is held to the ear the screen is black and the cheek can't press the buttons.
  _shield = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(_shield, "call-proximity-shield");
  gtk_widget_set_visible(_shield, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(stage), _shield);
  adw_window_set_content(ADW_WINDOW(_window), stage);

  _signals.clicked(_accept, [this] {
    const auto call = _backend.snapshot();
    if (call.stage == S::Busy) start(call.user, call.video);
    else _backend.accept();
  });
  _signals.clicked(_hangup, [this] {
    if (_backend.active()) _backend.hangup();
    else gtk_widget_set_visible(_window, FALSE);
  });
  _signals.clicked(_mute, [this] { _backend.setMuted(!_backend.snapshot().muted); });
  _signals.clicked(_camera, [this] {
    const auto &call = _backend.snapshot();
    if (call.camera.empty()) loadCameras();
    const auto &cameras = _cameras;
    if (!call.camera.empty()) { _backend.setCamera({}); showFrame({}, true); }
    else if (!cameras.empty()) _backend.setCamera(cameras.front().id);
    else ShowToast(Tr("lng_call_error_no_camera"));
  });
  _signals.clicked(_switchCamera, [this] { switchCamera(); });
  _signals.clicked(_speaker, [this] { chooseAudioOutput(); });
  _backend.setChanged([this] { refresh(); });
  _backend.setFrames([this](Calls::Frame frame, bool local) { showFrame(std::move(frame), local); });
}

CallView::~CallView() {
  _backend.setChanged(nullptr);
  _backend.setFrames(nullptr);
  _backend.hangup();
  _proximity.setEnabled(false);
  _idle.set(false);
  _ringer.stop();
  _notification.close();
  _lockScreen.hide();
  if (_timer) g_source_remove(_timer);
  _signals.clear();
  gtk_window_destroy(GTK_WINDOW(_window));
  g_object_unref(_window);
}

void CallView::start(Td::UserId user, bool video) {
  if (_backend.active()) { present(); return; }
  const auto known = _store.user(user);
  if (user == _store.selfId() || (known && (known->bot || known->deleted))) return;
  _backend.start(user, video);
}

void CallView::present() {
  if (_backend.snapshot().stage != S::None) gtk_window_present(GTK_WINDOW(_window));
}

std::string CallView::name() const {
  const auto user = _store.user(_backend.snapshot().user);
  return user ? Td::DisplayName(*user) : Tr("lng_deleted");
}

std::string CallView::status() const {
  switch (_backend.snapshot().stage) {
    case S::Incoming: return Tr("lng_call_status_incoming");
    case S::Requesting: return Tr("lng_call_status_requesting");
    case S::Ringing: return Tr("lng_call_status_ringing");
    case S::Exchanging: return Tr("lng_call_status_exchanging");
    case S::Established: {
      const auto seconds = _backend.duration();
      char result[32];
      std::snprintf(result, sizeof(result), "%d:%02d", seconds / 60, seconds % 60);
      return result;
    }
    case S::Connecting: case S::Reconnecting: return Tr("lng_call_status_connecting");
    case S::HangingUp: return Tr("lng_call_status_hanging");
    case S::Busy: return Tr("lng_call_status_busy");
    case S::Failed: return Tr("lng_call_status_failed");
    default: return Tr("lng_call_status_ended");
  }
}

void CallView::refresh() {
  const auto &call = _backend.snapshot();
  const auto active = _backend.active();
  const auto incoming = call.stage == S::Incoming;
  const auto busy = call.stage == S::Busy;
  const auto finishing = !active || call.stage == S::HangingUp;
  if (call.id != _shownCall || (!_shownCall && call.stage == S::Requesting)) {
    _shownCall = call.id;
    _cameras.clear();
    _devicesLoaded = false;
    _shownError.clear();
    _ratedCall = 0;
    stopVideo();
    ClearBox(_avatarBox);
    gtk_box_append(GTK_BOX(_avatarBox), MakeUserAvatar(_store, call.user, 128));
    _parent.present();
    present();
  }
  if (!call.camera.empty() && !_devicesLoaded) loadCameras();
  if (call.error.empty()) _shownError.clear();
  gtk_window_set_title(GTK_WINDOW(_window), Tr("lng_call_box_title").c_str());
  gtk_widget_set_tooltip_text(_switchCamera, Tr("lng_settings_call_camera").c_str());
  gtk_label_set_text(GTK_LABEL(_name), name().c_str());
  const auto text = status();
  gtk_label_set_text(GTK_LABEL(_status), text.c_str());
  gtk_label_set_text(GTK_LABEL(_fingerprint), call.emojis.c_str());
  gtk_widget_set_visible(_fingerprint, !call.emojis.empty() && !finishing);
  gtk_widget_set_tooltip_text(_fingerprint, Tr("lng_call_fingerprint_tooltip", {{"user", name()}}).c_str());
  gtk_widget_set_visible(_remoteMuted, call.remoteMuted && !finishing);
  gtk_label_set_text(GTK_LABEL(_remoteMuted), Tr("lng_call_microphone_off", {{"user", name()}}).c_str());
  gtk_widget_set_visible(_accept, (incoming || busy) && call.stage != S::HangingUp);
  gtk_button_set_label(GTK_BUTTON(_accept), Tr(busy ? "lng_call_redial" : "lng_call_accept").c_str());
  gtk_widget_set_sensitive(_mute, !finishing && !busy);
  gtk_widget_set_sensitive(_camera, call.stage == S::Established);
  gtk_widget_set_sensitive(_switchCamera, !finishing && !call.camera.empty() && _cameras.size() > 1);
  gtk_widget_set_sensitive(_speaker, !finishing && _audio.outputs().size() > 1);
  gtk_widget_set_sensitive(_hangup, call.stage != S::HangingUp);
  gtk_widget_set_tooltip_text(_hangup, Tr(incoming ? "lng_call_decline" : "lng_call_end_call").c_str());
  gtk_button_set_icon_name(GTK_BUTTON(_mute), call.muted
      ? "microphone-sensitivity-muted-symbolic" : "audio-input-microphone-symbolic");
  gtk_widget_set_tooltip_text(_mute, Tr(call.muted ? "lng_call_unmute_audio" : "lng_call_mute_audio").c_str());
  if (call.muted) gtk_widget_add_css_class(_mute, "active"); else gtk_widget_remove_css_class(_mute, "active");
  if (!call.camera.empty()) gtk_widget_add_css_class(_camera, "active"); else gtk_widget_remove_css_class(_camera, "active");
  gtk_widget_set_tooltip_text(_camera, Tr(call.camera.empty() ? "lng_call_start_video" : "lng_call_stop_video").c_str());
  _idle.set(active && !finishing);
  if (incoming) _ringer.start(); else _ringer.stop();
  if (!incoming && !finishing && !_audioStarted) { _audioStarted = true; _audio.begin(call.video); }
  if (finishing) {
    stopVideo();
    _proximity.setEnabled(false);
    if (_audioStarted) { _audioStarted = false; _audio.end(); _output.clear(); _backend.setOutputDevice({}); }
    _notification.close();
    _lockScreen.hide();
  } else if (incoming) {
    GdkTexture *texture = nullptr;
    if (auto *avatar = gtk_widget_get_first_child(_avatarBox); ADW_IS_AVATAR(avatar)) {
      // The small userpic is already a CPU texture. Rendering the widget to a new texture here needlessly
      // exports a Vulkan dmabuf on phones, even for callers whose notification is already shown.
      auto *image = adw_avatar_get_custom_image(ADW_AVATAR(avatar));
      if (GDK_IS_TEXTURE(image)) texture = GDK_TEXTURE(g_object_ref(image));
    }
    _notification.showIncoming(name(), Tr("lng_call_incoming"), texture);
    g_clear_object(&texture);
  } else _notification.showOngoing(name(), text);
  updateProximity();
  updateLockScreen();
  _parent.setCallStatus(active ? name() + " · " + text : std::string());
  if (active && !_timer) _timer = g_timeout_add_seconds(1, +[](gpointer data) -> gboolean {
    static_cast<CallView *>(data)->refresh(); return G_SOURCE_CONTINUE;
  }, this);
  if (!active && _timer) { g_source_remove(_timer); _timer = 0; }
  if (!call.error.empty() && _shownError != call.error) {
    _shownError = call.error;
    ShowToast(CallErrorText(call.error, name()));
  }
  if (call.needRating) rating();
}

void CallView::stopVideo() { showFrame({}, false); showFrame({}, true); }

void CallView::showFrame(Calls::Frame frame, bool local) {
  auto *picture = local ? _localVideo : _video;
  const auto valid = frame.pixels && frame.width > 0 && frame.height > 0;
  if (valid) {
    auto *owner = new std::shared_ptr<const std::vector<std::uint8_t>>(frame.pixels);
    auto *bytes = g_bytes_new_with_free_func(frame.pixels->data(), frame.pixels->size(),
        +[](gpointer data) { delete static_cast<std::shared_ptr<const std::vector<std::uint8_t>> *>(data); }, owner);
    auto *texture = gdk_memory_texture_new(frame.width, frame.height, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, bytes, frame.stride);
    gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(texture));
    g_object_unref(texture);
    g_bytes_unref(bytes);
  } else gtk_picture_set_paintable(GTK_PICTURE(picture), nullptr);
  (local ? _localWidth : _remoteWidth) = valid ? frame.width : 0;
  gtk_widget_set_visible(local ? _localFrame : picture, valid);
  if (!local) gtk_widget_set_visible(_avatarBox, !valid);
}

void CallView::loadCameras() {
  _cameras = Cameras();
  _devicesLoaded = true;
}

void CallView::switchCamera() {
  const auto &cameras = _cameras;
  const auto &current = _backend.snapshot().camera;
  if (cameras.size() < 2 || current.empty()) return;
  auto from = std::find_if(cameras.begin(), cameras.end(), [&](const auto &camera) { return camera.id == current; });
  if (from == cameras.end() || ++from == cameras.end()) from = cameras.begin();
  _backend.setCamera(from->id);
}

void CallView::updateAudioRoute() {
  if (!_speaker) return;
  const auto output = _audio.current();
  gtk_button_set_icon_name(GTK_BUTTON(_speaker), OutputIcon(output ? output->kind : OutputKind::Speaker));
  gtk_widget_set_tooltip_text(_speaker, (Tr("lng_settings_call_section_output")
      + (output ? "\n" + OutputName(*output) : std::string())).c_str());
  if (output && output->kind == OutputKind::Speaker) gtk_widget_add_css_class(_speaker, "active");
  else gtk_widget_remove_css_class(_speaker, "active");
  if (_backend.active() && output && output->device != _output) {
    _output = output->device;
    _backend.setOutputDevice(_output);
  }
  gtk_widget_set_sensitive(_speaker, _backend.active() && _audio.outputs().size() > 1);
  updateProximity();
  updateLockScreen();
}

void CallView::chooseAudioOutput() {
  const auto &outputs = _audio.outputs();
  const auto current = _audio.current();
  if (outputs.size() < 2) return;
  if (outputs.size() == 2 && current && !outputs[0].port.empty() && !outputs[1].port.empty()) {
    _audio.select(outputs[0].id == current->id ? outputs[1].id : outputs[0].id);
    return;
  }
  auto *popover = gtk_popover_new();
  auto *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  const auto token = _lifetime.token();
  for (const auto &output : outputs) {
    auto *row = gtk_button_new();
    gtk_widget_add_css_class(row, "flat");
    auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(OutputIcon(output.kind)));
    auto *label = gtk_label_new(OutputName(output).c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), label);
    auto *check = gtk_image_new_from_icon_name("object-select-symbolic");
    gtk_widget_set_opacity(check, current && current->id == output.id ? 1. : 0.);
    gtk_box_append(GTK_BOX(box), check);
    gtk_button_set_child(GTK_BUTTON(row), box);
    OnClicked(row, [this, token, popover, id = output.id] {
      gtk_popover_popdown(GTK_POPOVER(popover));
      if (!token.expired() && _backend.active()) _audio.select(id);
    });
    gtk_box_append(GTK_BOX(list), row);
  }
  gtk_popover_set_child(GTK_POPOVER(popover), list);
  gtk_widget_set_parent(popover, _speaker);
  DestroyPopoverOnClose(popover);
  gtk_popover_popup(GTK_POPOVER(popover));
}

void CallView::toggleSpeaker() {
  const auto current = _audio.current();
  const auto speaker = current && current->kind == OutputKind::Speaker;
  for (const auto &output : _audio.outputs()) if (output.kind == (speaker ? OutputKind::Earpiece : OutputKind::Speaker)) {
    _audio.select(output.id); return;
  }
}

void CallView::updateProximity() {
  const auto &call = _backend.snapshot();
  const auto output = _audio.current();
  _proximity.setEnabled(_backend.active() && call.stage != S::Incoming && call.stage != S::HangingUp
      && output && output->kind == OutputKind::Earpiece);
}

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

void CallView::updateLockScreen() {
  const auto &call = _backend.snapshot();
  if (!_backend.active() || call.stage == S::HangingUp || !(_screenLock.locked() || _debugLocked)) {
    _lockScreen.hide(); return;
  }
  if (!_lockScreen.shown()) _lockScreen.show(_store, call.user, name());
  const auto output = _audio.current();
  _lockScreen.update(status(), call.stage == S::Incoming, call.muted, output && output->kind == OutputKind::Speaker);
  _lockScreen.setShield(_proximity.near());
}

void CallView::rating() {
  const auto &call = _backend.snapshot();
  if (_ratedCall == call.id) return;
  _ratedCall = call.id;
  auto *dialog = adw_alert_dialog_new(Tr("lng_call_rate_label").c_str(), nullptr);
  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  auto *score = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 5, 1);
  gtk_range_set_value(GTK_RANGE(score), 5);
  gtk_scale_set_digits(GTK_SCALE(score), 0);
  gtk_scale_set_draw_value(GTK_SCALE(score), TRUE);
  auto *comment = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(comment), Tr("lng_call_rate_comment").c_str());
  gtk_box_append(GTK_BOX(box), score);
  gtk_box_append(GTK_BOX(box), comment);
  adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), box);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", Tr("lng_cancel").c_str(),
      "send", Tr("lng_send_button").c_str(), nullptr);
  const auto token = _lifetime.token();
  Connect<void(AdwAlertDialog *, const char *)>(dialog, "response", [this, token, score, comment, id = call.id]
      (AdwAlertDialog *, const char *response) {
    if (!token.expired() && _backend.snapshot().id == id && g_strcmp0(response, "send") == 0) {
      _backend.rate(int(gtk_range_get_value(GTK_RANGE(score))), gtk_editable_get_text(GTK_EDITABLE(comment)));
    }
  });
  adw_dialog_present(ADW_DIALOG(dialog), _window);
}

void CallView::debug(const std::string &action, const std::string &argument) {
  if (action == "start") start(argument.empty() ? 1 : std::atoll(argument.c_str()));
  else if (action == "video") start(argument.empty() ? 1 : std::atoll(argument.c_str()), true);
  else if (action == "accept") _backend.accept();
  else if (action == "hangup") _backend.hangup();
  else if (action == "mute") _backend.setMuted(!_backend.snapshot().muted);
  else if (action == "camera") {
    if (_backend.snapshot().camera.empty()) loadCameras();
    const auto &cameras = _cameras;
    if (!cameras.empty()) _backend.setCamera(_backend.snapshot().camera.empty() ? cameras.front().id : std::string());
  } else if (action == "switch-camera") switchCamera();
  else if (action == "speaker") toggleSpeaker();
  else if (action == "route") _audio.select(argument);
  else if (action == "near") setProximityShield(argument == "1");
  else if (action == "lock") { _debugLocked = argument == "1"; updateLockScreen(); }
  else if (action == "hide") gtk_widget_set_visible(_window, FALSE);
  else if (action == "present") present();
  else if (action == "devices") {
    for (const auto &camera : Calls::Cameras()) g_message("call-camera: id=%s name=%s", camera.id.c_str(), camera.name.c_str());
    for (const auto &output : _audio.outputs()) g_message("call-output: id=%s kind=%d device=%s", output.id.c_str(),
        int(output.kind), output.device.c_str());
  } else if (action == "rate") _backend.rate(std::atoi(argument.c_str()), "Scenario feedback");
  else if (g_getenv("CONTRAIL_SCENARIO") && action != "state") {
    _store.client().send(Td::api::make_object<Td::api::getOption>("contrail_call_" + action));
  }
  const auto &call = _backend.snapshot();
  g_message("call-state: id=%d user=%lld stage=%d active=%d incoming=%d muted=%d camera=%s remote=%d local=%d "
      "visible=%d shield=%d locked=%d notification=%u ringing=%u emojis=\"%s\" status=\"%s\" sound=%s", call.id, (long long)call.user,
      int(call.stage), _backend.active(), call.stage == S::Incoming, call.muted, call.camera.c_str(),
      _remoteWidth, _localWidth, gtk_widget_get_visible(_window), gtk_widget_get_visible(_shield), _lockScreen.shown(), _notification.id(), _ringer.eventId(),
      call.emojis.c_str(), status().c_str(), _backend.soundStats().c_str());
}

} // namespace Contrail
