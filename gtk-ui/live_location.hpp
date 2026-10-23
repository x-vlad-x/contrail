#pragma once

#include "app.hpp"
#include "location.hpp"

#include "base/timer.h"
#include "base/weak_ptr.h"

#include <rpl/event_stream.h>
#include <rpl/lifetime.h>

#include <vector>

class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace Data {
class Thread;
} // namespace Data

namespace Contrail {

// The live locations being shared from this device. They belong to the application, not to the window: they
// go on in the background, also after a restart (kept in tdata/contrail-live-locations.json), until their time
// is over or they are stopped here or on another device.
//
// The battery: the location service runs only to take a fix and stops as soon as it is good (30 seconds at most):
// every 30 seconds while moving, a minute after stopping, two minutes when the device stays where it is
// (LiveUpdatePolicy in geo.hpp). The message is edited only when the position changed, or once in five minutes.
class LiveLocations final : public base::has_weak_ptr {
public:
  static constexpr auto kUntilOff = TimeId(0x7FFFFFFF);

  LiveLocations();
  ~LiveLocations();

  void setSession(Main::Session *session);

  // Sends a live location to the chat, starting from `fix`.
  void start(not_null<Data::Thread *> thread, TimeId period, const GeoFix &fix, FullMsgId replyTo);
  void stop(FullMsgId id);
  void stopAll();
  // The message of a live location being shared to this chat, if any.
  [[nodiscard]] FullMsgId sharingIn(not_null<PeerData *> peer) const;
  [[nodiscard]] bool sharing(FullMsgId id) const;
  [[nodiscard]] bool any() const { return !_shares.empty(); }
  [[nodiscard]] rpl::producer<> changes() const { return _changes.events(); }

  QString debugState() const;
  void debugUpdate();

private:
  struct Share {
    FullMsgId id;
    TimeId start = 0;
    TimeId period = 0;
    LiveUpdatePolicy::Sent sent;
  };

  void load();
  void save() const;
  void prune();
  void changed();
  void schedule(int seconds);
  void acquire();
  void finishAcquire();
  void update(Share &share, const GeoFix &fix);
  void forget(FullMsgId id);

  Main::Session *_session = nullptr;
  std::vector<Share> _shares;
  std::unique_ptr<LocationService::Watch> _watch;
  std::optional<GeoFix> _best;
  int _stillCycles = 0;
  bool _deniedShown = false;
  base::Timer _timer;
  base::Timer _acquireTimeout;
  rpl::event_stream<> _changes;
  rpl::lifetime _sessionLifetime;
};

[[nodiscard]] LiveLocations &Live();

}  // namespace Contrail
