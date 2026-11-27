// Texts of messages, on made-up messages: no network, no account. The list below must follow TDLib's schema: a new
// type of content fails /message-text/schema until it is in the list and in the table of message_text.cpp.
#include "td/message_text.hpp"

#include <glib.h>

#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <string>

#define CONTENT_TYPES(X) \
  X(messageText) \
  X(messageRichMessage) \
  X(messageAnimation) \
  X(messageAudio) \
  X(messageDocument) \
  X(messagePaidMedia) \
  X(messagePhoto) \
  X(messageSticker) \
  X(messageVideo) \
  X(messageVideoNote) \
  X(messageVoiceNote) \
  X(messageExpiredPhoto) \
  X(messageExpiredVideo) \
  X(messageExpiredVideoNote) \
  X(messageExpiredVoiceNote) \
  X(messageLiveLocation) \
  X(messageLocation) \
  X(messageVenue) \
  X(messageContact) \
  X(messageAnimatedEmoji) \
  X(messageDice) \
  X(messageGame) \
  X(messagePoll) \
  X(messageStakeDice) \
  X(messageStory) \
  X(messageChecklist) \
  X(messageInvoice) \
  X(messageCall) \
  X(messageGroupCall) \
  X(messageVideoChatScheduled) \
  X(messageVideoChatStarted) \
  X(messageVideoChatEnded) \
  X(messageInviteVideoChatParticipants) \
  X(messagePollOptionAdded) \
  X(messagePollOptionDeleted) \
  X(messageBasicGroupChatCreate) \
  X(messageSupergroupChatCreate) \
  X(messageChatChangeTitle) \
  X(messageChatChangePhoto) \
  X(messageChatDeletePhoto) \
  X(messageChatOwnerLeft) \
  X(messageChatOwnerChanged) \
  X(messageChatHasProtectedContentToggled) \
  X(messageChatHasProtectedContentDisableRequested) \
  X(messageChatAddMembers) \
  X(messageChatJoinByLink) \
  X(messageChatJoinByRequest) \
  X(messageChatJoinFromCommunity) \
  X(messageChatDeleteMember) \
  X(messageChatAddedToCommunity) \
  X(messageChatRemovedFromCommunity) \
  X(messageChatUpgradeTo) \
  X(messageChatUpgradeFrom) \
  X(messagePinMessage) \
  X(messageScreenshotTaken) \
  X(messageChatSetBackground) \
  X(messageChatSetTheme) \
  X(messageChatSetMessageAutoDeleteTime) \
  X(messageChatBoost) \
  X(messageForumTopicCreated) \
  X(messageForumTopicEdited) \
  X(messageForumTopicIsClosedToggled) \
  X(messageForumTopicIsHiddenToggled) \
  X(messageSuggestProfilePhoto) \
  X(messageSuggestBirthdate) \
  X(messageCustomServiceAction) \
  X(messageGameScore) \
  X(messageManagedBotCreated) \
  X(messagePaymentSuccessful) \
  X(messagePaymentSuccessfulBot) \
  X(messagePaymentRefunded) \
  X(messageGiftedPremium) \
  X(messagePremiumGiftCode) \
  X(messageGiveawayCreated) \
  X(messageGiveaway) \
  X(messageGiveawayCompleted) \
  X(messageGiveawayWinners) \
  X(messageGiftedStars) \
  X(messageGiftedGrams) \
  X(messageGiveawayPrizeStars) \
  X(messageTonWalletTransfer) \
  X(messageTonConnectRequest) \
  X(messageGift) \
  X(messageUpgradedGift) \
  X(messageRefundedUpgradedGift) \
  X(messageUpgradedGiftPurchaseOffer) \
  X(messageUpgradedGiftPurchaseOfferRejected) \
  X(messagePaidMessagesRefunded) \
  X(messagePaidMessagePriceChanged) \
  X(messageDirectMessagePriceChanged) \
  X(messageUnpaidMessagesAllowedByLink) \
  X(messageChecklistTasksDone) \
  X(messageChecklistTasksAdded) \
  X(messageSuggestedPostApprovalFailed) \
  X(messageSuggestedPostApproved) \
  X(messageSuggestedPostDeclined) \
  X(messageSuggestedPostPaid) \
  X(messageSuggestedPostRefunded) \
  X(messageContactRegistered) \
  X(messageUsersShared) \
  X(messageChatShared) \
  X(messageBotWriteAccessAllowed) \
  X(messageWebAppDataSent) \
  X(messageWebAppDataReceived) \
  X(messagePassportDataSent) \
  X(messagePassportDataReceived) \
  X(messageProximityAlertTriggered) \
  X(messageUnsupported)

