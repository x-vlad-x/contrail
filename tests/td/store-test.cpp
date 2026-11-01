// The store on scripted updates: no network, no account.
#include "td/store.hpp"

#include <glib.h>

namespace Contrail {
namespace {

namespace api = Td::api;
using Td::ListKind;

class ScriptedTransport final : public Td::Transport {
 public:
  explicit ScriptedTransport(Sink &sink, std::vector<std::int32_t> &sent) : _sink(sink), _sent(sent) {}
  void start(Sink sink) override { _sink = std::move(sink); }
  void send(std::uint64_t id, Td::FunctionPtr function) override {
    _sent.push_back(function->get_id());
    _ids.push_back(id);
    last = id;
  }
  std::uint64_t last = 0;

 private:
  Sink &_sink;
  std::vector<std::int32_t> &_sent;
  std::vector<std::uint64_t> _ids;
};

struct Fixture {
  Td::Transport::Sink sink;
  std::vector<std::int32_t> sent;
  ScriptedTransport *transport = nullptr;
  Td::Client client;
  Td::Store store;

  Fixture()
      : client([this] {
          auto made = std::make_unique<ScriptedTransport>(sink, sent);
          transport = made.get();
          return made;
        }()),
        store(client, [](const api::message &message) { return "message " + std::to_string(message.id_); }) {}

  void update(api::object_ptr<api::Update> update) { sink(0, std::move(update)); }

  api::object_ptr<api::chatPosition> position(std::int64_t order, bool pinned = false, bool archive = false) {
    return api::make_object<api::chatPosition>(
        archive ? api::object_ptr<api::ChatList>(api::make_object<api::chatListArchive>())
                : api::object_ptr<api::ChatList>(api::make_object<api::chatListMain>()),
        order, pinned, nullptr);
  }

