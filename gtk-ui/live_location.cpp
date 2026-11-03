#include "live_location.hpp"

#include "location_ui.hpp"
#include "map_view.hpp"
#include "td/dates.hpp"
#include "td/strings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace Contrail {
namespace api = Td::api;
namespace {

constexpr auto kAcquireTimeout = 30u;  // Seconds.
constexpr auto kGoodAccuracy = 30.;    // Meters: a fix this good ends the search for one.
constexpr auto kRetryInterval = 120;   // Seconds, after the location service failed.

api::object_ptr<api::location> Point(const GeoFix &fix) {
  return api::make_object<api::location>(fix.lat, fix.lon, std::max(fix.accuracy, 0.));
}

int Heading(const GeoFix &fix) {
  // Telegram takes 1..360, 0 when there is none.
  if (!LiveUpdatePolicy::Moving(fix)) return 0;
  const auto heading = int(std::lround(fix.heading)) % 360;
  return heading ? heading : 360;
}

LiveUpdatePolicy::Sent SentOf(const GeoFix &fix) {
  return {.lat = fix.lat, .lon = fix.lon, .heading = LiveUpdatePolicy::Moving(fix) ? fix.heading : -1.,
          .time = g_get_real_time()};
}

}  // namespace

LiveLocations::LiveLocations() = default;

LiveLocations::~LiveLocations() {
  if (_timer) g_source_remove(_timer);
  if (_acquireTimeout) g_source_remove(_acquireTimeout);
}

void LiveLocations::setStore(Td::Store *store) {
  if (_store == store) return;
  _lifetime.reset();
  _watch = nullptr;
  if (_timer) g_source_remove(std::exchange(_timer, 0));
  if (_acquireTimeout) g_source_remove(std::exchange(_acquireTimeout, 0));
  _shares.clear();
  _store = store;
  if (store) store->client().subscribe([this](api::Update &update) { updated(update); }, _lifetime);
  changed();
}

void LiveLocations::updated(api::Update &update) {
  switch (update.get_id()) {
    case api::updateActiveLiveLocationMessages::ID:
      setActive(static_cast<api::updateActiveLiveLocationMessages &>(update).messages_);
      break;
    case api::updateMessageSendSucceeded::ID: {
      auto &data = static_cast<api::updateMessageSendSucceeded &>(update);
      if (!data.message_) break;
      for (auto &share : _shares) {
        if (share.chat == data.message_->chat_id_ && share.id == data.old_message_id_) {
          share.id = data.message_->id_;
          share.date = data.message_->date_;
          changed();
        }
      }
      break;
    }
    case api::updateMessageSendFailed::ID: {
      auto &data = static_cast<api::updateMessageSendFailed &>(update);
      if (data.message_) forget(data.message_->chat_id_, data.old_message_id_);
      break;
    }
    case api::updateDeleteMessages::ID: {
      auto &data = static_cast<api::updateDeleteMessages &>(update);
      if (!data.is_permanent_) break;
      for (const auto id : data.message_ids_) forget(data.chat_id_, id);
      break;
    }
    default: break;
  }
}

// What TDLib knows is shared from this account now: on start (the shares of the last run), and whenever one starts or
// ends, here or on another device.
void LiveLocations::setActive(const std::vector<api::object_ptr<api::message>> &messages) {
  auto shares = std::vector<Share>();
  for (const auto &message : messages) {
    auto info = LocationInfo();
    if (!message || !MessageLocation(*message, &info) || !info.livePeriod) continue;
    auto share = Share{.chat = message->chat_id_, .id = message->id_, .date = message->date_, .period = info.livePeriod};
    const auto known = std::ranges::find_if(_shares, [&](const Share &s) { return s.chat == share.chat && s.id == share.id; });
    if (known != _shares.end()) {
      share.sent = known->sent;
    } else {
      const auto date = message->edit_date_ ? message->edit_date_ : message->date_;
      share.sent = {.lat = info.lat, .lon = info.lon, .heading = -1., .time = gint64(date) * G_USEC_PER_SEC};
    }
    shares.push_back(share);
  }
  const auto had = _shares.size();
  _shares = std::move(shares);
  g_message("Live locations: %zu active (was %zu)", _shares.size(), had);
  prune();
  if (!_shares.empty() && !_timer && !_watch) schedule(1);
  changed();
}

