#include "call_notification.hpp"

#include "app.hpp"

#include "lang/lang_keys.h"

namespace Contrail {
namespace {

constexpr auto kService = "org.freedesktop.Notifications";
constexpr auto kPath = "/org/freedesktop/Notifications";
constexpr auto kInterface = "org.freedesktop.Notifications";

// The action names start with "call-": the desktop entry allows those on the lock screen.
constexpr auto kAccept = "call-accept";
constexpr auto kDecline = "call-decline";
constexpr auto kHangup = "call-hangup";

GVariant *ImageData(const QImage &image) {
  const auto rgba = image.convertToFormat(QImage::Format_RGBA8888);
  auto *bytes = g_bytes_new(rgba.constBits(), rgba.sizeInBytes());
  auto *data = g_variant_new_from_bytes(G_VARIANT_TYPE_BYTESTRING, bytes, TRUE);
  g_bytes_unref(bytes);
  return g_variant_new("(iiibii@ay)", rgba.width(), rgba.height(), int(rgba.bytesPerLine()), TRUE, 8, 4, data);
}

} // namespace

CallNotification::CallNotification(Callbacks callbacks)
    : _callbacks(std::move(callbacks)), _cancellable(g_cancellable_new()) {
  _bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
  if (!_bus) return;
  _actionSignal = g_dbus_connection_signal_subscribe(_bus, nullptr, kInterface, "ActionInvoked", kPath, nullptr,
      G_DBUS_SIGNAL_FLAGS_NONE, +[](GDBusConnection *, const char *, const char *, const char *, const char *,
                                     GVariant *parameters, gpointer data) {
        guint32 id = 0;
        const char *action = nullptr;
        g_variant_get(parameters, "(u&s)", &id, &action);
        static_cast<CallNotification *>(data)->invoked(id, action);
      }, this, nullptr);
  _closedSignal = g_dbus_connection_signal_subscribe(_bus, nullptr, kInterface, "NotificationClosed", kPath, nullptr,
      G_DBUS_SIGNAL_FLAGS_NONE, +[](GDBusConnection *, const char *, const char *, const char *, const char *,
                                     GVariant *parameters, gpointer data) {
        guint32 id = 0, reason = 0;
        g_variant_get(parameters, "(uu)", &id, &reason);
        static_cast<CallNotification *>(data)->closed(id);
      }, this, nullptr);
}

CallNotification::~CallNotification() {
  close();
  g_cancellable_cancel(_cancellable);
  if (_bus) {
    g_dbus_connection_signal_unsubscribe(_bus, _actionSignal);
    g_dbus_connection_signal_unsubscribe(_bus, _closedSignal);
    g_object_unref(_bus);
  }
  g_object_unref(_cancellable);
}

void CallNotification::showIncoming(const QString &name, const QString &status, const QImage &userpic) {
  if (_dismissed) return;
  _state = State::Incoming;
  _userpic = userpic;
  send(name, status);
}

void CallNotification::showOngoing(const QString &name, const QString &status) {
  if (_dismissed || _state == State::None) return;
  _state = State::Ongoing;
  send(name, status);
}

void CallNotification::close() {
  _state = State::None;
  _dismissed = false;
  _shown.clear();
  _userpic = QImage();
  // A notification still being shown is closed once its id is known.
  if (_inFlight) return;
  if (_bus && _id) {
    g_dbus_connection_call(_bus, kService, kPath, kInterface, "CloseNotification", g_variant_new("(u)", _id),
        nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
  }
  _id = 0;
}

void CallNotification::send(const QString &name, const QString &status) {
  if (!_bus) return;
  const auto incoming = _state == State::Incoming;
  const auto shown = QString::number(int(_state)) + u'\n' + name + u'\n' + status;
  if (shown == _shown) return;
  _shown = shown;
  _name = name;
  _status = status;
  if (_inFlight) {
    // Replaces the notification once the shell has told its id.
    _resend = true;
    return;
  }
  _inFlight = true;
  _resend = false;

  auto *actions = g_variant_builder_new(G_VARIANT_TYPE_STRING_ARRAY);
  // A tap on the notification itself brings up the call window (after unlocking).
  g_variant_builder_add(actions, "s", "default");
  g_variant_builder_add(actions, "s", "");
  const auto action = [&](const char *id, const QString &label) {
    g_variant_builder_add(actions, "s", id);
    g_variant_builder_add(actions, "s", label.toUtf8().constData());
  };
  if (incoming) {
    action(kDecline, tr::lng_call_decline(tr::now));
    action(kAccept, tr::lng_call_accept(tr::now));
  } else {
    action(kHangup, tr::lng_call_end_call(tr::now));
  }
  auto *hints = g_variant_builder_new(G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(hints, "{sv}", "desktop-entry", g_variant_new_string(kAppId));
  g_variant_builder_add(hints, "{sv}", "category", g_variant_new_string(incoming ? "call.incoming" : "call"));
  // Critical: wakes the screen and stays until the call is answered or over.
  g_variant_builder_add(hints, "{sv}", "urgency", g_variant_new_byte(incoming ? 2 : 1));
  // The buttons answer the call, the notification is replaced or closed here.
  g_variant_builder_add(hints, "{sv}", "resident", g_variant_new_boolean(TRUE));
  // The call rings by itself (Ringer): no sound or vibration of the notification's own.
  // (Not suppress-sound: Phosh takes it for vibration.)
  g_variant_builder_add(hints, "{sv}", "x-phosh-fb-profile", g_variant_new_string("none"));
  if (!_userpic.isNull()) {
    g_variant_builder_add(hints, "{sv}", "image-data", ImageData(_userpic));
  }

  g_dbus_connection_call(_bus, kService, kPath, kInterface, "Notify",
      g_variant_new("(susssasa{sv}i)", "Contrail", _id, kAppId, name.toUtf8().constData(),
          status.toUtf8().constData(), actions, hints, 0),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, _cancellable,
      +[](GObject *source, GAsyncResult *result, gpointer data) {
        const auto self = static_cast<CallNotification *>(data);
        GError *error = nullptr;
        auto *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
        if (!reply) {
          const auto cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
          g_error_free(error);
          // Cancelled: the object is gone.
          if (!cancelled) self->sent(0);
          return;
        }
        guint32 id = 0;
        g_variant_get(reply, "(u)", &id);
        g_variant_unref(reply);
        self->sent(id);
      }, this);
  g_variant_builder_unref(actions);
  g_variant_builder_unref(hints);
}

void CallNotification::sent(guint32 id) {
  _inFlight = false;
  if (_state == State::None) {
    // Closed meanwhile.
    _id = id;
    close();
    return;
  }
  _id = id;
  if (_resend) {
    _shown.clear();
    send(_name, _status);
  }
}

void CallNotification::invoked(guint32 id, const char *action) {
  if (!id || id != _id || !action) return;
  if (g_getenv("CONTRAIL_DEBUG")) g_message("Call notification: %s", action);
  const auto is = [&](const char *name) { return g_strcmp0(action, name) == 0; };
  // Not within the signal: answering or hanging up replaces or closes this notification.
  if (is(kAccept) && _callbacks.accept) _callbacks.accept();
  else if (is(kDecline) && _callbacks.decline) _callbacks.decline();
  else if (is(kHangup) && _callbacks.hangup) _callbacks.hangup();
  else if (is("default") && _callbacks.open) _callbacks.open();
}

void CallNotification::closed(guint32 id) {
  if (!id || id != _id) return;
  // Swiped away: the call goes on in the app, without coming back here.
  _id = 0;
  _dismissed = true;
}

} // namespace Contrail
