#include "td/scenario.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Contrail::Td {
namespace {

constexpr auto kSelfId = std::int64_t(1000);
constexpr auto kChatCount = 40;
constexpr auto kNow = 1793500000;
// CONTRAIL_SCENARIO=list: everything the chat list can show, and a long account that loads page by page.
constexpr auto kLoadedChats = 2000;
constexpr auto kLoadedChatIds = std::int64_t(100000);
constexpr auto kArchivedChats = 6;
constexpr auto kArchivedChatIds = std::int64_t(5000);
constexpr auto kGroupId = std::int64_t(-100);
constexpr auto kChannelId = std::int64_t(-200);
constexpr auto kPublicChatId = std::int64_t(-300);
constexpr auto kPinnedOrder = std::int64_t(1) << 62;
constexpr auto kNewMessageEveryMs = 5000;
// Message ids in TDLib are server ids shifted by 20 bits.
constexpr auto kIdStep = std::int64_t(1) << 20;

// CONTRAIL_SCENARIO=3000 gives a chat that long: enough to walk away from what is held.
int MessagesInChat() {
  static const auto result = [] {
    const auto value = g_getenv("CONTRAIL_SCENARIO");
    return value ? std::max(std::atoi(value), 120) : 120;
  }();
  return result;
}

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

// CONTRAIL_SCENARIO_PHOTOS=1: every fourth message is a photo, as large as Telegram sends them, to measure
// what pictures cost (tools/mem-scenario.sh). Eight pictures in ~/.cache/contrail-scenario, made once; every
// message still has file ids of its own, so each one is decoded as a different photo would be.
constexpr auto kScenarioPictures = 8;

bool WithPhotos() {
  static const auto result = g_getenv("CONTRAIL_SCENARIO_PHOTOS") != nullptr;
  return result;
}

std::string PhotoFile(int picture, int width, int height) {
  auto *name = g_strdup_printf("%d-%dx%d.jpg", picture, width, height);
  auto *path = g_build_filename(g_get_user_cache_dir(), "contrail-scenario", name, nullptr);
  auto result = std::string(path);
  g_free(path);
  g_free(name);
  if (g_file_test(result.c_str(), G_FILE_TEST_EXISTS)) return result;
  auto *directory = g_path_get_dirname(result.c_str());
  g_mkdir_with_parents(directory, 0700);
  g_free(directory);
  auto *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);
  if (!pixbuf) {
    g_warning("Scenario: no memory for a %dx%d picture", width, height);
    return result;
  }
  auto *pixels = gdk_pixbuf_get_pixels(pixbuf);
  const auto stride = gdk_pixbuf_get_rowstride(pixbuf);
  for (auto y = 0; y < height; ++y) {
    for (auto x = 0; x < width; ++x) {
      auto *pixel = pixels + y * stride + x * 3;
      pixel[0] = guchar(x * 255 / width);
      pixel[1] = guchar(y * 255 / height);
      pixel[2] = guchar((picture * 40 + (x ^ y)) & 0xff);  // Some detail, so the JPEG is not tiny.
    }
  }
  gchar *buffer = nullptr;
  gsize length = 0;
  GError *error = nullptr;
  // Written whole or not at all: two scenarios may start at once, and one may be killed while writing.
  if (!gdk_pixbuf_save_to_buffer(pixbuf, &buffer, &length, "jpeg", &error, "quality", "85", nullptr)
      || !g_file_set_contents(result.c_str(), buffer, gssize(length), &error)) {
    g_warning("Scenario: picture %s not made: %s", result.c_str(), error ? error->message : "?");
    g_clear_error(&error);
  }
  g_free(buffer);
  g_object_unref(pixbuf);
  return result;
}

// Made before anything is shown, so that measurements don't include encoding them.
void MakePhotoFiles() {
  for (auto picture = 0; picture < kScenarioPictures; ++picture) {
    PhotoFile(picture, 320, 240);
    PhotoFile(picture, 2560, 1920);
  }
}

api::object_ptr<api::photoSize> PhotoSize(int index, const char *type, int width, int height) {
  const auto path = PhotoFile(index % kScenarioPictures, width, height);
  GStatBuf info;
  const auto length = g_stat(path.c_str(), &info) == 0 ? std::int64_t(info.st_size) : std::int64_t(0);
  auto local = api::make_object<api::localFile>();
  local->path_ = path;
  local->is_downloading_completed_ = length > 0;
  local->downloaded_size_ = length;
  auto file = api::make_object<api::file>();
  file->id_ = 1000000 + index * 10 + (width > 1000 ? 1 : 0);
  file->size_ = std::int64_t(length);
  file->local_ = std::move(local);
  file->remote_ = api::make_object<api::remoteFile>();
  auto size = api::make_object<api::photoSize>();
  size->type_ = type;
  size->photo_ = std::move(file);
  size->width_ = width;
  size->height_ = height;
  return size;
}