void LiveLocations::start(Td::ChatId chat, std::int32_t topic, std::int64_t replyTo, std::int32_t period,
                          const GeoFix &fix) {
  if (!_store) return;
  auto content = api::make_object<api::inputMessageLiveLocation>(
      api::make_object<api::liveLocation>(Point(fix), period, Heading(fix), 0));
  auto reply = replyTo ? api::make_object<api::inputMessageReplyToMessage>(replyTo, nullptr, 0, std::string()) : nullptr;
  auto topicObject = topic ? api::make_object<api::messageTopicForum>(topic) : nullptr;
  g_message("Live locations: starting in %lld for %d s", (long long)chat, period);
  _store->client().request<api::message>(
      api::make_object<api::sendMessage>(chat, std::move(topicObject), std::move(reply), nullptr, nullptr,
                                         std::move(content)),
      _lifetime,
      [this, period, fix](api::message &message) {
        if (sharing(message.chat_id_, message.id_)) return;
        _shares.push_back({.chat = message.chat_id_, .id = message.id_, .date = message.date_, .period = period,
                           .sent = SentOf(fix)});
        if (!_timer && !_watch) schedule(LiveUpdatePolicy::kIntervalMoving);
        changed();
      },
      [](api::error &error) { ShowToast(error.message_); });
}

void LiveLocations::stop(Td::ChatId chat, std::int64_t id) {
  if (!_store || !sharing(chat, id)) return;
  forget(chat, id);
  g_message("Live locations: stopping %lld/%lld", (long long)chat, (long long)(id >> 20));
  // Without a location: the server ends it, and the message stays where it was last.
  _store->client().send(api::make_object<api::editMessageLiveLocation>(chat, id, nullptr, nullptr));
}

void LiveLocations::stopAll() {
  const auto shares = _shares;
  for (const auto &share : shares) stop(share.chat, share.id);
}

std::int64_t LiveLocations::sharingIn(Td::ChatId chat) const {
  const auto it = std::ranges::find_if(_shares, [&](const Share &share) { return share.chat == chat; });
  return it == _shares.end() ? 0 : it->id;
}

bool LiveLocations::sharing(Td::ChatId chat, std::int64_t id) const {
  return std::ranges::any_of(_shares, [&](const Share &share) { return share.chat == chat && share.id == id; });
}

void LiveLocations::onChanged(std::function<void()> handler, const Td::Lifetime &lifetime) {
  _subscribers.push_back({std::move(handler), lifetime.token()});
}

void LiveLocations::forget(Td::ChatId chat, std::int64_t id) {
  const auto before = _shares.size();
  std::erase_if(_shares, [&](const Share &share) { return share.chat == chat && share.id == id; });
  if (_shares.size() != before) changed();
}

void LiveLocations::changed() {
  if (_shares.empty()) {
    if (_timer) g_source_remove(std::exchange(_timer, 0));
    if (_acquireTimeout) g_source_remove(std::exchange(_acquireTimeout, 0));
    _watch = nullptr;
    _best = std::nullopt;
  }
  std::erase_if(_subscribers, [](const Subscriber &subscriber) { return subscriber.token.expired(); });
  // A handler may subscribe or go.
  const auto subscribers = _subscribers;
  for (const auto &subscriber : subscribers) {
    if (!subscriber.token.expired()) subscriber.handler();
  }
}

// The server ends a live location by itself when its time is over: nothing to send then.
void LiveLocations::prune() {
  const auto now = Td::Now();
  const auto before = _shares.size();
  std::erase_if(_shares, [&](const Share &share) {
    return share.period != kUntilOff && now >= share.date + share.period;
  });
  if (_shares.size() != before) changed();
}

void LiveLocations::schedule(int seconds) {
  if (_shares.empty()) return;
  if (_timer) g_source_remove(_timer);
  _timer = g_timeout_add_seconds(guint(std::max(seconds, 1)), +[](gpointer data) -> gboolean {
    auto *self = static_cast<LiveLocations *>(data);
    self->_timer = 0;
    self->acquire();
    return G_SOURCE_REMOVE;
  }, this);
}

