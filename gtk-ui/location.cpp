#include "location.hpp"

#include "base/weak_ptr.h"

#include <gio/gio.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Contrail {
namespace {

constexpr char kGeoClueName[] = "org.freedesktop.GeoClue2";
constexpr char kGeoClueManager[] = "/org/freedesktop/GeoClue2/Manager";
constexpr char kGeoClueManagerInterface[] = "org.freedesktop.GeoClue2.Manager";
constexpr char kGeoClueClientInterface[] = "org.freedesktop.GeoClue2.Client";
constexpr char kGeoClueLocationInterface[] = "org.freedesktop.GeoClue2.Location";
constexpr auto kGeoClueAccuracyExact = 8u;

constexpr char kPortalName[] = "org.freedesktop.portal.Desktop";
constexpr char kPortalPath[] = "/org/freedesktop/portal/desktop";
constexpr char kPortalLocation[] = "org.freedesktop.portal.Location";
constexpr auto kPortalAccuracyExact = 5u;

constexpr char kProperties[] = "org.freedesktop.DBus.Properties";

using Failure = LocationService::Failure;

bool InFlatpak() {
  return g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS);
}

// The desktop file GeoClue and the portal ask the user about.
const char *DesktopId() {
  auto *application = g_application_get_default();
  const auto id = application ? g_application_get_application_id(application) : nullptr;
  return id ? id : kAppId;
}

Failure FailureOf(const GError *error) {
  if (!error) return Failure::Unavailable;
  if (g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED)
      || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)) {
    return Failure::Denied;
  }
  if (auto *remote = g_dbus_error_get_remote_error(error)) {
    const auto denied = g_str_has_suffix(remote, ".AccessDenied") || g_str_has_suffix(remote, ".NotAllowed");
    g_free(remote);
    if (denied) return Failure::Denied;
  }
  return Failure::Unavailable;
}

// An asynchronous D-Bus call; `done` gets the reply or the error (never after the cancellable is cancelled).
void Call(GDBusConnection *bus, const char *name, const char *path, const char *interface, const char *method,
          GVariant *parameters, GCancellable *cancellable, Fn<void(GVariant *reply, GError *error)> done) {
  g_dbus_connection_call(
      bus, name, path, interface, method, parameters, nullptr, G_DBUS_CALL_FLAGS_NONE, -1, cancellable,
      +[](GObject *source, GAsyncResult *result, gpointer data) {
        const auto done = std::unique_ptr<Fn<void(GVariant *, GError *)>>(static_cast<Fn<void(GVariant *, GError *)> *>(data));
        GError *error = nullptr;
        auto *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
          RunCoreEvent([&] { (*done)(reply, error); });
        }
        if (reply) g_variant_unref(reply);
        if (error) g_error_free(error);
      },
      new Fn<void(GVariant *, GError *)>(std::move(done)));
}

void CallAndForget(GDBusConnection *bus, const char *name, const char *path, const char *interface, const char *method,
                   GVariant *parameters) {
  g_dbus_connection_call(bus, name, path, interface, method, parameters, nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                         nullptr, nullptr);
}

using SignalCallback = Fn<void(const char *path, GVariant *parameters)>;

guint Subscribe(GDBusConnection *bus, const char *sender, const char *interface, const char *member, const char *path,
                SignalCallback callback) {
  return g_dbus_connection_signal_subscribe(
      bus, sender, interface, member, path, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
      +[](GDBusConnection *, const char *, const char *path, const char *, const char *, GVariant *parameters,
          gpointer data) {
        const auto callback = *static_cast<SignalCallback *>(data);
        RunCoreEvent([&] { callback(path, parameters); });
      },
      new SignalCallback(std::move(callback)), +[](gpointer data) { delete static_cast<SignalCallback *>(data); });
}

// The fields of GeoClue's Location object and of the portal's LocationUpdated signal are the same.
std::optional<GeoFix> ParseFix(GVariant *dict) {
  auto fix = GeoFix();
  if (!g_variant_lookup(dict, "Latitude", "d", &fix.lat) || !g_variant_lookup(dict, "Longitude", "d", &fix.lon)) {
    return std::nullopt;
  }
  g_variant_lookup(dict, "Accuracy", "d", &fix.accuracy);
  if (!g_variant_lookup(dict, "Heading", "d", &fix.heading)) fix.heading = -1.;
  if (!g_variant_lookup(dict, "Speed", "d", &fix.speed)) fix.speed = -1.;
  guint64 seconds = 0, microseconds = 0;
  if (g_variant_lookup(dict, "Timestamp", "(tt)", &seconds, &microseconds) && seconds) {
    fix.time = gint64(seconds) * G_USEC_PER_SEC + gint64(microseconds);
  } else {
    fix.time = g_get_real_time();
  }
  if (!std::isfinite(fix.lat) || !std::isfinite(fix.lon) || std::abs(fix.lat) > 90. || std::abs(fix.lon) > 180.) {
    return std::nullopt;
  }
  return fix;
}

}  // namespace

