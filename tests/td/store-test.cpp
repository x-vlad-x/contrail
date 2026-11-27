// The store on scripted updates: no network, no account.
#include "td/dates.hpp"
#include "td/store.hpp"

#include <glib.h>

#include <algorithm>

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
    _last = std::move(function);
  }
  [[nodiscard]] const api::chatNotificationSettings *lastSettings() const {
    if (!_last || _last->get_id() != api::setChatNotificationSettings::ID) return nullptr;
    return static_cast<const api::setChatNotificationSettings &>(*_last).notification_settings_.get();
  }
  std::uint64_t last = 0;

 private:
  Sink &_sink;
  std::vector<std::int32_t> &_sent;
  std::vector<std::uint64_t> _ids;
  Td::FunctionPtr _last;
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

void MuteKeepsTheOtherSettings() {
  auto f = Fixture();
  f.addChat(1, 100);
  f.store.setMuted(1, true);
  g_assert_cmpint(f.sent.back(), ==, api::getChat::ID);
  auto chat = api::make_object<api::chat>();
  chat->id_ = 1;
  chat->notification_settings_ = api::make_object<api::chatNotificationSettings>();
  chat->notification_settings_->use_default_mute_for_ = true;
  chat->notification_settings_->sound_id_ = 42;
  f.sink(f.transport->last, std::move(chat));
  g_assert_cmpint(f.sent.back(), ==, api::setChatNotificationSettings::ID);
  const auto settings = f.transport->lastSettings();
  g_assert_nonnull(settings);
  g_assert_false(settings->use_default_mute_for_);
  g_assert_cmpint(settings->mute_for_, >, 366 * 86400);
  g_assert_cmpint(settings->sound_id_, ==, 42);
}

void MarkReadClearsEverythingThatShows() {
  auto f = Fixture();
  f.addChat(1, 100);
  auto message = api::make_object<api::message>();
  message->id_ = 77;
  f.update(api::make_object<api::updateChatLastMessage>(1, std::move(message),
                                                        std::vector<api::object_ptr<api::chatPosition>>()));
  f.update(api::make_object<api::updateChatUnreadMentionCount>(1, 1));
  f.update(api::make_object<api::updateChatUnreadReactionCount>(1, 1));
  f.update(api::make_object<api::updateChatIsMarkedAsUnread>(1, true));
  f.store.markRead(1);
  g_assert_true((f.sent == std::vector<std::int32_t>{api::viewMessages::ID, api::readAllChatMentions::ID,
                                                     api::readAllChatReactions::ID,
                                                     api::toggleChatIsMarkedAsUnread::ID}));
}

// The heart in the list: a message TDLib holds tells the chat's count only in its own update.
void UnreadReactionsAndTheQuickOne() {
  auto f = Fixture();
  f.addChat(1, 100);
  f.update(api::make_object<api::updateChatUnreadReactionCount>(1, 2));
  g_assert_cmpint(f.store.chat(1)->unreadReactions, ==, 2);
  f.update(api::make_object<api::updateMessageUnreadReactions>(1, 77, std::vector<api::object_ptr<api::unreadReaction>>(), 0));
  g_assert_cmpint(f.store.chat(1)->unreadReactions, ==, 0);
  g_assert_null(f.store.defaultReaction());
  f.update(api::make_object<api::updateDefaultReactionType>(api::make_object<api::reactionTypeEmoji>("x")));
  const auto quick = f.store.defaultReaction();
  g_assert_nonnull(quick);
  g_assert_cmpint(quick->get_id(), ==, api::reactionTypeEmoji::ID);
}

void UnreadChatsReachTheList() {
  auto f = Fixture();
  auto told = std::vector<ListKind>();
  auto lifetime = Td::Lifetime();
  f.store.onList([&](ListKind kind) { told.push_back(kind); }, lifetime);
  f.update(api::make_object<api::updateUnreadChatCount>(api::make_object<api::chatListArchive>(), 9, 4, 0, 0, 0));
  g_assert_cmpint(f.store.unreadChats(ListKind::Archive), ==, 4);
  g_assert_true((told == std::vector<ListKind>{ListKind::Archive}));
}

void ListDates() {
  const auto now = Td::Now();
  g_assert_cmpstr(Td::FormatListDate(now).c_str(), ==, Td::FormatTime(now).c_str());
  // Three days back it is a weekday, a month back a date with digits.
  const auto weekday = Td::FormatListDate(now - 3 * 86400);
  g_assert_cmpuint(weekday.size(), ==, 3);
  g_assert_true(Td::FormatListDate(now - 30 * 86400).find_first_of("0123456789") != std::string::npos);
}


api::object_ptr<api::forumTopic> MakeTopic(std::int64_t chat, std::int32_t id, std::int64_t order, bool pinned = false) {
  auto topic = api::make_object<api::forumTopic>();
  topic->info_ = api::make_object<api::forumTopicInfo>();
  topic->info_->chat_id_ = chat;
  topic->info_->forum_topic_id_ = id;
  topic->info_->name_ = "Topic " + std::to_string(id);
  topic->info_->is_general_ = id == 1;
  topic->order_ = order;
  topic->is_pinned_ = pinned;
  topic->last_read_inbox_message_id_ = 100;
  topic->notification_settings_ = api::make_object<api::chatNotificationSettings>();
  topic->notification_settings_->use_default_mute_for_ = true;
  return topic;
}

