// Positions and live locations (gtk-ui/geo.hpp): distances, stale fixes, when an update goes to the server and
// how long the location service sleeps in between.
#include "geo.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace Contrail;

namespace {

constexpr auto kSecond = gint64(G_USEC_PER_SEC);

GeoFix Fix(double lat, double lon, double accuracy, gint64 time, double heading = -1., double speed = -1.) {
  return {.lat = lat, .lon = lon, .accuracy = accuracy, .heading = heading, .speed = speed, .time = time};
}

}  // namespace

int main() {
  // A degree of latitude is about 111 km; the same point is 0 m away.
  assert(std::abs(DistanceMeters(0., 0., 1., 0.) - 111195.) < 10.);
  assert(DistanceMeters(55.75, 37.62, 55.75, 37.62) < 1e-6);
  // 0.0003° of latitude: about 33 m.
  assert(std::abs(DistanceMeters(46.5, 6.6, 46.5003, 6.6) - 33.4) < 0.5);

  // GeoClue's cached position from an hour ago is not where the device is; a late fix from a slower source isn't
  // either.
  const auto now = 1'800'000'000 * kSecond;
  assert(FreshFix(Fix(0, 0, 10, now - 10 * kSecond), std::nullopt, now));
  assert(!FreshFix(Fix(0, 0, 10, now - 3600 * kSecond), std::nullopt, now));
  assert(!FreshFix(Fix(0, 0, 10, now - 20 * kSecond), Fix(0, 0, 10, now - 5 * kSecond), now));
  assert(FreshFix(Fix(0, 0, 10, now - 5 * kSecond), Fix(0, 0, 10, now - 20 * kSecond), now));

  using Policy = LiveUpdatePolicy;
  const auto sent = Policy::Sent{.lat = 46.5, .lon = 6.6, .heading = -1., .time = now - 60 * kSecond};
  // Standing still (GPS noise inside the accuracy): nothing is sent.
  assert(!Policy::ShouldSend(sent, Fix(46.50005, 6.6, 12, now), now));
  // 33 m away with a 10 m fix: sent; the same with a 50 m fix is noise.
  assert(Policy::ShouldSend(sent, Fix(46.5003, 6.6, 10, now), now));
  assert(!Policy::ShouldSend(sent, Fix(46.5003, 6.6, 50, now), now));
  // Five minutes without an update: sent anyway, so it doesn't look abandoned.
  auto old = sent;
  old.time = now - 301 * kSecond;
  assert(Policy::ShouldSend(old, Fix(46.5, 6.6, 10, now), now));
  // Turning while moving: sent; the heading of a device standing still means nothing.
  auto heading = sent;
  heading.heading = 10.;
  assert(Policy::ShouldSend(heading, Fix(46.5, 6.6, 10, now, 80., 5.), now));
  assert(!Policy::ShouldSend(heading, Fix(46.5, 6.6, 10, now, 80., 0.2), now));
  assert(!Policy::ShouldSend(heading, Fix(46.5, 6.6, 10, now, 20., 5.), now));
  // 350° to 10° is a 20° turn, not 340°.
  heading.heading = 350.;
  assert(!Policy::ShouldSend(heading, Fix(46.5, 6.6, 10, now, 10., 5.), now));

  // The location service sleeps longer the longer the device stays.
  assert(Policy::NextInterval(Fix(0, 0, 10, now, 90., 10.), false, 5) == Policy::kIntervalMoving);
  assert(Policy::NextInterval(Fix(0, 0, 10, now), true, 0) == Policy::kIntervalMoving);
  assert(Policy::NextInterval(Fix(0, 0, 10, now), false, 1) == Policy::kInterval);
  assert(Policy::NextInterval(Fix(0, 0, 10, now), false, Policy::kStillCycles) == Policy::kIntervalStill);

  std::puts("geo: ok");
  return 0;
}
