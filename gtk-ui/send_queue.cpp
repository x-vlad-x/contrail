#include "send_queue.hpp"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/timer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "storage/localimageloader.h"
#include "ui/chat/attach/attach_prepare.h"

#include <rpl/lifetime.h>

#include <deque>
#include <map>

namespace Contrail {
namespace {

// The core prepares the files of a group in the background before their messages appear; a group that
// never shows up (a file that could not be read) doesn't hold the next ones back for longer than this.
constexpr auto kAppearTimeout = crl::time(60 * 1000);

struct Pending {
  Ui::PreparedGroup group;
  SendMediaType type;
  Api::SendAction action;
};

class HistoryQueue final {
public:
  HistoryQueue(not_null<History *> history, Fn<void()> idle);

  void add(Pending &&pending);

private:
  void next();
  void check();

  const not_null<History *> _history;
  const Fn<void()> _idle;
  std::deque<Pending> _pending;
  bool _sending = false;
  int _expected = 0;  // Messages of the group being sent that haven't appeared yet.
  std::vector<not_null<HistoryItem *>> _waiting;
  base::Timer _appearTimer;
  rpl::lifetime _lifetime;
};

HistoryQueue::HistoryQueue(not_null<History *> history, Fn<void()> idle)
    : _history(history), _idle(std::move(idle)), _appearTimer([=] {
        _expected = 0;
        check();
      }) {
  auto &data = history->owner();
  data.newItemAdded() | rpl::on_next([=](not_null<HistoryItem *> item) {
    if (g_getenv("CONTRAIL_DEBUG") && item->history() == _history) {
      g_message("Send queue: new item %lld sending=%d media=%d expected=%d", (long long)item->id.bare,
                int(item->isSending()), int(item->media() != nullptr), _expected);
    }
    // Being sent means it is ours (out() is false in Saved Messages).
    if (_expected > 0 && item->history() == _history && item->isSending() && item->media()) {
      _waiting.push_back(item);
      if (--_expected == 0) _appearTimer.cancel();
    }
  }, _lifetime);
  // Sent (the server's id), failed, or deleted while sending: the group no longer holds the next one.
  data.itemIdChanged() | rpl::on_next([=] { check(); }, _lifetime);
  data.itemDataChanges() | rpl::on_next([=] { check(); }, _lifetime);
  data.itemRemoved() | rpl::on_next([=](not_null<const HistoryItem *> item) {
    _waiting.erase(std::remove(_waiting.begin(), _waiting.end(), item), _waiting.end());
    crl::on_main(&_history->session(), [=] { check(); });
  }, _lifetime);
}

void HistoryQueue::add(Pending &&pending) {
  _pending.push_back(std::move(pending));
  if (!_sending) next();
}

void HistoryQueue::next() {
  if (_pending.empty()) {
    _sending = false;
    _idle();
    return;
  }
  auto pending = std::move(_pending.front());
  _pending.pop_front();
  _sending = true;
  _waiting.clear();
  _expected = int(pending.group.list.files.size());
  _appearTimer.callOnce(kAppearTimeout);
  const auto album = (pending.group.type != Ui::AlbumType::None) ? std::make_shared<SendingAlbum>() : nullptr;
  _history->session().api().sendFiles(std::move(pending.group.list), pending.type, album, pending.action);
}

void HistoryQueue::check() {
  if (g_getenv("CONTRAIL_DEBUG") && _sending) {
    auto sending = 0;
    for (const auto &item : _waiting) sending += item->isSending() ? 1 : 0;
    g_message("Send queue: check expected=%d waiting=%d sending=%d pending=%d", _expected, int(_waiting.size()),
              sending, int(_pending.size()));
  }
  if (!_sending || _expected > 0) return;
  for (const auto &item : _waiting) {
    if (item->isSending()) return;
  }
  _waiting.clear();
  next();
}

class SessionQueues final {
public:
  void add(not_null<History *> history, Pending &&pending) {
    auto &queue = _queues[history];
    if (!queue) {
      queue = std::make_unique<HistoryQueue>(history, [=] {
        // Not from within the queue's own callback.
        crl::on_main(&history->session(), [=] { _queues.erase(history); });
      });
    }
    queue->add(std::move(pending));
  }

private:
  std::map<not_null<History *>, std::unique_ptr<HistoryQueue>> _queues;
};

SessionQueues &QueuesFor(not_null<Main::Session *> session) {
  static auto queues = std::map<not_null<Main::Session *>, std::unique_ptr<SessionQueues>>();
  auto &result = queues[session];
  if (!result) {
    result = std::make_unique<SessionQueues>();
    session->lifetime().add([=] { queues.erase(session); });
  }
  return *result;
}

}  // namespace

void SendFileGroups(not_null<Main::Session *> session, std::vector<Ui::PreparedGroup> &&groups, SendMediaType type,
                    const Api::SendAction &action) {
  auto &queues = QueuesFor(session);
  for (auto &group : groups) {
    queues.add(action.history, Pending{std::move(group), type, action});
  }
}

}  // namespace Contrail