// --- backends ----------------------------------------------------------------

class LocationService::Backend : public base::has_weak_ptr {
public:
  explicit Backend(not_null<LocationService *> service) : _service(service), _cancellable(g_cancellable_new()) {}
  virtual ~Backend() { detach(); }

  // No more fixes nor failures from it, before it is destroyed (later: it may be inside its own callback).
  void detach() {
    if (_detached) return;
    _detached = true;
    g_cancellable_cancel(_cancellable);
    g_clear_object(&_cancellable);
    if (_bus) {
      for (const auto id : _subscriptions) g_dbus_connection_signal_unsubscribe(_bus, id);
      _subscriptions.clear();
      cleanup();
      g_clear_object(&_bus);
    }
  }

protected:
  // Releases what it holds on the service's side (the client, the session).
  virtual void cleanup() = 0;

  void received(const GeoFix &fix) {
    if (!_detached) _service->received(fix);
  }
  void failed(Failure failure) {
    if (!_detached) _service->fail(failure);
  }
  // Calls that end in a failure of the whole backend when they fail.
  void call(const char *name, const char *path, const char *interface, const char *method, GVariant *parameters,
            Fn<void(GVariant *)> done) {
    const auto weak = base::make_weak(this);
    Call(_bus, name, path, interface, method, parameters, _cancellable, [=](GVariant *reply, GError *error) {
      const auto strong = weak.get();
      if (!strong || strong->_detached) return;
      if (error) {
        g_message("Location: %s.%s failed: %s", interface, method, error->message);
        strong->failed(FailureOf(error));
      } else {
        done(reply);
      }
    });
  }
  void subscribe(const char *sender, const char *interface, const char *member, const char *path,
                 Fn<void(const char *, GVariant *)> callback) {
    const auto weak = base::make_weak(this);
    _subscriptions.push_back(Subscribe(_bus, sender, interface, member, path, [=](const char *path, GVariant *parameters) {
      if (const auto strong = weak.get(); strong && !strong->_detached) callback(path, parameters);
    }));
  }

  const not_null<LocationService *> _service;
  GDBusConnection *_bus = nullptr;
  GCancellable *_cancellable = nullptr;
  bool _detached = false;
  std::vector<guint> _subscriptions;
};

// GeoClue's own D-Bus interface: a client with the app's desktop id, exact accuracy. GeoClue asks its agent
// (the shell) whether the app may know where the device is.
class LocationService::GeoClue final : public Backend {
public:
  explicit GeoClue(not_null<LocationService *> service) : Backend(service) {
    _bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
    if (!_bus) {
      crl::on_main(this, [=] { failed(Failure::Unavailable); });
      return;
    }
    // CreateClient (GeoClue 2.5) gives every caller a client of its own; GetClient is the older one.
    const auto weak = base::make_weak(this);
    Call(_bus, kGeoClueName, kGeoClueManager, kGeoClueManagerInterface, "CreateClient", nullptr, _cancellable,
         [=](GVariant *reply, GError *error) {
           const auto strong = weak.get();
           if (!strong || _detached) return;
           if (g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD)) {
             call(kGeoClueName, kGeoClueManager, kGeoClueManagerInterface, "GetClient", nullptr,
                  [=](GVariant *reply) { created(reply); });
           } else if (error) {
             g_message("Location: GeoClue unavailable: %s", error->message);
             failed(FailureOf(error));
           } else {
             created(reply);
           }
         });
  }
  ~GeoClue() { detach(); }

