#include "chat_live_locations.hpp"

#include "live_location.hpp"
#include "location_ui.hpp"

#include "apiwrap.h"
#include "base/unixtime.h"
#include "data/data_changes.h"
#include "data/data_location.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/view/history_view_element.h"
#include "main/main_session.h"

#include <algorithm>

namespace Contrail {
namespace {

constexpr auto kRecentLimit = 100;

TimeId UpdateDate(not_null<const HistoryItem *> item) {
  const auto edited = item->Get<HistoryMessageEdited>();
  return edited ? edited->date : item->date();
}

bool IsLiveLocation(not_null<const HistoryItem *> item) {
  const auto location = LocationMedia(item);
  return location && location->livePeriod() > 0;
}

}  // namespace

bool IsMyLiveLocation(not_null<const HistoryItem *> item) {
  return item->out() || item->from()->isSelf();
}

ChatLiveLocations::ChatLiveLocations(not_null<History *> history)
    : _history(history), _expireTimer([=] { refresh(); }) {
  auto &data = _history->owner();
  data.newItemAdded() | rpl::filter([=](not_null<HistoryItem *> item) {
    return item->history() == _history;
  }) | rpl::on_next([=](not_null<HistoryItem *> item) {
    consider(item);
  }, _lifetime);
  // Stopped (an edit ends it), moved, or the media came with an edit.
  _history->session().changes().messageUpdates(Data::MessageUpdate::Flag::Edited)
      | rpl::filter([=](const Data::MessageUpdate &update) { return update.item->history() == _history; })
      | rpl::on_next([=](const Data::MessageUpdate &update) { consider(update.item); }, _lifetime);
  data.itemRemoved() | rpl::on_next([=](not_null<const HistoryItem *> item) {
    const auto i = std::find(_known.begin(), _known.end(), item);
    if (i == _known.end()) return;
    _known.erase(i);
    refresh();
  }, _lifetime);
  // Live and Contrail's own sharing state: a share stopped here ends before the server's edit comes.
  Live().changes() | rpl::on_next([=] { refresh(); }, _lifetime);

  for (const auto &block : _history->blocks) {
    for (const auto &view : block->messages) {
      if (IsLiveLocation(view->data())) _known.push_back(view->data());
    }
  }
  refresh();
  request();
}

ChatLiveLocations::~ChatLiveLocations() {
  if (_requestId) _history->session().api().request(_requestId).cancel();
}

HistoryItem *ChatLiveLocations::mine() const {
  return (!_active.empty() && IsMyLiveLocation(_active.front())) ? _active.front().get() : nullptr;
}

void ChatLiveLocations::request() {
  if (_requestId) return;
  const auto session = &_history->session();
  const auto weak = base::make_weak(this);
  _requestId = session->api()
                   .request(MTPmessages_GetRecentLocations(_history->peer->input(), MTP_int(kRecentLimit), MTP_long(0)))
                   .done([=](const MTPmessages_Messages &result) {
                     const auto strong = weak.get();
                     if (strong) strong->_requestId = 0;
                     result.match([](const MTPDmessages_messagesNotModified &) {}, [&](const auto &data) {
                       auto &owner = session->data();
                       owner.processUsers(data.vusers());
                       owner.processChats(data.vchats());
                       for (const auto &message : data.vmessages().v) {
                         const auto id = IdFromMessage(message);
                         const auto peerId = PeerFromMessage(message);
                         if (owner.message(peerId, id)) {
                           // Its place now.
                           owner.updateEditedMessage(message);
                         } else {
                           owner.addNewMessage(message, MessageFlags(), NewMessageType::Existing);
                         }
                         if (const auto item = owner.message(peerId, id); item && strong) {
                           strong->consider(item);
                         }
                       }
                     });
                   })
                   .fail([=] {
                     if (const auto strong = weak.get()) strong->_requestId = 0;
                   })
                   .send();
}

void ChatLiveLocations::consider(not_null<HistoryItem *> item) {
  if (item->history() != _history) return;
  const auto known = std::find(_known.begin(), _known.end(), item) != _known.end();
  if (IsLiveLocation(item)) {
    if (!known) _known.push_back(item);
    refresh();
  } else if (known) {
    // The media changed: not a live location anymore.
    _known.erase(std::find(_known.begin(), _known.end(), item));
    refresh();
  }
}

void ChatLiveLocations::refresh() {
  const auto now = base::unixtime::now();
  auto active = std::vector<not_null<HistoryItem *>>();
  auto nextEnd = TimeId(0);
  for (const auto &item : _known) {
    if (!LiveLocationActive(item) || item->isSending() || item->hasFailed()) continue;
    // One per sender: the newest of theirs.
    const auto from = item->from();
    const auto same = std::find_if(active.begin(), active.end(), [&](not_null<HistoryItem *> other) {
      return other->from() == from;
    });
    if (same != active.end()) {
      if ((*same)->date() >= item->date()) continue;
      *same = item;
    } else {
      active.push_back(item);
    }
    const auto period = LocationMedia(item)->livePeriod();
    if (period != LiveLocations::kUntilOff) {
      const auto end = item->date() + period;
      if (!nextEnd || end < nextEnd) nextEnd = end;
    }
  }
  std::stable_sort(active.begin(), active.end(), [](not_null<HistoryItem *> a, not_null<HistoryItem *> b) {
    if (IsMyLiveLocation(a) != IsMyLiveLocation(b)) return IsMyLiveLocation(a);
    return UpdateDate(a) > UpdateDate(b);
  });
  if (nextEnd) {
    _expireTimer.callOnce(std::max(nextEnd - now, 1) * crl::time(1000) + 500);
  } else {
    _expireTimer.cancel();
  }
  // The same people may be at new places: whoever shows them asks the items again.
  _active = std::move(active);
  _changes.fire({});
}

}  // namespace Contrail