void LiveLocations::later(std::function<void()> callback) {
  struct Call {
    std::weak_ptr<char> alive;
    std::function<void()> callback;
  };
  g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer data) -> gboolean {
    const auto call = static_cast<Call *>(data);
    if (!call->alive.expired()) call->callback();
    return G_SOURCE_REMOVE;
  }, new Call{_alive, std::move(callback)}, +[](gpointer data) { delete static_cast<Call *>(data); });
}

void LiveLocations::acquire() {
  prune();
  if (_shares.empty() || !_store || _watch) return;
  _best = std::nullopt;
  const auto weak = std::weak_ptr<char>(_alive);
  _watch = Location().watch(
      [this, weak](const GeoFix &fix) {
        if (weak.expired() || !_watch) return;
        if (!_best || fix.accuracy <= _best->accuracy) _best = fix;
        // Not from inside the service's callback: the watch ends here.
        if (fix.accuracy > 0 && fix.accuracy <= kGoodAccuracy) later([this] { finishAcquire(); });
      },
      [this, weak](LocationService::Failure failure) {
        if (weak.expired()) return;
        if (failure == LocationService::Failure::Denied && !_deniedShown) {
          _deniedShown = true;
          ShowToast(Tr("lng_live_location") + ": "
                    + Translated("no access to the location", "нет доступа к геопозиции", "немає доступу до геопозиції"));
        }
        later([this] {
          if (_acquireTimeout) g_source_remove(std::exchange(_acquireTimeout, 0));
          _watch = nullptr;
          schedule(kRetryInterval);
        });
      });
  if (_acquireTimeout) g_source_remove(_acquireTimeout);
  _acquireTimeout = g_timeout_add_seconds(kAcquireTimeout, +[](gpointer data) -> gboolean {
    auto *self = static_cast<LiveLocations *>(data);
    self->_acquireTimeout = 0;
    self->finishAcquire();
    return G_SOURCE_REMOVE;
  }, this);
}

void LiveLocations::finishAcquire() {
  if (!_watch) return;
  _watch = nullptr;
  if (_acquireTimeout) g_source_remove(std::exchange(_acquireTimeout, 0));
  const auto fix = std::exchange(_best, std::nullopt);
  if (!fix) {
    schedule(LiveUpdatePolicy::kInterval);
    return;
  }
  const auto now = g_get_real_time();
  auto moved = false;
  for (auto &share : _shares) {
    if (LiveUpdatePolicy::Moved(share.sent, *fix)) moved = true;
    if (LiveUpdatePolicy::ShouldSend(share.sent, *fix, now)) update(share, *fix);
  }
  _stillCycles = moved ? 0 : (_stillCycles + 1);
  schedule(LiveUpdatePolicy::NextInterval(*fix, moved, _stillCycles));
}

void LiveLocations::update(Share &share, const GeoFix &fix) {
  if (!_store) return;
  share.sent = SentOf(fix);
  const auto chat = share.chat;
  const auto id = share.id;
  g_message("Live locations: %lld/%lld goes to %s", (long long)chat, (long long)(id >> 20),
            FormatCoordinates(fix.lat, fix.lon).c_str());
  _store->client().request<api::message>(
      api::make_object<api::editMessageLiveLocation>(
          chat, id, nullptr, api::make_object<api::liveLocation>(Point(fix), 0, Heading(fix), 0)),
      _lifetime, [](api::message &) {},
      [this, chat, id](api::error &error) {
        // Deleted, or its time is over: there is nothing to update anymore.
        if (error.message_ != "MESSAGE_NOT_MODIFIED") forget(chat, id);
      });
}

std::string LiveLocations::debugState() const {
  char buffer[160];
  std::snprintf(buffer, sizeof(buffer), "shares=%zu waiting=%d timer=%d still=%d", _shares.size(), _watch ? 1 : 0,
                _timer ? 1 : 0, _stillCycles);
  auto result = std::string(buffer);
  for (const auto &share : _shares) {
    result += " [" + std::to_string(share.chat) + "/" + std::to_string(share.id >> 20) + " period="
              + std::to_string(share.period) + " sent=" + FormatCoordinates(share.sent.lat, share.sent.lon) + "]";
  }
  return result;
}

void LiveLocations::debugUpdate() {
  if (_timer) g_source_remove(std::exchange(_timer, 0));
  acquire();
}

LiveLocations &Live() {
  static auto instance = LiveLocations();
  return instance;
}

}  // namespace Contrail