void TopicsOfAGroupHeld() {
  auto f = Fixture();
  constexpr auto kChat = std::int64_t(-400);
  auto group = api::make_object<api::supergroup>();
  group->id_ = 400;
  group->is_forum_ = true;
  f.update(api::make_object<api::updateSupergroup>(std::move(group)));
  auto chat = api::make_object<api::chat>();
  chat->id_ = kChat;
  chat->title_ = "Club";
  chat->type_ = api::make_object<api::chatTypeSupergroup>(400, false);
  f.update(api::make_object<api::updateNewChat>(std::move(chat)));
  g_assert_true(f.store.chat(kChat)->forum);

  auto hold = f.store.holdTopics(kChat);
  f.store.loadTopics(kChat);
  g_assert_cmpint(f.sent.back(), ==, api::getForumTopics::ID);
  auto page = api::make_object<api::forumTopics>();
  page->topics_.push_back(MakeTopic(kChat, 1, 100));
  page->topics_.push_back(MakeTopic(kChat, 2, 300));
  page->topics_.push_back(MakeTopic(kChat, 3, 200, true));
  page->total_count_ = 3;
  f.sink(f.transport->last, std::move(page));
  g_assert_true(f.store.topicsComplete(kChat));
  // Pinned first, then by order.
  g_assert_true((f.store.topics(kChat) == std::vector<std::int32_t>{3, 2, 1}));

  // A new message in General: shown at once, the order comes with the topic asked for again.
  auto told = std::vector<std::int32_t>();
  auto lifetime = Td::Lifetime();
  f.store.onTopic([&](Td::ChatId, std::int32_t id) { told.push_back(id); }, lifetime);
  auto message = api::make_object<api::message>();
  message->id_ = 500;
  message->chat_id_ = kChat;
  message->topic_id_ = api::make_object<api::messageTopicForum>(1);
  f.update(api::make_object<api::updateNewMessage>(std::move(message)));
  g_assert_cmpstr(f.store.topic(kChat, 1)->row.preview.c_str(), ==, "message 500");
  g_assert_cmpint(f.store.topic(kChat, 1)->row.unread, ==, 1);
  g_assert_cmpint(f.store.topic(kChat, 2)->row.unread, ==, 0);
  g_assert_cmpint(f.sent.back(), ==, api::getForumTopic::ID);
  f.sink(f.transport->last, MakeTopic(kChat, 1, 400));
  g_assert_true((f.store.topics(kChat) == std::vector<std::int32_t>{3, 1, 2}));
  g_assert_true(std::ranges::count(told, 0) > 0);

  // Read in the topic: nothing left unread after the last message.
  f.update(api::make_object<api::updateForumTopic>(kChat, 2, false, 600, 0, 0, 0, 0, nullptr, nullptr));
  g_assert_cmpint(f.store.topic(kChat, 2)->row.readInbox, ==, 600);

  // No longer a forum; and topics nobody holds are forgotten.
  group = api::make_object<api::supergroup>();
  group->id_ = 400;
  f.update(api::make_object<api::updateSupergroup>(std::move(group)));
  g_assert_false(f.store.chat(kChat)->forum);
  hold = nullptr;
  g_assert_null(f.store.topic(kChat, 1));
  g_assert_true(f.store.topics(kChat).empty());
}

// Who may pin: anybody in a private chat, in a group the members if everybody may, otherwise by their own rights.
void PinRights() {
  auto f = Fixture();
  f.addChat(1, 10);
  g_assert_true(f.store.canPin(*f.store.chat(1)));

  constexpr auto kGroup = std::int64_t(-500);
  auto chat = api::make_object<api::chat>();
  chat->id_ = kGroup;
  chat->type_ = api::make_object<api::chatTypeSupergroup>(500, false);
  chat->permissions_ = api::make_object<api::chatPermissions>();
  f.update(api::make_object<api::updateNewChat>(std::move(chat)));
  const auto group = [&](api::object_ptr<api::ChatMemberStatus> status, bool channel = false) {
    auto supergroup = api::make_object<api::supergroup>();
    supergroup->id_ = 500;
    supergroup->is_channel_ = channel;
    supergroup->status_ = std::move(status);
    f.update(api::make_object<api::updateSupergroup>(std::move(supergroup)));
    return f.store.canPin(*f.store.chat(kGroup));
  };
  g_assert_false(group(api::make_object<api::chatMemberStatusMember>()));
  auto everybody = api::make_object<api::chatPermissions>();
  everybody->can_pin_messages_ = true;
  f.update(api::make_object<api::updateChatPermissions>(kGroup, std::move(everybody)));
  g_assert_true(group(api::make_object<api::chatMemberStatusMember>()));
  // Restricted: its own permissions say, whatever the others may.
  g_assert_false(group(api::make_object<api::chatMemberStatusRestricted>(true, 0, api::make_object<api::chatPermissions>())));
  auto rights = api::make_object<api::chatAdministratorRights>();
  rights->can_edit_messages_ = true;
  g_assert_false(group(api::make_object<api::chatMemberStatusAdministrator>(false, std::move(rights))));
  // In a channel pinning goes with editing.
  rights = api::make_object<api::chatAdministratorRights>();
  rights->can_edit_messages_ = true;
  g_assert_true(group(api::make_object<api::chatMemberStatusAdministrator>(false, std::move(rights)), true));
  g_assert_false(group(api::make_object<api::chatMemberStatusMember>(), true));
  g_assert_true(group(api::make_object<api::chatMemberStatusCreator>(false, true)));
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
  g_test_add_func("/store/mute", MuteKeepsTheOtherSettings);
  g_test_add_func("/store/mark-read", MarkReadClearsEverythingThatShows);
  g_test_add_func("/store/unread-chats", UnreadChatsReachTheList);
  g_test_add_func("/store/unread-reactions", UnreadReactionsAndTheQuickOne);
  g_test_add_func("/store/list-dates", ListDates);
  g_test_add_func("/store/topics", TopicsOfAGroupHeld);
  g_test_add_func("/store/pin-rights", PinRights);
  return g_test_run();
}
