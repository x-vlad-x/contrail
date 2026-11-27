#pragma once

#include "app.hpp"

#include "geo.hpp"

#include <glib.h>

#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <string>

namespace Contrail {

// The device's position, from GeoClue: directly, or through the desktop portal (in a Flatpak, or when GeoClue
// isn't there). The location service runs (and the GPS with it) only while somebody watches: the location
// picker while it is on the screen, the map after "my location" was tapped, a live location while it takes its
// next fix. Nothing keeps it on otherwise.
class LocationService final {
public:
  enum class Failure {
    Unavailable,  // No location service, or it stopped.
    Denied,       // The user or the system said no.
  };

  // Fixes come while it lives.
  class Watch final {
  public:
    ~Watch();

  private:
    friend class LocationService;
    explicit Watch(int id) : _id(id) {}
    int _id = 0;
  };

  LocationService();
  ~LocationService();

  [[nodiscard]] std::unique_ptr<Watch> watch(std::function<void(const GeoFix &)> fix, std::function<void(Failure)> failed);
  // The last position, if it was measured within `maxAge` seconds.
  [[nodiscard]] std::optional<GeoFix> recent(int maxAge) const;
  // Whether a service is running for the watches now.
  [[nodiscard]] bool running() const;

  // For the debug actions.
  [[nodiscard]] std::string debugState() const;
  void debugFix(double lat, double lon, double accuracy);

private:
  struct Watcher {
    int id = 0;
    std::function<void(const GeoFix &)> fix;
    std::function<void(Failure)> failed;
  };
  class Backend;
  class GeoClue;
  class Portal;

  // A backend may be inside its own callback when it is stopped.
  static void DestroyLater(Backend *backend);
  void release(int id);
  void start();
  void stop();
  void received(const GeoFix &fix);
  void fail(Failure failure);

  std::list<Watcher> _watchers;
  int _lastId = 0;
  std::unique_ptr<Backend> _backend;
  bool _triedPortal = false;
  bool _scripted = false;  // The scenario transport: no backend, fixes from loc-fix.
  std::optional<GeoFix> _last;
  const std::shared_ptr<char> _alive = std::make_shared<char>();
};

[[nodiscard]] LocationService &Location();

}  // namespace Contrail
