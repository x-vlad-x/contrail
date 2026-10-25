#pragma once

// Positions and when a live location goes to the server again: no GTK or Telegram here (tests/geo.cpp).

#include <glib.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace Contrail {

// A position of the device.
struct GeoFix {
  double lat = 0.;
  double lon = 0.;
  double accuracy = 0.;  // Meters.
  double heading = -1.;  // Degrees clockwise from north; negative when unknown (standing still).
  double speed = -1.;    // Meters per second; negative when unknown.
  gint64 time = 0;       // When it was measured, g_get_real_time() microseconds.
};

// Meters between two points (haversine).
inline double DistanceMeters(double lat1, double lon1, double lat2, double lon2) {
  constexpr auto kEarthRadius = 6371000.;
  const auto radians = [](double degrees) { return degrees * M_PI / 180.; };
  const auto dlat = radians(lat2 - lat1), dlon = radians(lon2 - lon1);
  const auto a = std::sin(dlat / 2) * std::sin(dlat / 2)
                 + std::cos(radians(lat1)) * std::cos(radians(lat2)) * std::sin(dlon / 2) * std::sin(dlon / 2);
  return 2 * kEarthRadius * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}

// Where a map shows all the points (the shared map of a chat's live locations): its middle and zoom level for a view
// of `width` × `height` pixels, with `margin` pixels kept free around them. Web Mercator with 256-pixel tiles, as
// OpenStreetMap's; a single point (or points close together) gets `maxZoom`.
struct MapFit {
  double lat = 0.;
  double lon = 0.;
  double zoom = 0.;
};

inline MapFit FitPoints(const std::vector<std::pair<double, double>> &points, double width, double height,
                        double margin, double maxZoom) {
  if (points.empty()) return {};
  const auto x = [](double lon) { return (lon + 180.) / 360.; };
  const auto y = [](double lat) {
    const auto clamped = std::clamp(lat, -85.05112878, 85.05112878) * M_PI / 180.;
    return (1. - std::asinh(std::tan(clamped)) / M_PI) / 2.;
  };
  auto minX = 1., maxX = 0., minY = 1., maxY = 0.;
  for (const auto &[lat, lon] : points) {
    minX = std::min(minX, x(lon));
    maxX = std::max(maxX, x(lon));
    minY = std::min(minY, y(lat));
    maxY = std::max(maxY, y(lat));
  }
  constexpr auto kTile = 256.;
  const auto fit = [&](double size, double span) {
    const auto room = std::max(size - 2 * margin, 1.);
    return (span * kTile > 0.) ? std::log2(room / (span * kTile)) : maxZoom;
  };
  const auto zoom = std::clamp(std::min(fit(width, maxX - minX), fit(height, maxY - minY)), 1., maxZoom);
  const auto middleY = (minY + maxY) / 2.;
  return {
      .lat = std::atan(std::sinh(M_PI * (1. - 2. * middleY))) * 180. / M_PI,
      .lon = (minX + maxX) / 2. * 360. - 180.,
      .zoom = zoom,
  };
}

// GeoClue reports the last position it knows when a client starts, however old, and a slow source may report
// after a faster one: those aren't where the device is now.
inline bool FreshFix(const GeoFix &fix, const std::optional<GeoFix> &previous, gint64 now) {
  constexpr auto kMaxAge = gint64(5 * 60) * G_USEC_PER_SEC;
  if (now - fix.time > kMaxAge) return false;
  return !previous || fix.time >= previous->time;
}

// When a live location goes to the server again, and when the next fix is taken.
struct LiveUpdatePolicy {
  static constexpr auto kMinMove = 25.;        // Meters, or the fix's accuracy if that is worse.
  static constexpr auto kMinTurn = 30.;        // Degrees of heading, while moving.
  static constexpr auto kMovingSpeed = 1.5;    // Meters per second: faster is moving.
  static constexpr auto kKeepAlive = 5 * 60;   // Seconds: "updated N minutes ago" doesn't grow old.
  static constexpr auto kIntervalMoving = 30;  // Seconds between fixes while moving,
  static constexpr auto kInterval = 60;        // after it stopped,
  static constexpr auto kIntervalStill = 120;  // and when it stayed for a while.
  static constexpr auto kStillCycles = 3;

  struct Sent {
    double lat = 0.;
    double lon = 0.;
    double heading = -1.;
    gint64 time = 0;  // g_get_real_time().
  };

  static bool Moving(const GeoFix &fix) {
    return fix.heading >= 0 && fix.speed >= kMovingSpeed;
  }
  static bool Moved(const Sent &sent, const GeoFix &fix) {
    return DistanceMeters(sent.lat, sent.lon, fix.lat, fix.lon) >= std::max(kMinMove, fix.accuracy);
  }
  static bool ShouldSend(const Sent &sent, const GeoFix &fix, gint64 now) {
    if (now - sent.time >= gint64(kKeepAlive) * G_USEC_PER_SEC) return true;
    if (Moved(sent, fix)) return true;
    if (Moving(fix) && sent.heading >= 0) {
      return std::abs(std::remainder(fix.heading - sent.heading, 360.)) >= kMinTurn;
    }
    return false;
  }
  static int NextInterval(const GeoFix &fix, bool moved, int stillCycles) {
    if (Moving(fix) || moved) return kIntervalMoving;
    return (stillCycles >= kStillCycles) ? kIntervalStill : kInterval;
  }
};

}  // namespace Contrail