// g_assert_cmpstr keeps the pointer it is given: a temporary string would be gone by then.
#define CHECK_TEXT(actual, expected)                \
  do {                                              \
    const auto checked = std::string(actual);       \
    g_assert_cmpstr(checked.c_str(), ==, expected); \
  } while (false)

namespace Contrail {
namespace {

namespace api = Td::api;
using Td::ContentKind;

constexpr auto kSelf = std::int64_t(1);
constexpr auto kAnna = std::int64_t(2);
constexpr auto kBoris = std::int64_t(3);
constexpr auto kClara = std::int64_t(4);
constexpr auto kPrivate = std::int64_t(2);
constexpr auto kGroup = std::int64_t(-100);
constexpr auto kChannel = std::int64_t(-200);
constexpr auto kSaved = std::int64_t(1);

class SilentTransport final : public Td::Transport {
 public:
  explicit SilentTransport(Sink &sink) : _sink(sink) {}
  void start(Sink sink) override { _sink = std::move(sink); }
  void send(std::uint64_t id, Td::FunctionPtr function) override {}

 private:
  Sink &_sink;
};

struct Fixture {
  Td::Transport::Sink sink;
  Td::Client client;
  Td::Store store;

  Fixture()
      : client(std::make_unique<SilentTransport>(sink)),
        store(client, [](const api::message &) { return std::string(); }) {
    sink(0, api::make_object<api::updateOption>("my_id", api::make_object<api::optionValueInteger>(kSelf)));
    addUser(kSelf, "Me", "");
    addUser(kAnna, "Anna", "Smith");
    addUser(kBoris, "Boris", "");
    addUser(kClara, "Clara", "");
    addChat(kPrivate, "Anna Smith", api::make_object<api::chatTypePrivate>(kAnna));
    addChat(kSaved, "Saved", api::make_object<api::chatTypePrivate>(kSelf));
    addChat(kGroup, "Friends", api::make_object<api::chatTypeSupergroup>(100, false));
    addChat(kChannel, "News", api::make_object<api::chatTypeSupergroup>(200, true));
  }

  void addUser(std::int64_t id, const char *first, const char *last) {
    auto user = api::make_object<api::user>();
    user->id_ = id;
    user->first_name_ = first;
    user->last_name_ = last;
    sink(0, api::make_object<api::updateUser>(std::move(user)));
  }

