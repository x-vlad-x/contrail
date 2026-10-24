#pragma once

#include "app.hpp"

#include "base/timer.h"
#include "base/weak_ptr.h"

#include <rpl/event_stream.h>
#include <rpl/lifetime.h>

#include <vector>

class History;
class HistoryItem;

namespace Contrail {

// This account's own live location: sent from here or another device (in Saved Messages too, where messages
// aren't "out").
[[nodiscard]] bool IsMyLiveLocation(not_null<const HistoryItem *> item);

// The live locations shared in a chat now, by everyone (this device's own too): what the chat's bar and the shared
// map show. The server lists the recent ones (messages.getRecentLocations, as the phone apps ask); then they come
// as new messages and their edits, and end with their time. A live location's message keeps its HistoryItem when
// it is sent and its id changes from the local one to the server's.
class ChatLiveLocations final : public base::has_weak_ptr {
public:
  explicit ChatLiveLocations(not_null<History *> history);
  ~ChatLiveLocations();

  // One message per sender (the newest), this account's own first, then by when they were updated.
  [[nodiscard]] const std::vector<not_null<HistoryItem *>> &active() const { return _active; }
  [[nodiscard]] HistoryItem *mine() const;
  [[nodiscard]] rpl::producer<> changes() const { return _changes.events(); }
  // Asks the server for the chat's live locations: their new places too, which don't always come as updates
  // (edits in a large group). The shared map asks every half a minute.
  void request();

private:
  void consider(not_null<HistoryItem *> item);
  void refresh();

  const not_null<History *> _history;
  std::vector<not_null<HistoryItem *>> _known;   // Every live location's message seen, active or not.
  std::vector<not_null<HistoryItem *>> _active;
  mtpRequestId _requestId = 0;
  base::Timer _expireTimer;
  rpl::event_stream<> _changes;
  rpl::lifetime _lifetime;
};

}  // namespace Contrail