  void addChat(std::int64_t id, std::int64_t order, const char *title = "Chat") {
    auto chat = api::make_object<api::chat>();
    chat->id_ = id;
    chat->title_ = title;
    chat->type_ = api::make_object<api::chatTypePrivate>(id);
    chat->positions_.push_back(position(order));
    update(api::make_object<api::updateNewChat>(std::move(chat)));
  }
};

void ListFollowsOrder() {
  auto f = Fixture();
  f.addChat(1, 100);
  f.addChat(2, 300);
  f.addChat(3, 200);
  g_assert_true((f.store.list(ListKind::Main) == std::vector<Td::ChatId>{2, 3, 1}));
  f.update(api::make_object<api::updateChatPosition>(1, f.position(400)));
  g_assert_true((f.store.list(ListKind::Main) == std::vector<Td::ChatId>{1, 2, 3}));
}

void OrderZeroLeavesTheList() {
  auto f = Fixture();
  f.addChat(1, 100);
  f.addChat(2, 200);
  f.update(api::make_object<api::updateChatPosition>(2, f.position(0)));
  g_assert_true((f.store.list(ListKind::Main) == std::vector<Td::ChatId>{1}));
}

void ArchiveIsItsOwnList() {
  auto f = Fixture();
  f.addChat(1, 100);
  f.update(api::make_object<api::updateChatPosition>(1, f.position(0)));
  f.update(api::make_object<api::updateChatPosition>(1, f.position(50, false, true)));
  g_assert_true(f.store.list(ListKind::Main).empty());
  g_assert_true((f.store.list(ListKind::Archive) == std::vector<Td::ChatId>{1}));
}

void LastMessageMovesAndDescribes() {
  auto f = Fixture();
  f.addChat(1, 100);
  f.addChat(2, 200);
  auto message = api::make_object<api::message>();
  message->id_ = 77;
  message->date_ = 1000;
  message->is_outgoing_ = true;
  auto positions = std::vector<api::object_ptr<api::chatPosition>>();
  positions.push_back(f.position(500));
  f.update(api::make_object<api::updateChatLastMessage>(1, std::move(message), std::move(positions)));
  const auto chat = f.store.chat(1);
  g_assert_cmpstr(chat->preview.c_str(), ==, "message 77");
  g_assert_cmpint(chat->lastDate, ==, 1000);
  g_assert_true(chat->lastOutgoing);
  g_assert_cmpint(f.store.list(ListKind::Main).front(), ==, 1);
}

void AnEmptiedChatHasNoPreview() {
  auto f = Fixture();
  f.addChat(1, 100);
  auto message = api::make_object<api::message>();
  message->id_ = 77;
  f.update(api::make_object<api::updateChatLastMessage>(1, std::move(message),
                                                        std::vector<api::object_ptr<api::chatPosition>>()));
  f.update(api::make_object<api::updateChatLastMessage>(1, nullptr, std::vector<api::object_ptr<api::chatPosition>>()));
  g_assert_cmpstr(f.store.chat(1)->preview.c_str(), ==, "");
}

void UnreadFollowsUpdates() {
  auto f = Fixture();
  f.addChat(1, 100);
  f.update(api::make_object<api::updateChatReadInbox>(1, 50, 7));
  g_assert_cmpint(f.store.chat(1)->unread, ==, 7);
  g_assert_cmpint(f.store.chat(1)->readInbox, ==, 50);
}

void UpdatesForUnknownChatsAreIgnored() {
  auto f = Fixture();
  f.update(api::make_object<api::updateChatTitle>(99, "Nobody"));
  g_assert_null(f.store.chat(99));
}

void SubscribersEndWithTheirLifetime() {
  auto f = Fixture();
  auto seen = 0;
  auto lifetime = std::make_unique<Td::Lifetime>();
  f.store.onChat([&](Td::ChatId) { ++seen; }, *lifetime);
  f.addChat(1, 100);
  g_assert_cmpint(seen, ==, 1);
  lifetime = nullptr;
  f.update(api::make_object<api::updateChatTitle>(1, "New"));
  g_assert_cmpint(seen, ==, 1);
  g_assert_cmpstr(f.store.chat(1)->title.c_str(), ==, "New");
}

void LoadStopsAtTheEndOfTheList() {
  auto f = Fixture();
  f.store.loadMore(ListKind::Main);
  f.store.loadMore(ListKind::Main);  // Already running.
  g_assert_cmpuint(f.sent.size(), ==, 1);
  f.sink(f.transport->last, api::make_object<api::error>(404, "Not Found"));
  g_assert_true(f.store.listComplete(ListKind::Main));
  f.store.loadMore(ListKind::Main);
  g_assert_cmpuint(f.sent.size(), ==, 1);
}

void CompleteFilesAreNotAskedAgain() {
  auto f = Fixture();
  auto local = api::make_object<api::localFile>();
  local->is_downloading_completed_ = true;
  local->path_ = "/tmp/x";
  local->downloaded_size_ = 10;
  auto file = api::make_object<api::file>(5, 10, 10, std::move(local), nullptr);
  auto path = std::string();
  auto lifetime = Td::Lifetime();
  f.store.onFile([&](const Td::File &file) { path = file.path; }, lifetime);
  f.update(api::make_object<api::updateFile>(std::move(file)));
  g_assert_cmpstr(path.c_str(), ==, "/tmp/x");
  f.store.download(5);
  g_assert_true(f.sent.empty());
}

}  // namespace
}  // namespace Contrail

int main(int argc, char **argv) {
  using namespace Contrail;
  g_test_init(&argc, &argv, nullptr);
  g_test_add_func("/store/order", ListFollowsOrder);
  g_test_add_func("/store/order-zero", OrderZeroLeavesTheList);
  g_test_add_func("/store/archive", ArchiveIsItsOwnList);
  g_test_add_func("/store/last-message", LastMessageMovesAndDescribes);
  g_test_add_func("/store/emptied-chat", AnEmptiedChatHasNoPreview);
  g_test_add_func("/store/unread", UnreadFollowsUpdates);
  g_test_add_func("/store/unknown-chat", UpdatesForUnknownChatsAreIgnored);
  g_test_add_func("/store/subscribers", SubscribersEndWithTheirLifetime);
  g_test_add_func("/store/load-end", LoadStopsAtTheEndOfTheList);
  g_test_add_func("/store/complete-file", CompleteFilesAreNotAskedAgain);
  return g_test_run();
}