  void addChat(std::int64_t id, const char *title, api::object_ptr<api::ChatType> type) {
    auto chat = api::make_object<api::chat>();
    chat->id_ = id;
    chat->title_ = title;
    chat->type_ = std::move(type);
    sink(0, api::make_object<api::updateNewChat>(std::move(chat)));
  }
};

[[nodiscard]] api::object_ptr<api::MessageSender> User(std::int64_t id) {
  return api::make_object<api::messageSenderUser>(id);
}

[[nodiscard]] api::object_ptr<api::formattedText> Formatted(const char *text) {
  return api::make_object<api::formattedText>(text, std::vector<api::object_ptr<api::textEntity>>());
}

[[nodiscard]] api::object_ptr<api::message> Message(api::object_ptr<api::MessageContent> content, std::int64_t chat,
                                                    std::int64_t from, bool outgoing = false) {
  auto message = api::make_object<api::message>();
  message->id_ = 10;
  message->chat_id_ = chat;
  if (from) message->sender_id_ = User(from);
  message->is_outgoing_ = outgoing;
  message->content_ = std::move(content);
  return message;
}

[[nodiscard]] api::object_ptr<api::message> Text(const char *text) {
  return Message(api::make_object<api::messageText>(Formatted(text), nullptr, nullptr), kGroup, kAnna);
}

template <typename Check>
void ForEachContent(Check check) {
#define X(type) check(#type, api::object_ptr<api::MessageContent>(api::make_object<api::type>()));
  CONTENT_TYPES(X)
#undef X
}

[[nodiscard]] std::set<std::string> SchemaTypes() {
  auto file = std::ifstream(TD_API_TL);
  g_assert_true(file.good());
  auto result = std::set<std::string>();
  const auto line = std::regex(R"(^(message[A-Za-z]*)\b.* = MessageContent;$)");
  for (auto text = std::string(); std::getline(file, text);) {
    if (text == "---functions---") break;
    if (auto match = std::smatch(); std::regex_match(text, match, line)) result.insert(match[1]);
  }
  return result;
}

void EveryTypeOfTheSchemaIsCovered() {
  auto listed = std::map<std::string, ContentKind>();
  ForEachContent([&](const char *name, api::object_ptr<api::MessageContent> content) {
    listed[name] = Td::ClassifyContent(*content);
  });
  const auto schema = SchemaTypes();
  g_assert_false(schema.empty());
  auto counts = std::map<ContentKind, int>();
  for (const auto &name : schema) {
    const auto found = listed.find(name);
    if (found == listed.end()) g_error("%s is in the schema but not in CONTENT_TYPES", name.c_str());
    if (found->second == ContentKind::Unknown) g_error("%s is missing from the table of message_text.cpp", name.c_str());
    ++counts[found->second];
  }
  for (const auto &[name, kind] : listed) {
    if (!schema.count(name)) g_error("%s is not in the schema any more", name.c_str());
  }
  g_print("# %d types: %d shown, %d service, %d fallback\n", int(schema.size()), counts[ContentKind::Shown],
          counts[ContentKind::Service], counts[ContentKind::Fallback]);
}

// Every field of a made-up object is empty or null, like the fields TDLib may leave out, from every kind of sender.
void EmptyFieldsAreSafe() {
  auto f = Fixture();
  ForEachContent([&](const char *name, api::object_ptr<api::MessageContent> content) {
    const auto service = Td::IsServiceMessage(*content);
    auto message = api::make_object<api::message>();
    message->content_ = std::move(content);
    const auto check = [&] {
      const auto text = Td::ServiceText(*message, f.store);
      const auto preview = Td::MessagePreview(*message, f.store);
      (void)Td::SenderPrefix(*message, f.store);
      if (!service) CHECK_TEXT(text, "");
      for (const auto &line : {text, preview}) {
        if (line.find("lng_") != std::string::npos) g_error("%s: no such string: %s", name, line.c_str());
        if (line.find('{') != std::string::npos) g_error("%s: left unfilled: %s", name, line.c_str());
      }
    };
    for (const auto chat : {std::int64_t(0), kPrivate, kSaved, kGroup, kChannel, std::int64_t(999)}) {
      message->chat_id_ = chat;
      for (const auto outgoing : {false, true}) {
        message->is_outgoing_ = outgoing;
        message->is_channel_post_ = (chat == kChannel);
        message->sender_id_ = nullptr;
        check();
        message->sender_id_ = User(kAnna);
        check();
        message->sender_id_ = User(999);
        check();
        message->sender_id_ = api::make_object<api::messageSenderChat>(chat);
        check();
      }
    }
    message->content_ = nullptr;
    check();
  });
  auto message = api::make_object<api::message>();
  auto lone = Td::MessagePreview(*message, f.store);
  CHECK_TEXT(lone, "");
}

void TextIsOneLine() {
  auto f = Fixture();
  const auto preview = Td::MessagePreview(*Text("  first\n\n  second\ttext "), f.store);
  CHECK_TEXT(preview, "first second text");
  const auto text = std::string(5000, 'a');
  const auto longer = Td::MessagePreview(*Text(text.c_str()), f.store);
  g_assert_cmpuint(longer.size(), <, 300);
}

void SpoilersStayHidden() {
  auto f = Fixture();
  auto entities = std::vector<api::object_ptr<api::textEntity>>();
  // UTF-16: the emoji takes two units.
  entities.push_back(api::make_object<api::textEntity>(7, 6, api::make_object<api::textEntityTypeSpoiler>()));
  auto text = api::make_object<api::formattedText>("\xF0\x9F\x98\x80 the secret end", std::move(entities));
  auto message = Message(api::make_object<api::messageText>(std::move(text), nullptr, nullptr), kPrivate, kAnna);
  const auto preview = Td::MessagePreview(*message, f.store);
  CHECK_TEXT(preview, "\xF0\x9F\x98\x80 the \xE2\x96\x92 end");
}

void MediaGoWithTheirCaption() {
  auto f = Fixture();
  const auto preview = [&](api::object_ptr<api::MessageContent> content) {
    return Td::MessagePreview(*Message(std::move(content), kPrivate, kAnna), f.store);
  };
  auto photo = api::make_object<api::messagePhoto>();
  CHECK_TEXT(preview(std::move(photo)), "Photo");
  photo = api::make_object<api::messagePhoto>();
  photo->caption_ = Formatted("at the sea");
  CHECK_TEXT(preview(std::move(photo)), "Photo, at the sea");
  photo = api::make_object<api::messagePhoto>();
  photo->caption_ = Formatted("once");
  photo->is_secret_ = true;
  CHECK_TEXT(preview(std::move(photo)), "Self-Destructing Photo");
  auto document = api::make_object<api::messageDocument>();
  document->document_ = api::make_object<api::document>();
  document->document_->file_name_ = "report.pdf";
  CHECK_TEXT(preview(std::move(document)), "report.pdf");
  auto audio = api::make_object<api::messageAudio>();
  audio->audio_ = api::make_object<api::audio>();
  audio->audio_->title_ = "Song";
  audio->audio_->performer_ = "Band";
  CHECK_TEXT(preview(std::move(audio)), "Band \xE2\x80\x93 Song");
  auto call = api::make_object<api::messageCall>();
  call->duration_ = 75;
  CHECK_TEXT(preview(std::move(call)), "Incoming call (1 min 15 s)");
  auto missed = api::make_object<api::messageCall>();
  missed->discard_reason_ = api::make_object<api::callDiscardReasonMissed>();
  CHECK_TEXT(preview(std::move(missed)), "Missed call");
}

void ServiceLinesNameThePeople() {
  auto f = Fixture();
  const auto text = [&](api::object_ptr<api::MessageContent> content, std::int64_t from, std::int64_t chat = kGroup) {
    return Td::ServiceText(*Message(std::move(content), chat, from, from == kSelf), f.store);
  };
  using Ids = std::vector<std::int64_t>;
  CHECK_TEXT(text(api::make_object<api::messageChatAddMembers>(Ids{kBoris}), kAnna), "Anna Smith added Boris");
  CHECK_TEXT(text(api::make_object<api::messageChatAddMembers>(Ids{kAnna}), kAnna), "Anna Smith joined the group");
  CHECK_TEXT(text(api::make_object<api::messageChatAddMembers>(Ids{kBoris, kClara, kAnna}), kSelf), "Me added Boris, Clara and Anna Smith");
  CHECK_TEXT(text(api::make_object<api::messageChatDeleteMember>(kAnna), kAnna), "Anna Smith left the group");
  CHECK_TEXT(text(api::make_object<api::messageChatDeleteMember>(kBoris), kAnna), "Anna Smith removed Boris");
  CHECK_TEXT(text(api::make_object<api::messageChatDeleteMember>(555), kAnna), "Anna Smith removed Deleted Account");
  CHECK_TEXT(text(api::make_object<api::messageChatSetMessageAutoDeleteTime>(86400 * 2, 0), kSelf), "You set messages to auto-delete in 2 days");
  CHECK_TEXT(text(api::make_object<api::messageVideoChatEnded>(7200), kAnna), "Anna Smith ended the video chat (2 hours)");
  CHECK_TEXT(text(api::make_object<api::messageVideoChatEnded>(90), kAnna, kChannel), "Live stream finished (90 seconds)");
  CHECK_TEXT(text(api::make_object<api::messageScreenshotTaken>(), kSelf, kPrivate), "You took a screenshot!");
  CHECK_TEXT(text(api::make_object<api::messageChatChangeTitle>("Crew"), kAnna), "Anna Smith changed group name to \xC2\xAB" "Crew\xC2\xBB");
  CHECK_TEXT(text(api::make_object<api::messageContactRegistered>(), kAnna, kPrivate), "Anna Smith joined Telegram");
  CHECK_TEXT(text(api::make_object<api::messageChatUpgradeFrom>("Old", 5), kAnna), "");
}

void MoneyHasItsUnits() {
  auto f = Fixture();
  const auto text = [&](api::object_ptr<api::MessageContent> content) {
    return Td::ServiceText(*Message(std::move(content), kPrivate, kSelf, true), f.store);
  };
  auto paid = api::make_object<api::messagePaymentSuccessful>();
  paid->currency_ = "USD";
  paid->total_amount_ = 1250;
  CHECK_TEXT(text(std::move(paid)), "You successfully transferred $12.50 to Anna Smith");
  paid = api::make_object<api::messagePaymentSuccessful>();
  paid->currency_ = "JPY";
  paid->total_amount_ = 500;
  paid->invoice_name_ = "Tea";
  CHECK_TEXT(text(std::move(paid)), "You successfully transferred \xC2\xA5" "500 to Anna Smith for Tea");
  paid = api::make_object<api::messagePaymentSuccessful>();
  paid->currency_ = "XTR";
  paid->total_amount_ = 7;
  CHECK_TEXT(text(std::move(paid)), "You successfully transferred 7 Stars to Anna Smith");
  auto grams = api::make_object<api::messageGiftedGrams>();
  grams->gram_amount_ = 1'500'000'000;
  CHECK_TEXT(text(std::move(grams)), "You sent a gift for 1.5 Grams");
}

void APinQuotesWhatItPinned() {
  auto f = Fixture();
  auto pin = Message(api::make_object<api::messagePinMessage>(5), kGroup, kAnna);
  auto service = Td::ServiceText(*pin, f.store);
  CHECK_TEXT(service, "Anna Smith pinned a message");
  auto pinned = Text("short one");
  pinned->id_ = 5;
  service = Td::ServiceText(*pin, f.store, pinned.get());
  CHECK_TEXT(service, "Anna Smith pinned \"short one\"");
  pinned = Text("a very long pinned message goes here");
  pinned->id_ = 5;
  service = Td::ServiceText(*pin, f.store, pinned.get());
  CHECK_TEXT(service, "Anna Smith pinned \"a very long pinn\xE2\x80\xA6\"");
  auto media = Message(api::make_object<api::messagePhoto>(), kGroup, kAnna);
  media->id_ = 5;
  service = Td::ServiceText(*pin, f.store, media.get());
  CHECK_TEXT(service, "Anna Smith pinned a photo");
  auto gone = Message(api::make_object<api::messagePinMessage>(0), kGroup, kAnna);
  service = Td::ServiceText(*gone, f.store);
  CHECK_TEXT(service, "Anna Smith pinned Deleted message");
}

void AStoryMentionIsAServiceLine() {
  auto f = Fixture();
  auto story = api::make_object<api::messageStory>();
  g_assert_false(Td::IsServiceMessage(*story));
  story->via_mention_ = true;
  g_assert_true(Td::IsServiceMessage(*story));
  const auto text = Td::ServiceText(*Message(std::move(story), kPrivate, kAnna), f.store);
  CHECK_TEXT(text, "Anna mentioned you in a story");
}

void PrefixFollowsTheChat() {
  auto f = Fixture();
  const auto prefix = [&](std::int64_t chat, std::int64_t from) {
    auto content = api::make_object<api::messageText>(Formatted("hi"), nullptr, nullptr);
    return Td::SenderPrefix(*Message(std::move(content), chat, from, from == kSelf), f.store);
  };
  CHECK_TEXT(prefix(kGroup, kAnna), "Anna");
  CHECK_TEXT(prefix(kGroup, kSelf), "You");
  CHECK_TEXT(prefix(kPrivate, kAnna), "");
  CHECK_TEXT(prefix(kChannel, kAnna), "");
  CHECK_TEXT(prefix(999, kAnna), "");
  auto joined = Message(api::make_object<api::messageChatJoinByLink>(), kGroup, kAnna);
  CHECK_TEXT(Td::SenderPrefix(*joined, f.store), "");
  auto saved = Message(api::make_object<api::messageText>(Formatted("hi"), nullptr, nullptr), kSaved, kSelf);
  saved->forward_info_ = api::make_object<api::messageForwardInfo>();
  saved->forward_info_->origin_ = api::make_object<api::messageOriginUser>(kBoris);
  CHECK_TEXT(Td::SenderPrefix(*saved, f.store), "Boris");
}

void TodayIsToday() {
  auto f = Fixture();
  const auto now = g_get_real_time() / G_USEC_PER_SEC;
  const auto text = Td::ServiceText(*Message(api::make_object<api::messageVideoChatScheduled>(1, now), kGroup, kAnna),
                                    f.store);
  g_assert_true(g_str_has_prefix(text.c_str(), "Anna Smith scheduled a video chat for today at "));
}

}  // namespace
}  // namespace Contrail

int main(int argc, char **argv) {
  using namespace Contrail;
  g_test_init(&argc, &argv, nullptr);
  g_test_add_func("/message-text/schema", EveryTypeOfTheSchemaIsCovered);
  g_test_add_func("/message-text/empty-fields", EmptyFieldsAreSafe);
  g_test_add_func("/message-text/one-line", TextIsOneLine);
  g_test_add_func("/message-text/spoilers", SpoilersStayHidden);
  g_test_add_func("/message-text/media", MediaGoWithTheirCaption);
  g_test_add_func("/message-text/service", ServiceLinesNameThePeople);
  g_test_add_func("/message-text/money", MoneyHasItsUnits);
  g_test_add_func("/message-text/pin", APinQuotesWhatItPinned);
  g_test_add_func("/message-text/story-mention", AStoryMentionIsAServiceLine);
  g_test_add_func("/message-text/prefix", PrefixFollowsTheChat);
  g_test_add_func("/message-text/today", TodayIsToday);
  return g_test_run();
}
