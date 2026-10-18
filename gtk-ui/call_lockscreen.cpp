#include "call_lockscreen.hpp"

#include "userpics.hpp"

#include "lang/lang_keys.h"

#ifdef CONTRAIL_LAYER_SHELL
#include <gtk4-layer-shell.h>
#endif // CONTRAIL_LAYER_SHELL

namespace Contrail {
namespace {

// A round button with its label under it, as in the phone's dialer.
GtkWidget *LabeledButton(GtkWidget **button, const char *icon, const QString &label, const char *css) {
  *button = gtk_button_new_from_icon_name(icon);
  gtk_widget_add_css_class(*button, "circular");
  gtk_widget_add_css_class(*button, "lockscreen-call-button");
  if (css) gtk_widget_add_css_class(*button, css);
  gtk_widget_set_halign(*button, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(*button, U8(label).c_str());
  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_box_append(GTK_BOX(box), *button);
  auto *text = gtk_label_new(U8(label).c_str());
  gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(box), text);
  return box;
}

GtkWidget *ButtonRow() {
  auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
  gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
  return row;
}

} // namespace

LockScreenCall::LockScreenCall(Callbacks callbacks) : _callbacks(std::move(callbacks)) {
}

LockScreenCall::~LockScreenCall() {
  _signals.clear();
  if (_window) {
    gtk_window_destroy(GTK_WINDOW(_window));
    g_object_unref(_window);
  }
}

bool LockScreenCall::Supported() {
#ifdef CONTRAIL_LAYER_SHELL
  return gtk_layer_is_supported();
#else // CONTRAIL_LAYER_SHELL
  return false;
#endif // CONTRAIL_LAYER_SHELL
}

void LockScreenCall::build() {
#ifdef CONTRAIL_LAYER_SHELL
  _window = gtk_window_new();
  g_object_ref_sink(_window);
  gtk_widget_add_css_class(_window, "call-window");
  gtk_window_set_title(GTK_WINDOW(_window), U8(tr::lng_call_box_title(tr::now)).c_str());
  gtk_layer_init_for_window(GTK_WINDOW(_window));
  gtk_layer_set_namespace(GTK_WINDOW(_window), "contrail-call");
  gtk_layer_set_layer(GTK_WINDOW(_window), GTK_LAYER_SHELL_LAYER_OVERLAY);
  for (const auto edge : {GTK_LAYER_SHELL_EDGE_LEFT, GTK_LAYER_SHELL_EDGE_RIGHT, GTK_LAYER_SHELL_EDGE_TOP,
                          GTK_LAYER_SHELL_EDGE_BOTTOM}) {
    gtk_layer_set_anchor(GTK_WINDOW(_window), edge, TRUE);
  }
  // The whole screen, the shell's top bar included; the lock screen keeps the keyboard.
  gtk_layer_set_exclusive_zone(GTK_WINDOW(_window), -1);
  gtk_layer_set_keyboard_mode(GTK_WINDOW(_window), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

  auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(content, "lockscreen-call");
  SetMarginAll(content, 24);
  gtk_widget_set_margin_top(content, 96);
  gtk_widget_set_margin_bottom(content, 64);
  _avatarBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_halign(_avatarBox, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_bottom(_avatarBox, 16);
  gtk_box_append(GTK_BOX(content), _avatarBox);
  _name = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(_name), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(_name, "title-1");
  gtk_box_append(GTK_BOX(content), _name);
  _status = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(_status), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(_status, "title-4");
  gtk_box_append(GTK_BOX(content), _status);
  auto *spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(content), spacer);

  GtkWidget *decline = nullptr, *accept = nullptr, *hangup = nullptr;
  _incoming = ButtonRow();
  gtk_box_append(GTK_BOX(_incoming), LabeledButton(&decline, "call-stop-symbolic", tr::lng_call_decline(tr::now),
      "destructive-action"));
  gtk_box_append(GTK_BOX(_incoming), LabeledButton(&accept, "call-start-symbolic", tr::lng_call_accept(tr::now),
      "lockscreen-call-accept"));
  gtk_box_append(GTK_BOX(content), _incoming);
  _ongoing = ButtonRow();
  gtk_box_append(GTK_BOX(_ongoing), LabeledButton(&_mute, "audio-input-microphone-symbolic",
      tr::lng_call_mute_audio(tr::now), nullptr));
  gtk_box_append(GTK_BOX(_ongoing), LabeledButton(&_speaker, "audio-speakers-symbolic",
      tr::lng_settings_call_section_output(tr::now), nullptr));
  gtk_box_append(GTK_BOX(_ongoing), LabeledButton(&hangup, "call-stop-symbolic", tr::lng_call_end_call(tr::now),
      "destructive-action"));
  gtk_box_append(GTK_BOX(content), _ongoing);

  auto *overlay = gtk_overlay_new();
  gtk_overlay_set_child(GTK_OVERLAY(overlay), content);
  _shield = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(_shield, "call-proximity-shield");
  gtk_widget_set_visible(_shield, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), _shield);
  gtk_window_set_child(GTK_WINDOW(_window), overlay);

  const auto call = [](const std::function<void()> &callback) { if (callback) callback(); };
  _signals.clicked(accept, [=] { call(_callbacks.accept); });
  _signals.clicked(decline, [=] { call(_callbacks.hangup); });
  _signals.clicked(hangup, [=] { call(_callbacks.hangup); });
  _signals.clicked(_mute, [=] { call(_callbacks.mute); });
  _signals.clicked(_speaker, [=] { call(_callbacks.speaker); });
  _signals.connect<gboolean(GtkWindow *)>(_window, "close-request", [](GtkWindow *) -> gboolean { return TRUE; });
#endif // CONTRAIL_LAYER_SHELL
}

void LockScreenCall::show(PeerData *peer, const QString &name) {
  if (!Supported()) return;
  if (!_window) build();
  if (!_window) return;
  ClearBox(_avatarBox);
  gtk_box_append(GTK_BOX(_avatarBox), peer ? MakeAvatar(peer, 128, true) : adw_avatar_new(128, U8(name).c_str(), TRUE));
  gtk_label_set_text(GTK_LABEL(_name), U8(name).c_str());
  gtk_widget_set_visible(_shield, FALSE);
  gtk_widget_set_visible(_window, TRUE);
  _shown = true;
}

void LockScreenCall::update(const QString &status, bool incoming, bool muted, bool speaker) {
  if (!_window) return;
  gtk_label_set_text(GTK_LABEL(_status), U8(status).c_str());
  gtk_widget_set_visible(_incoming, incoming);
  gtk_widget_set_visible(_ongoing, !incoming);
  gtk_button_set_icon_name(GTK_BUTTON(_mute), muted
      ? "microphone-sensitivity-muted-symbolic" : "audio-input-microphone-symbolic");
  if (muted) gtk_widget_add_css_class(_mute, "active");
  else gtk_widget_remove_css_class(_mute, "active");
  if (speaker) gtk_widget_add_css_class(_speaker, "active");
  else gtk_widget_remove_css_class(_speaker, "active");
}

void LockScreenCall::setShield(bool near) {
  if (_window) gtk_widget_set_visible(_shield, near && _shown);
}

void LockScreenCall::hide() {
  _shown = false;
  if (_window) gtk_widget_set_visible(_window, FALSE);
}

} // namespace Contrail