private:
  void cleanup() override {
    if (_client.empty()) return;
    CallAndForget(_bus, kGeoClueName, _client.c_str(), kGeoClueClientInterface, "Stop", nullptr);
    CallAndForget(_bus, kGeoClueName, kGeoClueManager, kGeoClueManagerInterface, "DeleteClient",
                  g_variant_new("(o)", _client.c_str()));
    _client.clear();
  }

  void created(GVariant *reply) {
    const char *path = nullptr;
    g_variant_get(reply, "(&o)", &path);
    _client = path;
    const auto set = [=](const char *property, GVariant *value, Fn<void(GVariant *)> then) {
      call(kGeoClueName, _client.c_str(), kProperties, "Set",
           g_variant_new("(ssv)", kGeoClueClientInterface, property, value), std::move(then));
    };
    set("DesktopId", g_variant_new_string(DesktopId()), [=](GVariant *) {
      set("RequestedAccuracyLevel", g_variant_new_uint32(kGeoClueAccuracyExact), [=](GVariant *) {
        subscribe(kGeoClueName, kGeoClueClientInterface, "LocationUpdated", _client.c_str(),
                  [=](const char *, GVariant *parameters) {
                    const char *location = nullptr, *previous = nullptr;
                    g_variant_get(parameters, "(&o&o)", &previous, &location);
                    read(location);
                  });
        // GeoClue may stop the client (the agent took the permission back): no more fixes come then.
        subscribe(kGeoClueName, kProperties, "PropertiesChanged", _client.c_str(), [=](const char *, GVariant *parameters) {
          GVariant *changed = nullptr;
          g_variant_get(parameters, "(&s@a{sv}@as)", nullptr, &changed, nullptr);
          gboolean active = TRUE;
          if (g_variant_lookup(changed, "Active", "b", &active) && !active && _started) {
            g_message("Location: GeoClue stopped the client");
            failed(Failure::Unavailable);
          }
          g_variant_unref(changed);
        });
        call(kGeoClueName, _client.c_str(), kGeoClueClientInterface, "Start", nullptr, [=](GVariant *) {
          _started = true;
        });
      });
    });
  }

  void read(const char *location) {
    call(kGeoClueName, location, kProperties, "GetAll", g_variant_new("(s)", kGeoClueLocationInterface),
         [=](GVariant *reply) {
           GVariant *dict = nullptr;
           g_variant_get(reply, "(@a{sv})", &dict);
           if (const auto fix = ParseFix(dict)) received(*fix);
           g_variant_unref(dict);
         });
  }

  std::string _client;
  bool _started = false;
};

// The desktop portal's Location interface (xdg-desktop-portal asks GeoClue for the app, after asking the user).
class LocationService::Portal final : public Backend {
public:
  explicit Portal(not_null<LocationService *> service) : Backend(service) {
    _bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!_bus) {
      crl::on_main(this, [=] { failed(Failure::Unavailable); });
      return;
    }
    static auto counter = 0;
    const auto token = "contrail" + std::to_string(++counter);
    auto sender = std::string(g_dbus_connection_get_unique_name(_bus) + 1);
    std::replace(sender.begin(), sender.end(), '.', '_');
    _session = std::string(kPortalPath) + "/session/" + sender + "/" + token;
    const auto request = std::string(kPortalPath) + "/request/" + sender + "/" + token;

    subscribe(kPortalName, kPortalLocation, "LocationUpdated", kPortalPath, [=](const char *, GVariant *parameters) {
      const char *session = nullptr;
      GVariant *dict = nullptr;
      g_variant_get(parameters, "(&o@a{sv})", &session, &dict);
      if (_session == session) {
        if (const auto fix = ParseFix(dict)) received(*fix);
      }
      g_variant_unref(dict);
    });
    subscribe(kPortalName, "org.freedesktop.portal.Request", "Response", request.c_str(),
              [=](const char *, GVariant *parameters) {
                guint32 response = 2;
                g_variant_get(parameters, "(u@a{sv})", &response, nullptr);
                if (response != 0) {
                  g_message("Location: the portal answered %u", response);
                  failed(response == 1 ? Failure::Denied : Failure::Unavailable);
                }
              });
    // The interface is there only when xdg-desktop-portal was built with GeoClue.
    call(kPortalName, kPortalPath, kProperties, "Get", g_variant_new("(ss)", kPortalLocation, "version"),
         [=](GVariant *) {
           auto options = GVariantBuilder();
           g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
           g_variant_builder_add(&options, "{sv}", "session_handle_token", g_variant_new_string(token.c_str()));
           g_variant_builder_add(&options, "{sv}", "accuracy", g_variant_new_uint32(kPortalAccuracyExact));
           call(kPortalName, kPortalPath, kPortalLocation, "CreateSession", g_variant_new("(a{sv})", &options),
                [=](GVariant *reply) {
                  const char *session = nullptr;
                  g_variant_get(reply, "(&o)", &session);
                  _session = session;
                  _created = true;
                  auto options = GVariantBuilder();
                  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
                  g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
                  call(kPortalName, kPortalPath, kPortalLocation, "Start",
                       g_variant_new("(osa{sv})", session, "", &options), [](GVariant *) {});
                });
         });
  }
  ~Portal() { detach(); }

