#pragma once

#include "geo.hpp"
#include "location.hpp"
#include "td/store.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Contrail {

// The live locations being shared from this account. They belong to the application, not to the window: they go
// on in the background, also after a restart (TDLib keeps them and tells about them with
// updateActiveLiveLocationMessages), until their time is over or they are stopped here or on another device.
//
// The battery: the location service runs only to take a fix and stops as soon as it is good (30 seconds at most):
// every 30 seconds while moving, a minute after stopping, two minutes when the device stays where it is
// (LiveUpdatePolicy in geo.hpp). The message is edited only when the position changed, or once in five minutes.
class LiveLocations final {
 public:
  LiveLocations();
  ~LiveLocations();

  // The account's store; null while there is none (the client goes or changes).
  void setStore(Td::Store *store);

  // Sends a live location to the chat (to the topic, as a reply), starting from `fix`.
  void start(Td::ChatId chat, std::int32_t topic, std::int64_t replyTo, std::int32_t period, const GeoFix &fix);
  void stop(Td::ChatId chat, std::int64_t id);
  void stopAll();
  // The message of a live location being shared to this chat, 0 if there is none.
  [[nodiscard]] std::int64_t sharingIn(Td::ChatId chat) const;
  [[nodiscard]] bool sharing(Td::ChatId chat, std::int64_t id) const;
  [[nodiscard]] bool any() const { return !_shares.empty(); }
  // Called when a live location starts or ends.
  void onChanged(std::function<void()> handler, const Td::Lifetime &lifetime);

  [[nodiscard]] std::string debugState() const;
  void debugUpdate();

 private:
  struct Share {
    Td::ChatId chat = 0;
    std::int64_t id = 0;
    std::int32_t date = 0;
    std::int32_t period = 0;
    LiveUpdatePolicy::Sent sent;
  };
  struct Subscriber {
    std::function<void()> handler;
    std::weak_ptr<void> token;
  };

  void updated(Td::api::Update &update);
  void setActive(const std::vector<Td::api::object_ptr<Td::api::message>> &messages);
  void prune();
  void changed();
  void schedule(int seconds);
  void acquire();
  void finishAcquire();
  void update(Share &share, const GeoFix &fix);
  void forget(Td::ChatId chat, std::int64_t id);
  void later(std::function<void()> callback);

  Td::Store *_store = nullptr;
  Td::Lifetime _lifetime;
  std::vector<Share> _shares;
  std::vector<Subscriber> _subscribers;
  std::unique_ptr<LocationService::Watch> _watch;
  std::optional<GeoFix> _best;
  int _stillCycles = 0;
  bool _deniedShown = false;
  guint _timer = 0;
  guint _acquireTimeout = 0;
  const std::shared_ptr<char> _alive = std::make_shared<char>();
};

[[nodiscard]] LiveLocations &Live();

}  // namespace Contrail
