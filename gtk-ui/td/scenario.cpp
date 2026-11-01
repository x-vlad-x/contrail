#include "td/scenario.hpp"

#include <glib.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace Contrail::Td {
namespace {

constexpr auto kSelfId = std::int64_t(1000);
constexpr auto kChatCount = 40;
constexpr auto kMessagesInChat = 120;
constexpr auto kNow = 1793500000;
// Message ids in TDLib are server ids shifted by 20 bits.
constexpr auto kIdStep = std::int64_t(1) << 20;

api::object_ptr<api::formattedText> Formatted(const std::string &text) {
  return api::make_object<api::formattedText>(text, std::vector<api::object_ptr<api::textEntity>>());
}

api::object_ptr<api::user> MakeUser(std::int64_t id, const std::string &first, const std::string &last) {
  auto user = api::make_object<api::user>();
  user->id_ = id;
  user->first_name_ = first;
  user->last_name_ = last;
  user->type_ = api::make_object<api::userTypeRegular>();
  user->status_ = api::make_object<api::userStatusOffline>(kNow - 3600);
  return user;
}

api::object_ptr<api::message> MakeMessage(std::int64_t chatId, std::int64_t id, std::int64_t senderId, int date,
                                          const std::string &text) {
  auto message = api::make_object<api::message>();
  message->id_ = id;
  message->chat_id_ = chatId;
  message->sender_id_ = api::make_object<api::messageSenderUser>(senderId);
  message->is_outgoing_ = senderId == kSelfId;
  message->date_ = date;
  auto content = api::make_object<api::messageText>();
  content->text_ = Formatted(text);
  message->content_ = std::move(content);
  return message;
}

std::string MessageText(int index) {
  switch (index % 5) {
    case 0: return "Message " + std::to_string(index);
    case 1: return "A longer message that has to wrap on the narrow screen of a phone, number " + std::to_string(index);
    case 2: return "With an emoji 😀 in the middle, number " + std::to_string(index);
    case 3: return "Первая строка\nВторая строка " + std::to_string(index);
    default: return "ok";
  }
}

class ScenarioTransport final : public Transport {
 public:
  ~ScenarioTransport() override {
    if (_source) g_source_remove(_source);
  }

  void start(Sink sink) override {
    _sink = std::move(sink);
    _source = g_idle_add_once(+[](gpointer data) {
      auto *self = static_cast<ScenarioTransport *>(data);
      self->_source = 0;
      self->authorization(api::make_object<api::authorizationStateWaitTdlibParameters>());
    }, this);
  }

  void send(std::uint64_t id, FunctionPtr function) override {
    switch (function->get_id()) {
      case api::setTdlibParameters::ID:
        answer(id, api::make_object<api::ok>());
        authorization(api::make_object<api::authorizationStateReady>());
        fill();
        break;
      case api::getAuthorizationState::ID:
        answer(id, api::make_object<api::authorizationStateReady>());
        break;
      case api::loadChats::ID:
        answer(id, api::make_object<api::error>(404, "Not Found"));
        break;
      case api::getChatHistory::ID:
        answer(id, history(static_cast<api::getChatHistory &>(*function)));
        break;
      case api::sendMessage::ID:
        answer(id, sent(static_cast<api::sendMessage &>(*function)));
        break;
      case api::getLanguagePackStrings::ID:
        answer(id, api::make_object<api::error>(400, "The scenario has no language pack"));
        break;
      case api::setOption::ID:
      case api::openChat::ID:
      case api::closeChat::ID:
      case api::viewMessages::ID:
      case api::setChatDraftMessage::ID:
      case api::downloadFile::ID:
        answer(id, api::make_object<api::ok>());
        break;
      default:
        answer(id, api::make_object<api::error>(400, "Not in the scenario"));
        break;
    }
  }

