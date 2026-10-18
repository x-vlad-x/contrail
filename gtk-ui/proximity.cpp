#include "proximity.hpp"

namespace Contrail {
namespace {

constexpr auto kSensorName = "net.hadess.SensorProxy";
constexpr auto kSensorPath = "/net/hadess/SensorProxy";

bool Property(GDBusProxy *proxy, const char *name) {
  auto *value = g_dbus_proxy_get_cached_property(proxy, name);
  if (!value) return false;
  const auto result = g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(value);
  g_variant_unref(value);
  return result;
}

GDBusConnection *SessionBus() {
  static auto *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
  return bus;
}

bool Cancelled(GError *error) {
  const auto result = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  if (error && !result && g_getenv("CONTRAIL_DEBUG")) g_message("Proximity: %s", error->message);
  return result;
}

} // namespace

Proximity::Proximity(Changed changed) : _changed(std::move(changed)), _cancellable(g_cancellable_new()) {
}

Proximity::~Proximity() {
  g_cancellable_cancel(_cancellable);
  if (_sensor) {
    g_signal_handlers_disconnect_by_data(_sensor, this);
    // The service drops the claim when the app leaves the bus; release it now if the app stays.
    if (_claimed || _claiming) {
      g_dbus_proxy_call(_sensor, "ReleaseProximity", nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
    }
    g_object_unref(_sensor);
  }
  g_object_unref(_cancellable);
}

void Proximity::setEnabled(bool enabled) {
  if (_enabled == enabled) return;
  _enabled = enabled;
  if (!enabled) {
    release();
    return;
  }
  if (_sensor) {
    claim();
  } else if (!_connecting) {
    _connecting = true;
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr, kSensorName, kSensorPath,
        kSensorName, _cancellable, +[](GObject *, GAsyncResult *result, gpointer data) {
          GError *error = nullptr;
          auto *sensor = g_dbus_proxy_new_for_bus_finish(result, &error);
          if (Cancelled(error)) {
            g_error_free(error);
            return;
          }
          g_clear_error(&error);
          static_cast<Proximity *>(data)->connected(sensor);
        }, this);
  }
}

void Proximity::connected(GDBusProxy *sensor) {
  _connecting = false;
  if (!sensor) return;
  _sensor = sensor;
  g_signal_connect(_sensor, "g-properties-changed", G_CALLBACK(+[](GDBusProxy *, GVariant *changed, GStrv,
      gpointer data) { static_cast<Proximity *>(data)->propertiesChanged(changed); }), this);
  if (_enabled) claim();
}

void Proximity::claim() {
  if (!_sensor || _claimed || _claiming || !Property(_sensor, "HasProximity")) return;
  _claiming = true;
  g_dbus_proxy_call(_sensor, "ClaimProximity", nullptr, G_DBUS_CALL_FLAGS_NONE, -1, _cancellable,
      +[](GObject *source, GAsyncResult *result, gpointer data) {
        GError *error = nullptr;
        auto *reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), result, &error);
        if (Cancelled(error)) {
          g_error_free(error);
          return;
        }
        g_clear_error(&error);
        if (reply) g_variant_unref(reply);
        static_cast<Proximity *>(data)->claimed(reply != nullptr);
      }, this);
}

void Proximity::claimed(bool success) {
  _claiming = false;
  _claimed = success;
  if (!success) return;
  if (!_enabled) {
    release();
    return;
  }
  if (g_getenv("CONTRAIL_DEBUG")) g_message("Proximity sensor claimed");
  setNear(Property(_sensor, "ProximityNear"));
}

void Proximity::release() {
  setNear(false);
  if (!_sensor || !_claimed) return;
  _claimed = false;
  g_dbus_proxy_call(_sensor, "ReleaseProximity", nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
}

void Proximity::propertiesChanged(GVariant *changed) {
  gboolean value = FALSE;
  if (g_variant_lookup(changed, "HasProximity", "b", &value) && value && _enabled) claim();
  if (g_variant_lookup(changed, "ProximityNear", "b", &value) && _claimed && _enabled) setNear(value);
}

void Proximity::setNear(bool near) {
  if (_near == near) return;
  _near = near;
  if (g_getenv("CONTRAIL_DEBUG")) g_message("Proximity: %s", near ? "near" : "far");
  _changed(near);
}

void Proximity::debugSetNear(bool near) {
  if (_enabled) setNear(near);
}

IdleInhibitor::IdleInhibitor() : _cancellable(g_cancellable_new()) {
}

IdleInhibitor::~IdleInhibitor() {
  g_cancellable_cancel(_cancellable);
  set(false);
  g_object_unref(_cancellable);
}

void IdleInhibitor::set(bool inhibit) {
  if (_requested == inhibit || !SessionBus()) return;
  _requested = inhibit;
  if (!inhibit) {
    uninhibit();
    return;
  }
  if (_pending || _cookie) return;
  _pending = true;
  // Flags: 4 suspending, 8 the session becoming idle (which blanks and then locks the screen).
  g_dbus_connection_call(SessionBus(), "org.gnome.SessionManager", "/org/gnome/SessionManager",
      "org.gnome.SessionManager", "Inhibit", g_variant_new("(susu)", "io.github.x_vlad_x.Contrail", 0u,
      "A call is in progress", 4u | 8u), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, _cancellable,
      +[](GObject *source, GAsyncResult *result, gpointer data) {
        GError *error = nullptr;
        auto *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
        if (Cancelled(error)) {
          g_error_free(error);
          return;
        }
        g_clear_error(&error);
        const auto self = static_cast<IdleInhibitor *>(data);
        self->_pending = false;
        if (!reply) return;
        g_variant_get(reply, "(u)", &self->_cookie);
        g_variant_unref(reply);
        if (!self->_requested) self->uninhibit();
      }, this);
}

void IdleInhibitor::uninhibit() {
  if (!_cookie) return;
  g_dbus_connection_call(SessionBus(), "org.gnome.SessionManager", "/org/gnome/SessionManager",
      "org.gnome.SessionManager", "Uninhibit", g_variant_new("(u)", _cookie), nullptr, G_DBUS_CALL_FLAGS_NONE, -1,
      nullptr, nullptr, nullptr);
  _cookie = 0;
}

} // namespace Contrail