api::object_ptr<api::MessageContent> PhotoContent(int index) {
  auto photo = api::make_object<api::photo>();
  photo->sizes_.push_back(PhotoSize(index, "m", 320, 240));
  photo->sizes_.push_back(PhotoSize(index, "w", 2560, 1920));
  auto content = api::make_object<api::messagePhoto>();
  content->photo_ = std::move(photo);
  content->caption_ = Formatted(std::string());
  return content;
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

api::object_ptr<api::ChatList> ListObject(bool archive) {
  if (archive) return api::make_object<api::chatListArchive>();
  return api::make_object<api::chatListMain>();
}

class ScenarioTransport final : public Transport {
 public:
  ScenarioTransport() : _fullList(g_strcmp0(g_getenv("CONTRAIL_SCENARIO"), "list") == 0) {}

  ~ScenarioTransport() override {
    if (_source) g_source_remove(_source);
    if (_ticker) g_source_remove(_ticker);
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
        answer(id, loadChats(static_cast<api::loadChats &>(*function)));
        break;
      case api::getChat::ID:
        answer(id, makeChat(static_cast<api::getChat &>(*function).chat_id_));
        break;
      case api::createPrivateChat::ID:
        answer(id, makeChat(static_cast<api::createPrivateChat &>(*function).user_id_));
        break;
      case api::searchChatsOnServer::ID:
        answer(id, api::make_object<api::chats>(0, std::vector<std::int64_t>()));
        break;
      case api::searchPublicChats::ID:
        answer(id, publicChats(static_cast<api::searchPublicChats &>(*function)));
        break;
      case api::toggleChatIsPinned::ID:
        pin(static_cast<api::toggleChatIsPinned &>(*function));
        answer(id, api::make_object<api::ok>());
        break;
      case api::setChatNotificationSettings::ID: {
        auto &request = static_cast<api::setChatNotificationSettings &>(*function);
        update(api::make_object<api::updateChatNotificationSettings>(request.chat_id_,
                                                                     std::move(request.notification_settings_)));
        answer(id, api::make_object<api::ok>());
        break;
      }
      case api::addChatToList::ID:
        move(static_cast<api::addChatToList &>(*function));
        answer(id, api::make_object<api::ok>());
        break;
      case api::deleteChatHistory::ID:
        remove(static_cast<api::deleteChatHistory &>(*function).chat_id_);
        answer(id, api::make_object<api::ok>());
        break;
      case api::leaveChat::ID:
        remove(static_cast<api::leaveChat &>(*function).chat_id_);
        answer(id, api::make_object<api::ok>());
        break;
      case api::readAllChatMentions::ID:
        update(api::make_object<api::updateChatUnreadMentionCount>(
            static_cast<api::readAllChatMentions &>(*function).chat_id_, 0));
        answer(id, api::make_object<api::ok>());
        break;
      case api::readAllChatReactions::ID:
        update(api::make_object<api::updateChatUnreadReactionCount>(
            static_cast<api::readAllChatReactions &>(*function).chat_id_, 0));
        answer(id, api::make_object<api::ok>());
        break;
      case api::toggleChatIsMarkedAsUnread::ID: {
        auto &request = static_cast<api::toggleChatIsMarkedAsUnread &>(*function);
        update(api::make_object<api::updateChatIsMarkedAsUnread>(request.chat_id_, request.is_marked_as_unread_));
        answer(id, api::make_object<api::ok>());
        break;
      }
      case api::viewMessages::ID: {
        auto &request = static_cast<api::viewMessages &>(*function);
        if (_fullList && !request.message_ids_.empty()) {
          update(api::make_object<api::updateChatReadInbox>(request.chat_id_, request.message_ids_.back(), 0));
        }
        answer(id, api::make_object<api::ok>());
        break;
      }
      case api::getChatHistory::ID:
        answer(id, history(static_cast<api::getChatHistory &>(*function)));
        break;
      case api::sendMessage::ID:
        answer(id, sent(static_cast<api::sendMessage &>(*function)));
        break;
      case api::getChatMessageByDate::ID: {
        const auto &request = static_cast<api::getChatMessageByDate &>(*function);
        const auto index = MessagesInChat() - (kNow - request.date_) / 600;
        if (index < 1) answer(id, api::make_object<api::error>(404, "Not Found"));
        else answer(id, messageAt(request.chat_id_, std::min(index, MessagesInChat())));
        break;
      }
      case api::getRepliedMessage::ID: {
        const auto &request = static_cast<api::getRepliedMessage &>(*function);
        answer(id, messageAt(request.chat_id_, int(request.message_id_ / kIdStep) - 1));
        break;
      }
      case api::getMessage::ID: {
        const auto &request = static_cast<api::getMessage &>(*function);
        answer(id, messageAt(request.chat_id_, int(request.message_id_ / kIdStep)));
        break;
      }
      case api::getMessageProperties::ID: {
        auto properties = api::make_object<api::messageProperties>();
        properties->can_be_edited_ = true;
        properties->can_be_deleted_for_all_users_ = true;
        properties->can_be_deleted_only_for_self_ = true;
        answer(id, std::move(properties));
        break;
      }
      case api::editMessageText::ID: {
        auto &request = static_cast<api::editMessageText &>(*function);
        auto text = std::string();
        if (request.input_message_content_->get_id() == api::inputMessageText::ID) {
          text = static_cast<api::inputMessageText &>(*request.input_message_content_).text_->text_;
        }
        auto content = api::make_object<api::messageText>();
        content->text_ = Formatted(text);
        update(api::make_object<api::updateMessageContent>(request.chat_id_, request.message_id_, std::move(content)));
        update(api::make_object<api::updateMessageEdited>(request.chat_id_, request.message_id_, kNow, nullptr));
        answer(id, MakeMessage(request.chat_id_, request.message_id_, kSelfId, kNow, text));
        break;
      }
      case api::deleteMessages::ID: {
        auto &request = static_cast<api::deleteMessages &>(*function);
        update(api::make_object<api::updateDeleteMessages>(request.chat_id_, std::move(request.message_ids_), true, false));
        answer(id, api::make_object<api::ok>());
        break;
      }
      case api::getLanguagePackStrings::ID:
        answer(id, api::make_object<api::error>(400, "The scenario has no language pack"));
        break;
      case api::setOption::ID:
      case api::openChat::ID:
      case api::closeChat::ID:
      case api::setChatDraftMessage::ID:
      case api::sendChatAction::ID:
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
    if (WithPhotos()) MakePhotoFiles();
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
      chat->last_read_inbox_message_id_ = (MessagesInChat() - chat->unread_count_) * kIdStep;
      chat->notification_settings_ = api::make_object<api::chatNotificationSettings>();
      chat->last_message_ = messageAt(id, MessagesInChat());
      chat->positions_.push_back(api::make_object<api::chatPosition>(
          api::make_object<api::chatListMain>(), std::int64_t(kChatCount - i + 1) << 32, i == 1, nullptr));
      if (_fullList) decorate(*chat, i);
      _orders[id] = {std::int64_t(kChatCount - i + 1) << 32, false};
      update(api::make_object<api::updateNewChat>(std::move(chat)));
    }
    if (!_fullList) return;
    addOtherChats();
    _ticker = g_timeout_add(kNewMessageEveryMs, +[](gpointer data) -> gboolean {
      static_cast<ScenarioTransport *>(data)->newMessage();
      return G_SOURCE_CONTINUE;
    }, this);
  }

  // One friend for every way a row can look.
  void decorate(api::chat &chat, int index) {
    const auto id = chat.id_;
    const auto outgoing = [&](api::object_ptr<api::MessageSendingState> state) {
      chat.last_message_ = MakeMessage(id, MessagesInChat() * kIdStep, kSelfId, kNow - 60, "Outgoing message");
      chat.last_message_->sending_state_ = std::move(state);
    };
    switch (index) {
      case 2: {
        auto content = api::make_object<api::draftMessageContentText>();
        content->text_ = Formatted("A draft\nof two lines");
        chat.draft_message_ = api::make_object<api::draftMessage>();
        chat.draft_message_->content_ = std::move(content);
        break;
      }
      case 3:
        chat.unread_count_ = 5;
        chat.notification_settings_->mute_for_ = 1000000;
        break;
      case 4:
        chat.unread_count_ = 2;
        chat.unread_mention_count_ = 1;
        break;
      case 5: chat.unread_reaction_count_ = 1; break;
      case 6: chat.is_marked_as_unread_ = true; break;
      case 7:
        outgoing(nullptr);
        chat.last_read_outbox_message_id_ = MessagesInChat() * kIdStep;
        break;
      case 8: outgoing(nullptr); break;
      case 9: outgoing(api::make_object<api::messageSendingStatePending>(0)); break;
      case 10: outgoing(api::make_object<api::messageSendingStateFailed>()); break;
      default: break;
    }
  }

  void addChat(std::int64_t id, api::object_ptr<api::ChatType> type, const std::string &title, std::int64_t order,
               bool archive, const std::string &text, std::int64_t sender) {
    auto chat = api::make_object<api::chat>();
    chat->id_ = id;
    chat->type_ = std::move(type);
    chat->title_ = title;
    chat->notification_settings_ = api::make_object<api::chatNotificationSettings>();
    if (!text.empty()) chat->last_message_ = MakeMessage(id, kIdStep, sender, kNow - 7200, text);
    if (order) {
      chat->positions_.push_back(api::make_object<api::chatPosition>(ListObject(archive), order, false, nullptr));
      _orders[id] = {order, archive};
    }
    update(api::make_object<api::updateNewChat>(std::move(chat)));
  }

  void addOtherChats() {
    update(api::make_object<api::updateUser>(MakeUser(2000, "Anna", "Long")));
    addChat(kSelfId, api::make_object<api::chatTypePrivate>(kSelfId), "Vlad", std::int64_t(100) << 32, false,
            "Saved note", kSelfId);
    addChat(kGroupId, api::make_object<api::chatTypeBasicGroup>(100), "Team", std::int64_t(99) << 32, false,
            "Lunch at one?", 2000);
    addChat(kChannelId, api::make_object<api::chatTypeSupergroup>(200, true), "News", std::int64_t(98) << 32, false,
            "Today's post", 2000);
    update(api::make_object<api::updateChatAction>(1, nullptr, api::make_object<api::messageSenderUser>(1),
                                                   api::make_object<api::chatActionTyping>()));
  }

  // Page by page like the real thing: an account with far more chats than one answer.
  ObjectPtr loadChats(api::loadChats &request) {
    if (!_fullList) return api::make_object<api::error>(404, "Not Found");
    if (request.chat_list_ && request.chat_list_->get_id() == api::chatListArchive::ID) {
      if (_archiveLoaded) return api::make_object<api::error>(404, "Not Found");
      _archiveLoaded = true;
      for (auto i = 1; i <= kArchivedChats; ++i) {
        const auto id = kArchivedChatIds + i;
        update(api::make_object<api::updateUser>(MakeUser(id, "Old", std::to_string(i))));
        addChat(id, api::make_object<api::chatTypePrivate>(id), "Old " + std::to_string(i),
                std::int64_t(kArchivedChats - i + 1) << 32, true, "Archived message", id);
      }
      update(api::make_object<api::updateUnreadChatCount>(ListObject(true), 2, 2, 0, 0, 0));
      return api::make_object<api::ok>();
    }
    if (_loaded >= kLoadedChats) return api::make_object<api::error>(404, "Not Found");
    for (auto i = 0; i < request.limit_ && _loaded < kLoadedChats; ++i) {
      ++_loaded;
      const auto id = kLoadedChatIds + _loaded;
      update(api::make_object<api::updateUser>(MakeUser(id, "Person", std::to_string(_loaded))));
      addChat(id, api::make_object<api::chatTypePrivate>(id), "Person " + std::to_string(_loaded),
              kLoadedChats - _loaded + 1, false, "Older message " + std::to_string(_loaded), id);
    }
    return api::make_object<api::ok>();
  }

  ObjectPtr makeChat(std::int64_t id) {
    auto chat = api::make_object<api::chat>();
    chat->id_ = id;
    chat->type_ = api::make_object<api::chatTypePrivate>(id);
    chat->title_ = id == kSelfId ? "Vlad" : "Chat " + std::to_string(id);
    chat->notification_settings_ = api::make_object<api::chatNotificationSettings>();
    chat->notification_settings_->sound_id_ = 1;
    return chat;
  }

  ObjectPtr publicChats(api::searchPublicChats &request) {
    auto ids = std::vector<std::int64_t>();
    if (_fullList && request.query_.find("pub") != std::string::npos) {
      if (!_publicAdded) {
        _publicAdded = true;
        auto group = api::make_object<api::supergroup>();
        group->id_ = 300;
        auto names = api::make_object<api::usernames>();
        names->editable_username_ = "public_news";
        names->active_usernames_.push_back("public_news");
        group->usernames_ = std::move(names);
        update(api::make_object<api::updateSupergroup>(std::move(group)));
        addChat(kPublicChatId, api::make_object<api::chatTypeSupergroup>(300, true), "Public Channel", 0, false, "", 0);
      }
      ids.push_back(kPublicChatId);
    }
    return api::make_object<api::chats>(int(ids.size()), std::move(ids));
  }

  void position(std::int64_t id, std::int64_t order, bool archive, bool pinned = false) {
    update(api::make_object<api::updateChatPosition>(
        id, api::make_object<api::chatPosition>(ListObject(archive), order, pinned, nullptr)));
  }

  void pin(api::toggleChatIsPinned &request) {
    const auto archive = request.chat_list_ && request.chat_list_->get_id() == api::chatListArchive::ID;
    const auto id = request.chat_id_;
    position(id, request.is_pinned_ ? kPinnedOrder + (++_pins) : _orders[id].first, archive, request.is_pinned_);
  }

  void move(api::addChatToList &request) {
    const auto id = request.chat_id_;
    const auto archive = request.chat_list_ && request.chat_list_->get_id() == api::chatListArchive::ID;
    auto &known = _orders[id];
    position(id, 0, known.second);
    known.second = archive;
    position(id, known.first, archive);
  }

  void remove(std::int64_t id) {
    position(id, 0, _orders[id].second);
    _orders.erase(id);
  }

  // Something happens while a person reads another chat. Always somebody down the list: the row has to come up.
  void newMessage() {
    const auto id = std::int64_t(20 + (_ticks % 10));
    const auto index = MessagesInChat() + 1000 + (++_ticks);
    auto positions = std::vector<api::object_ptr<api::chatPosition>>();
    _orders[id].first = std::int64_t(1000 + _ticks) << 32;
    positions.push_back(api::make_object<api::chatPosition>(ListObject(false), _orders[id].first, false, nullptr));
    update(api::make_object<api::updateChatReadInbox>(id, 0, _ticks));
    update(api::make_object<api::updateChatLastMessage>(
        id, MakeMessage(id, index * kIdStep, id, kNow, "New message " + std::to_string(_ticks)), std::move(positions)));
  }

  api::object_ptr<api::message> messageAt(std::int64_t chatId, int index) {
    const auto sender = index % 3 == 0 ? kSelfId : chatId;
    auto message = MakeMessage(chatId, index * kIdStep, sender, kNow - (MessagesInChat() - index) * 600, MessageText(index));
    if (WithPhotos() && index % 4 == 1) message->content_ = PhotoContent(index);
    // Every seventh message answers the one before it.
    if (index % 7 == 0) {
      message->reply_to_ = api::make_object<api::messageReplyToMessage>(
          chatId, (index - 1) * kIdStep, nullptr, 0, std::string(), nullptr, 0, nullptr);
    }
    return message;
  }

  ObjectPtr history(api::getChatHistory &request) {
    auto result = api::make_object<api::messages>();
    const auto chatId = request.chat_id_;
    auto newest = request.from_message_id_ ? int(request.from_message_id_ / kIdStep) : MessagesInChat() + 1;
    // from_message_id itself is included only with a negative offset, as in TDLib.
    newest -= request.offset_;
    if (request.offset_ == 0) --newest;
    if (newest > MessagesInChat() + _sentExtra[chatId]) newest = MessagesInChat() + _sentExtra[chatId];
    for (auto index = newest; index >= 1 && int(result->messages_.size()) < request.limit_; --index) {
      result->messages_.push_back(messageAt(chatId, index));
    }
    result->total_count_ = MessagesInChat();
    return result;
  }

  ObjectPtr sent(api::sendMessage &request) {
    const auto chatId = request.chat_id_;
    auto text = std::string();
    if (request.input_message_content_
        && request.input_message_content_->get_id() == api::inputMessageText::ID) {
      text = static_cast<api::inputMessageText &>(*request.input_message_content_).text_->text_;
    }
    const auto index = MessagesInChat() + ++_sentExtra[chatId];
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

  const bool _fullList;
  Sink _sink;
  guint _source = 0;
  guint _ticker = 0;
  std::map<std::int64_t, int> _sentExtra;
  std::map<std::int64_t, std::pair<std::int64_t, bool>> _orders;  // Chat: its order when not pinned, and whether archived.
  int _loaded = 0;
  int _pins = 0;
  int _ticks = 0;
  bool _archiveLoaded = false;
  bool _publicAdded = false;
};

}  // namespace

std::unique_ptr<Transport> MakeScenarioTransport() {
  return std::make_unique<ScenarioTransport>();
}

}  // namespace Contrail::Td