 private:
  void answer(std::uint64_t id, ObjectPtr object) { _sink(id, std::move(object)); }
  void update(api::object_ptr<api::Update> update) { _sink(0, std::move(update)); }
  void authorization(api::object_ptr<api::AuthorizationState> state) {
    update(api::make_object<api::updateAuthorizationState>(std::move(state)));
  }

  void fill() {
    update(api::make_object<api::updateOption>("my_id", api::make_object<api::optionValueInteger>(kSelfId)));
    update(api::make_object<api::updateUser>(MakeUser(kSelfId, "Vlad", "")));
    for (auto i = 1; i <= kChatCount; ++i) {
      const auto id = std::int64_t(i);
      update(api::make_object<api::updateUser>(MakeUser(id, "Friend", std::to_string(i))));
      auto chat = api::make_object<api::chat>();
      chat->id_ = id;
      chat->type_ = api::make_object<api::chatTypePrivate>(id);
      chat->title_ = "Friend " + std::to_string(i);
      chat->unread_count_ = i % 4 == 0 ? i : 0;
      chat->notification_settings_ = api::make_object<api::chatNotificationSettings>();
      chat->last_message_ = messageAt(id, kMessagesInChat);
      chat->positions_.push_back(api::make_object<api::chatPosition>(
          api::make_object<api::chatListMain>(), std::int64_t(kChatCount - i + 1) << 32, i == 1, nullptr));
      update(api::make_object<api::updateNewChat>(std::move(chat)));
    }
  }

  api::object_ptr<api::message> messageAt(std::int64_t chatId, int index) {
    const auto sender = index % 3 == 0 ? kSelfId : chatId;
    return MakeMessage(chatId, index * kIdStep, sender, kNow - (kMessagesInChat - index) * 600, MessageText(index));
  }

  ObjectPtr history(api::getChatHistory &request) {
    auto result = api::make_object<api::messages>();
    const auto chatId = request.chat_id_;
    auto newest = request.from_message_id_ ? int(request.from_message_id_ / kIdStep) : kMessagesInChat + 1;
    // from_message_id itself is included only with a negative offset, as in TDLib.
    newest -= request.offset_;
    if (request.offset_ == 0) --newest;
    if (newest > kMessagesInChat + _sentExtra[chatId]) newest = kMessagesInChat + _sentExtra[chatId];
    for (auto index = newest; index >= 1 && int(result->messages_.size()) < request.limit_; --index) {
      result->messages_.push_back(messageAt(chatId, index));
    }
    result->total_count_ = kMessagesInChat;
    return result;
  }

  ObjectPtr sent(api::sendMessage &request) {
    const auto chatId = request.chat_id_;
    auto text = std::string();
    if (request.input_message_content_
        && request.input_message_content_->get_id() == api::inputMessageText::ID) {
      text = static_cast<api::inputMessageText &>(*request.input_message_content_).text_->text_;
    }
    const auto index = kMessagesInChat + ++_sentExtra[chatId];
    const auto temporary = index * kIdStep + 1;
    auto pending = MakeMessage(chatId, temporary, kSelfId, kNow, text);
    pending->sending_state_ = api::make_object<api::messageSendingStatePending>(0);
    update(api::make_object<api::updateNewMessage>(MakeMessage(chatId, temporary, kSelfId, kNow, text)));
    update(api::make_object<api::updateMessageSendSucceeded>(MakeMessage(chatId, index * kIdStep, kSelfId, kNow, text),
                                                             temporary));
    update(api::make_object<api::updateChatLastMessage>(chatId, MakeMessage(chatId, index * kIdStep, kSelfId, kNow, text),
                                                        std::vector<api::object_ptr<api::chatPosition>>()));
    return pending;
  }

  Sink _sink;
  guint _source = 0;
  std::map<std::int64_t, int> _sentExtra;
};

}  // namespace

std::unique_ptr<Transport> MakeScenarioTransport() {
  return std::make_unique<ScenarioTransport>();
}

}  // namespace Contrail::Td