private:
  void cleanup() override {
    if (!_created) return;
    CallAndForget(_bus, kPortalName, _session.c_str(), "org.freedesktop.portal.Session", "Close", nullptr);
    _created = false;
  }

  std::string _session;
  bool _created = false;
};

// --- the service -------------------------------------------------------------

LocationService::Watch::~Watch() {
  Location().release(_id);
}

LocationService::LocationService() = default;

LocationService::~LocationService() {
  if (_backend) _backend->detach();
}

std::unique_ptr<LocationService::Watch> LocationService::watch(Fn<void(const GeoFix &)> fix, Fn<void(Failure)> failed) {
  const auto id = ++_lastId;
  _watchers.push_back({.id = id, .fix = std::move(fix), .failed = std::move(failed)});
  if (!_backend) {
    start();
  } else if (const auto last = recent(30)) {
    // Something to show while the next fix comes.
    crl::on_main([=] {
      for (const auto &watcher : _watchers) {
        if (watcher.id == id) {
          const auto callback = watcher.fix;
          callback(*last);
          break;
        }
      }
    });
  }
  return std::unique_ptr<Watch>(new Watch(id));
}

std::optional<GeoFix> LocationService::recent(int maxAge) const {
  if (!_last || g_get_real_time() - _last->time > gint64(maxAge) * G_USEC_PER_SEC) return std::nullopt;
  return _last;
}

bool LocationService::running() const {
  return _backend != nullptr;
}

void LocationService::release(int id) {
  _watchers.remove_if([&](const Watcher &watcher) { return watcher.id == id; });
  if (_watchers.empty()) stop();
}

void LocationService::start() {
  _backend = (InFlatpak() || _triedPortal) ? std::unique_ptr<Backend>(std::make_unique<Portal>(this))
                                           : std::unique_ptr<Backend>(std::make_unique<GeoClue>(this));
  g_message("Location: started (%s)", (InFlatpak() || _triedPortal) ? "portal" : "GeoClue");
}

void LocationService::stop() {
  _triedPortal = false;
  if (_backend) {
    g_message("Location: stopped");
    _backend->detach();
    DestroyLater(_backend.release());
  }
}

void LocationService::received(const GeoFix &fix) {
  if (!FreshFix(fix, _last, g_get_real_time())) return;
  _last = fix;
  // A watcher may go (or come) from its callback.
  auto callbacks = std::vector<std::pair<int, Fn<void(const GeoFix &)>>>();
  for (const auto &watcher : _watchers) callbacks.emplace_back(watcher.id, watcher.fix);
  for (const auto &[id, callback] : callbacks) {
    if (std::any_of(_watchers.begin(), _watchers.end(), [&](const Watcher &w) { return w.id == id; })) {
      callback(fix);
    }
  }
}

void LocationService::fail(Failure failure) {
  if (!_backend) return;
  _backend->detach();
  DestroyLater(_backend.release());
  if (failure == Failure::Unavailable && !InFlatpak() && !_triedPortal) {
    // No GeoClue on the system bus (or it can't be reached): the portal may still have it.
    _triedPortal = true;
    start();
    return;
  }
  _triedPortal = false;
  auto callbacks = std::vector<std::pair<int, Fn<void(Failure)>>>();
  for (const auto &watcher : _watchers) callbacks.emplace_back(watcher.id, watcher.failed);
  for (const auto &[id, callback] : callbacks) {
    if (callback && std::any_of(_watchers.begin(), _watchers.end(), [&](const Watcher &w) { return w.id == id; })) {
      callback(failure);
    }
  }
}

QString LocationService::debugState() const {
  auto result = u"watchers=%1 running=%2"_q.arg(_watchers.size()).arg(_backend ? 1 : 0);
  if (_last) {
    result += u" last=%1,%2 accuracy=%3 age=%4s"_q.arg(_last->lat, 0, 'f', 6)
                  .arg(_last->lon, 0, 'f', 6)
                  .arg(_last->accuracy)
                  .arg((g_get_real_time() - _last->time) / G_USEC_PER_SEC);
  }
  return result;
}

void LocationService::debugFix(double lat, double lon, double accuracy) {
  received({.lat = lat, .lon = lon, .accuracy = accuracy, .time = g_get_real_time()});
}

void LocationService::DestroyLater(Backend *backend) {
  g_idle_add(+[](gpointer data) -> gboolean {
    delete static_cast<Backend *>(data);
    return G_SOURCE_REMOVE;
  }, backend);
}

LocationService &Location() {
  static auto instance = LocationService();
  return instance;
}

}  // namespace Contrail
