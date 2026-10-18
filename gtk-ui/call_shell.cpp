#include "call_shell.hpp"

#include "app.hpp"

namespace Contrail {
namespace {

constexpr auto kLogin = "org.freedesktop.login1";
constexpr auto kSessionInterface = "org.freedesktop.login1.Session";
constexpr auto kFeedbackName = "org.sigxcpu.Feedback";
constexpr auto kFeedbackPath = "/org/sigxcpu/Feedback";

GDBusConnection *SessionBus() {
  return g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
}

} // namespace

ScreenLock::ScreenLock(Changed changed) : _changed(std::move(changed)), _cancellable(g_cancellable_new()) {
  _bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
  if (!_bus) return;
  // The user's graphical session; the app itself may run in another one (started over SSH).
  g_dbus_connection_call(_bus, kLogin, "/org/freedesktop/login1/user/self", "org.freedesktop.DBus.Properties",
      "Get", g_variant_new("(ss)", "org.freedesktop.login1.User", "Display"), G_VARIANT_TYPE("(v)"),
      G_DBUS_CALL_FLAGS_NONE, -1, _cancellable,
      +[](GObject *source, GAsyncResult *result, gpointer data) {
        auto *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, nullptr);
        if (!reply) return; // No logind, or cancelled.
        GVariant *display = nullptr;
        g_variant_get(reply, "(v)", &display);
        const char *path = nullptr;
        g_variant_get(display, "(&s&o)", nullptr, &path);
        if (path && g_strcmp0(path, "/") != 0) static_cast<ScreenLock *>(data)->watch(path);
        g_variant_unref(display);
        g_variant_unref(reply);
      }, this);
}

ScreenLock::~ScreenLock() {
  g_cancellable_cancel(_cancellable);
  if (_bus) {
    if (_signal) g_dbus_connection_signal_unsubscribe(_bus, _signal);
    g_object_unref(_bus);
  }
  g_object_unref(_cancellable);
}

void ScreenLock::watch(const char *session) {
  _signal = g_dbus_connection_signal_subscribe(_bus, kLogin, "org.freedesktop.DBus.Properties", "PropertiesChanged",
      session, kSessionInterface, G_DBUS_SIGNAL_FLAGS_NONE,
      +[](GDBusConnection *, const char *, const char *, const char *, const char *, GVariant *parameters,
          gpointer data) {
        GVariant *changed = nullptr;
        g_variant_get(parameters, "(&s@a{sv}@as)", nullptr, &changed, nullptr);
        gboolean locked = FALSE;
        if (g_variant_lookup(changed, "LockedHint", "b", &locked)) static_cast<ScreenLock *>(data)->set(locked);
        g_variant_unref(changed);
      }, this, nullptr);
  g_dbus_connection_call(_bus, kLogin, session, "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", kSessionInterface, "LockedHint"), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1,
      _cancellable,
      +[](GObject *source, GAsyncResult *result, gpointer data) {
        auto *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, nullptr);
        if (!reply) return;
        GVariant *value = nullptr;
        g_variant_get(reply, "(v)", &value);
        static_cast<ScreenLock *>(data)->set(g_variant_get_boolean(value));
        g_variant_unref(value);
        g_variant_unref(reply);
      }, this);
}

void ScreenLock::set(bool locked) {
  if (_locked == locked) return;
  _locked = locked;
  if (g_getenv("CONTRAIL_DEBUG")) g_message("Screen %s", locked ? "locked" : "unlocked");
  if (_changed) _changed(locked);
}

Ringer::Ringer() : _cancellable(g_cancellable_new()) {
  _bus = SessionBus();
}

Ringer::~Ringer() {
  stop();
  g_cancellable_cancel(_cancellable);
  if (_bus) g_object_unref(_bus);
  g_object_unref(_cancellable);
}

void Ringer::start() {
  _wanted = true;
  if (!_bus || _pending || _id) return;
  _pending = true;
  // Timeout 0: the feedback loops until it is ended.
  g_dbus_connection_call(_bus, kFeedbackName, kFeedbackPath, kFeedbackName, "TriggerFeedback",
      g_variant_new("(ss@a{sv}i)", kAppId, "phone-incoming-call", g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0), 0),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, _cancellable,
      +[](GObject *source, GAsyncResult *result, gpointer data) {
        GError *error = nullptr;
        auto *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
        if (!reply) {
          const auto cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
          g_error_free(error);
          if (!cancelled) static_cast<Ringer *>(data)->started(0);
          return;
        }
        guint32 id = 0;
        g_variant_get(reply, "(u)", &id);
        g_variant_unref(reply);
        static_cast<Ringer *>(data)->started(id);
      }, this);
}

void Ringer::started(guint32 id) {
  _pending = false;
  _id = id;
  if (!_wanted) stop();
}

void Ringer::stop() {
  _wanted = false;
  if (!_bus || !_id) return;
  g_dbus_connection_call(_bus, kFeedbackName, kFeedbackPath, kFeedbackName, "EndFeedback",
      g_variant_new("(u)", _id), nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
  _id = 0;
}

bool SystemRingtone() {
  auto *bus = SessionBus();
  if (!bus) return false;
  auto *reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", kFeedbackName), G_VARIANT_TYPE("(b)"),
      G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr);
  g_object_unref(bus);
  if (!reply) return false;
  gboolean result = FALSE;
  g_variant_get(reply, "(b)", &result);
  g_variant_unref(reply);
  return result;
}

} // namespace Contrail
