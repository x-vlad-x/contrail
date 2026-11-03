#include "td/scenario.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
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
// A group with topics, below the friends: General, a pinned closed one, one with unread messages and a mention, a
// muted one. Sending "/in N text" in it brings "text" from somebody into topic N, "/unforum" turns the topics off,
// "/pin N" has an admin pin message N of the topic it is sent in.
constexpr auto kForumId = std::int64_t(-400);
constexpr auto kForumGroupId = std::int64_t(400);
constexpr auto kForumOrder = std::int64_t(1) << 31;
constexpr auto kTopicMessages = 30;
constexpr auto kForumUsers = std::int64_t(2001);
constexpr auto kPinnedOrder = std::int64_t(1) << 62;
constexpr auto kNewMessageEveryMs = 5000;
// Message ids in TDLib are server ids shifted by 20 bits.
constexpr auto kIdStep = std::int64_t(1) << 20;
// Pinned messages: three in Friend 1's chat (the middle one a photo, whatever CONTRAIL_SCENARIO_PHOTOS says), one in
// Friend 2's, one in the group's topic Random. Nobody but its admins may pin in the group: there the bar can only be
// hidden.
constexpr auto kPinnedPhoto = 70;

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

// The blurred picture a message carries: a JPEG of a few hundred bytes.
api::object_ptr<api::minithumbnail> MiniThumbnail() {
  auto *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 40, 30);
  gdk_pixbuf_fill(pixbuf, 0x3080c0ff);
  gchar *buffer = nullptr;
  gsize length = 0;
  auto result = api::object_ptr<api::minithumbnail>();
  if (gdk_pixbuf_save_to_buffer(pixbuf, &buffer, &length, "jpeg", nullptr, "quality", "40", nullptr)) {
    result = api::make_object<api::minithumbnail>(40, 30, std::string(buffer, length));
  }
  g_free(buffer);
  g_object_unref(pixbuf);
  return result;
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
    if (forumRequest(id, *function) || pinnedRequest(id, *function)) return;
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
      case api::createPrivateChat::ID: {
        const auto user = static_cast<api::createPrivateChat &>(*function).user_id_;
        // Saved Messages, never used yet: TDLib tells about the chat before the answer, as about any new one.
        if (user == kSelfId && !_selfAdded) {
          _selfAdded = true;
          update(api::make_object<api::updateNewChat>(
              api::move_object_as<api::chat>(makeChat(kSelfId))));
        }
        answer(id, makeChat(user));
        break;
      }
      case api::forwardMessages::ID:
        answer(id, forwarded(static_cast<api::forwardMessages &>(*function)));
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
        properties->can_be_forwarded_ = true;
        properties->can_be_copied_ = true;
        properties->can_be_saved_ = true;
        properties->can_be_replied_ = true;
        properties->can_be_deleted_for_all_users_ = true;
        properties->can_be_deleted_only_for_self_ = true;
        const auto chat = static_cast<api::getMessageProperties &>(*function).chat_id_;
        properties->can_be_pinned_ = chat != kForumId && chat != kChannelId;
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
    addForum();
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
    _selfAdded = true;
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
    if (chatId == 1 && index == kPinnedPhoto) {
      auto photo = api::move_object_as<api::messagePhoto>(PhotoContent(index));
      photo->photo_->minithumbnail_ = MiniThumbnail();
      photo->caption_ = Formatted("Pinned photo");
      message->content_ = std::move(photo);
    }
    // Every seventh message answers the one before it.
    if (index % 7 == 0) {
      message->reply_to_ = api::make_object<api::messageReplyToMessage>(
          chatId, (index - 1) * kIdStep, nullptr, 0, std::string(), nullptr, 0, nullptr);
    }
    applyEntry(*message);
    message->is_pinned_ = isPinned(chatId, message->id_);
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
    auto text = std::string();
    if (request.input_message_content_
        && request.input_message_content_->get_id() == api::inputMessageText::ID) {
      text = static_cast<api::inputMessageText &>(*request.input_message_content_).text_->text_;
    }
    return outgoing(request.chat_id_, Entry{.text = text});
  }

  // --- what this account sent or forwarded ---------------------------------------

  // Who a forwarded message was written by.
  struct Origin {
    std::int64_t user = 0;
    std::int64_t chat = 0;
    std::string hidden;
  };
  struct Entry {
    std::string text;
    int photo = -1;  // The scenario's photo of this index, with `text` as its caption.
    bool forwarded = false;
    Origin origin;
  };

  // A message sent or forwarded here: it stays as sent when the chat is opened again.
  void applyEntry(api::message &message) {
    const auto it = _entries.find({message.chat_id_, message.id_ - message.id_ % kIdStep});
    if (it == _entries.end()) return;
    const auto &entry = it->second;
    message.sender_id_ = api::make_object<api::messageSenderUser>(kSelfId);
    message.is_outgoing_ = true;
    message.date_ = kNow;
    message.reply_to_ = nullptr;
    if (entry.photo >= 0) {
      auto content = api::move_object_as<api::messagePhoto>(PhotoContent(entry.photo));
      content->caption_ = Formatted(entry.text);
      message.content_ = std::move(content);
    } else {
      auto content = api::make_object<api::messageText>();
      content->text_ = Formatted(entry.text);
      message.content_ = std::move(content);
    }
    message.forward_info_ = nullptr;
    if (!entry.forwarded) return;
    auto origin = api::object_ptr<api::MessageOrigin>();
    if (entry.origin.user) {
      origin = api::make_object<api::messageOriginUser>(entry.origin.user);
    } else if (entry.origin.chat) {
      origin = api::make_object<api::messageOriginChat>(entry.origin.chat, std::string());
    } else {
      origin = api::make_object<api::messageOriginHiddenUser>(entry.origin.hidden);
    }
    message.forward_info_ = api::make_object<api::messageForwardInfo>(std::move(origin), kNow - 3600, nullptr,
                                                                      std::string());
  }

  api::object_ptr<api::message> outgoing(std::int64_t chatId, Entry entry) {
    const auto index = MessagesInChat() + ++_sentExtra[chatId];
    const auto id = index * kIdStep;
    _entries[{chatId, id}] = std::move(entry);
    const auto temporary = id + 1;
    auto pending = messageAt(chatId, index);
    pending->id_ = temporary;
    pending->sending_state_ = api::make_object<api::messageSendingStatePending>(0);
    auto shown = messageAt(chatId, index);
    shown->id_ = temporary;
    update(api::make_object<api::updateNewMessage>(std::move(shown)));
    update(api::make_object<api::updateMessageSendSucceeded>(messageAt(chatId, index), temporary));
    update(api::make_object<api::updateChatLastMessage>(chatId, messageAt(chatId, index),
                                                        std::vector<api::object_ptr<api::chatPosition>>()));
    return pending;
  }

  // A message as the chat has it now, null when there is none.
  api::object_ptr<api::message> existing(std::int64_t chatId, std::int64_t id) {
    if (chatId == kForumId) {
      const auto topic = findTopic(TopicOfId(id));
      const auto index = IndexOfId(id);
      if (!topic || index < 1 || index > topic->last) return nullptr;
      return topicMessage(topic->id, index, index > kTopicMessages ? _sentTexts[id] : "");
    }
    const auto index = int(id / kIdStep);
    if (index < 1 || index > MessagesInChat() + _sentExtra[chatId]) return nullptr;
    return messageAt(chatId, index);
  }

  // As Telegram does: with the senders' names the copies say whose they were (of a forwarded one, whose it was
  // first); without them they are this account's own messages.
  ObjectPtr forwarded(api::forwardMessages &request) {
    auto result = api::make_object<api::messages>();
    const auto topicId = request.topic_id_ && request.topic_id_->get_id() == api::messageTopicForum::ID
        ? static_cast<api::messageTopicForum &>(*request.topic_id_).forum_topic_id_ : 1;
    for (const auto id : request.message_ids_) {
      auto source = existing(request.from_chat_id_, id);
      if (!source) {
        result->messages_.push_back(nullptr);
        continue;
      }
      auto entry = Entry();
      if (const auto text = source->content_->get_id() == api::messageText::ID
              ? static_cast<const api::messageText &>(*source->content_).text_.get() : nullptr) {
        entry.text = text->text_;
      } else if (source->content_->get_id() == api::messagePhoto::ID) {
        const auto &photo = static_cast<const api::messagePhoto &>(*source->content_);
        entry.photo = int(source->id_ / kIdStep);
        if (!request.remove_caption_ && photo.caption_) entry.text = photo.caption_->text_;
      }
      if (!request.send_copy_) {
        entry.forwarded = true;
        const auto origin = source->forward_info_ ? source->forward_info_->origin_.get() : nullptr;
        if (origin && origin->get_id() == api::messageOriginUser::ID) {
          entry.origin.user = static_cast<const api::messageOriginUser &>(*origin).sender_user_id_;
        } else if (origin && origin->get_id() == api::messageOriginChat::ID) {
          entry.origin.chat = static_cast<const api::messageOriginChat &>(*origin).sender_chat_id_;
        } else if (origin && origin->get_id() == api::messageOriginHiddenUser::ID) {
          entry.origin.hidden = static_cast<const api::messageOriginHiddenUser &>(*origin).sender_name_;
        } else if (source->sender_id_->get_id() == api::messageSenderUser::ID) {
          entry.origin.user = static_cast<const api::messageSenderUser &>(*source->sender_id_).user_id_;
        } else {
          entry.origin.chat = static_cast<const api::messageSenderChat &>(*source->sender_id_).chat_id_;
        }
      }
      if (request.chat_id_ == kForumId) {
        const auto into = findTopic(topicId);
        if (!into) return api::make_object<api::error>(400, "TOPIC_ID_INVALID");
        result->messages_.push_back(forumOutgoing(*into, std::move(entry)));
      } else {
        result->messages_.push_back(outgoing(request.chat_id_, std::move(entry)));
      }
    }
    result->total_count_ = int(result->messages_.size());
    return result;
  }


  // --- pinned messages -----------------------------------------------------------

  bool isPinned(std::int64_t chat, std::int64_t id) const {
    const auto it = _pinned.find(chat);
    return it != _pinned.end() && it->second.contains(id);
  }

  api::object_ptr<api::message> anyMessage(std::int64_t chat, std::int64_t id) {
    if (chat == kForumId) {
      const auto topic = TopicOfId(id);
      const auto index = IndexOfId(id);
      return topicMessage(topic, index, index > kTopicMessages ? _sentTexts[id] : "");
    }
    return messageAt(chat, int(id / kIdStep));
  }

  void setPinned(std::int64_t chat, std::int64_t id, bool pinned) {
    auto &ids = _pinned[chat];
    if (pinned ? !ids.insert(id).second : !ids.erase(id)) return;
    update(api::make_object<api::updateMessageIsPinned>(chat, id, pinned));
  }

  // Newest first, as TDLib answers; from_message_id itself is left out.
  ObjectPtr searchPinned(const api::searchChatMessages &request) {
    auto ids = std::vector<std::int64_t>();
    if (const auto it = _pinned.find(request.chat_id_); it != _pinned.end()) {
      const auto topic = request.topic_id_ && request.topic_id_->get_id() == api::messageTopicForum::ID
          ? static_cast<const api::messageTopicForum &>(*request.topic_id_).forum_topic_id_ : 0;
      for (const auto id : it->second) {
        if (!topic || TopicOfId(id) == topic) ids.push_back(id);
      }
    }
    std::ranges::sort(ids, std::greater<>());
    auto result = api::make_object<api::foundChatMessages>();
    result->total_count_ = int(ids.size());
    for (const auto id : ids) {
      if (request.from_message_id_ && id >= request.from_message_id_) continue;
      if (int(result->messages_.size()) == request.limit_) {
        result->next_from_message_id_ = result->messages_.back()->id_;
        break;
      }
      result->messages_.push_back(anyMessage(request.chat_id_, id));
    }
    return result;
  }

  bool pinnedRequest(std::uint64_t id, api::Function &function) {
    switch (function.get_id()) {
      case api::searchChatMessages::ID: {
        auto &request = static_cast<api::searchChatMessages &>(function);
        if (!request.filter_ || request.filter_->get_id() != api::searchMessagesFilterPinned::ID) return false;
        answer(id, searchPinned(request));
        return true;
      }
      case api::getChatPinnedMessage::ID: {
        const auto chat = static_cast<api::getChatPinnedMessage &>(function).chat_id_;
        const auto it = _pinned.find(chat);
        if (it == _pinned.end() || it->second.empty()) answer(id, api::make_object<api::error>(404, "Not Found"));
        else answer(id, anyMessage(chat, *it->second.rbegin()));
        return true;
      }
      case api::pinChatMessage::ID: {
        const auto &request = static_cast<api::pinChatMessage &>(function);
        if (request.chat_id_ == kForumId || request.chat_id_ == kChannelId) {
          answer(id, api::make_object<api::error>(400, "CHAT_ADMIN_REQUIRED"));
          return true;
        }
        setPinned(request.chat_id_, request.message_id_, true);
        answer(id, api::make_object<api::ok>());
        return true;
      }
      case api::unpinChatMessage::ID: {
        const auto &request = static_cast<api::unpinChatMessage &>(function);
        setPinned(request.chat_id_, request.message_id_, false);
        answer(id, api::make_object<api::ok>());
        return true;
      }
      case api::unpinAllChatMessages::ID: {
        const auto chat = static_cast<api::unpinAllChatMessages &>(function).chat_id_;
        const auto ids = _pinned[chat];
        for (const auto message : ids) setPinned(chat, message, false);
        answer(id, api::make_object<api::ok>());
        return true;
      }
      case api::unpinAllForumTopicMessages::ID: {
        const auto &request = static_cast<api::unpinAllForumTopicMessages &>(function);
        const auto ids = _pinned[request.chat_id_];
        for (const auto message : ids) {
          if (TopicOfId(message) == request.forum_topic_id_) setPinned(request.chat_id_, message, false);
        }
        answer(id, api::make_object<api::ok>());
        return true;
      }
      default:
        return false;
    }
  }

  // --- the group with topics ---------------------------------------------------

  struct Topic {
    std::int32_t id = 0;
    std::string name;
    int last = kTopicMessages;  // Its messages are 1..last.
    std::int64_t readInbox = 0;
    int mentions = 0;
    bool pinned = false;
    bool closed = false;
    bool muted = false;
    std::string draft;
  };

  // A message id of the group: the topic in the lowest digit, so the topics' messages go in turn.
  static std::int64_t TopicMessageId(std::int32_t topic, int index) { return (std::int64_t(index) * 10 + topic) * kIdStep; }
  static std::int32_t TopicOfId(std::int64_t id) { return std::int32_t((id / kIdStep) % 10); }
  static int IndexOfId(std::int64_t id) { return int((id / kIdStep) / 10); }

  Topic *findTopic(std::int32_t id) {
    const auto it = std::ranges::find_if(_topics, [&](const Topic &topic) { return topic.id == id; });
    return it == _topics.end() ? nullptr : &*it;
  }

  static std::int64_t SenderOf(std::int32_t topic, int index) {
    return index % 3 == 0 ? kSelfId : kForumUsers + (index + topic) % 3;
  }

  api::object_ptr<api::message> topicMessage(std::int32_t topic, int index, const std::string &text = std::string()) {
    const auto id = TopicMessageId(topic, index);
    const auto date = std::min(kNow - (kTopicMessages - index) * 600 + topic * 10, kNow);
    auto message = MakeMessage(kForumId, id, SenderOf(topic, index), date,
                               text.empty() ? "Topic " + std::to_string(topic) + ", message " + std::to_string(index) : text);
    message->topic_id_ = api::make_object<api::messageTopicForum>(topic);
    applyEntry(*message);
    message->is_pinned_ = isPinned(kForumId, id);
    return message;
  }

  int unreadOf(const Topic &topic) {
    auto result = 0;
    for (auto index = topic.last; index >= 1 && TopicMessageId(topic.id, index) > topic.readInbox; --index) {
      if (SenderOf(topic.id, index) != kSelfId) ++result;
    }
    return result;
  }

  api::object_ptr<api::chatNotificationSettings> topicSettings(const Topic &topic) {
    auto settings = api::make_object<api::chatNotificationSettings>();
    settings->use_default_mute_for_ = !topic.muted;
    settings->mute_for_ = topic.muted ? 0x7fffffff : 0;
    return settings;
  }

  api::object_ptr<api::draftMessage> topicDraft(const Topic &topic) {
    if (topic.draft.empty()) return nullptr;
    auto content = api::make_object<api::draftMessageContentText>();
    content->text_ = Formatted(topic.draft);
    auto draft = api::make_object<api::draftMessage>();
    draft->content_ = std::move(content);
    return draft;
  }

  api::object_ptr<api::forumTopic> forumTopic(const Topic &topic) {
    auto info = api::make_object<api::forumTopicInfo>(
        kForumId, topic.id, topic.name, api::make_object<api::forumTopicIcon>(0x6FB9F0, 0), kNow - 86400 * 30,
        api::make_object<api::messageSenderUser>(kForumUsers), topic.id == 1, false, topic.closed, false, false);
    auto result = api::make_object<api::forumTopic>();
    result->info_ = std::move(info);
    result->last_message_ = topicMessage(topic.id, topic.last, topic.last > kTopicMessages ? _sentTexts[TopicMessageId(topic.id, topic.last)] : "");
    result->order_ = result->last_message_->id_;
    result->is_pinned_ = topic.pinned;
    result->unread_count_ = unreadOf(topic);
    result->last_read_inbox_message_id_ = topic.readInbox;
    result->last_read_outbox_message_id_ = TopicMessageId(topic.id, topic.last);
    result->unread_mention_count_ = topic.mentions;
    result->notification_settings_ = topicSettings(topic);
    result->draft_message_ = topicDraft(topic);
    return result;
  }

  void topicChanged(const Topic &topic) {
    update(api::make_object<api::updateForumTopic>(kForumId, topic.id, topic.pinned, topic.readInbox,
                                                   TopicMessageId(topic.id, topic.last), topic.mentions, 0, 0,
                                                   topicSettings(topic), topicDraft(topic)));
  }

  // The group's last message, unread count and mentions follow its topics.
  api::object_ptr<api::message> forumLastMessage() {
    const Topic *newest = nullptr;
    for (const auto &topic : _topics) {
      if (!newest || TopicMessageId(topic.id, topic.last) > TopicMessageId(newest->id, newest->last)) newest = &topic;
    }
    const auto id = TopicMessageId(newest->id, newest->last);
    return topicMessage(newest->id, newest->last, newest->last > kTopicMessages ? _sentTexts[id] : "");
  }

  void forumCounters() {
    auto unread = 0, mentions = 0;
    auto read = std::int64_t(0);
    for (const auto &topic : _topics) {
      unread += unreadOf(topic);
      mentions += topic.mentions;
      read = std::max(read, topic.readInbox);
    }
    update(api::make_object<api::updateChatReadInbox>(kForumId, read, unread));
    update(api::make_object<api::updateChatUnreadMentionCount>(kForumId, mentions));
  }

  void addForum() {
    update(api::make_object<api::updateUser>(MakeUser(kForumUsers, "Anna", "K")));
    update(api::make_object<api::updateUser>(MakeUser(kForumUsers + 1, "Boris", "L")));
    update(api::make_object<api::updateUser>(MakeUser(kForumUsers + 2, "Clara", "M")));
    _topics = {
        {.id = 1, .name = "General"},
        {.id = 2, .name = "Announcements", .pinned = true, .closed = true},
        {.id = 3, .name = "Random", .mentions = 1},
        {.id = 4, .name = "Help", .muted = true},
    };
    for (auto &topic : _topics) topic.readInbox = TopicMessageId(topic.id, topic.last);
    // Random: the last few messages not read yet.
    findTopic(3)->readInbox = TopicMessageId(3, kTopicMessages - 4);
    supergroup(true);
    auto chat = api::make_object<api::chat>();
    chat->id_ = kForumId;
    chat->type_ = api::make_object<api::chatTypeSupergroup>(kForumGroupId, false);
    chat->title_ = "Contrail Club";
    chat->notification_settings_ = api::make_object<api::chatNotificationSettings>();
    chat->notification_settings_->use_default_mute_for_ = true;
    chat->last_message_ = forumLastMessage();
    chat->view_as_topics_ = true;
    chat->positions_.push_back(api::make_object<api::chatPosition>(ListObject(false), kForumOrder, false, nullptr));
    _orders[kForumId] = {kForumOrder, false};
    update(api::make_object<api::updateNewChat>(std::move(chat)));
    forumCounters();
  }

  void supergroup(bool forum) {
    auto group = api::make_object<api::supergroup>();
    group->id_ = kForumGroupId;
    group->member_count_ = 12;
    group->is_forum_ = forum;
    update(api::make_object<api::updateSupergroup>(std::move(group)));
  }

  ObjectPtr forumHistory(const api::getForumTopicHistory &request) {
    const auto topic = findTopic(request.forum_topic_id_);
    if (!topic) return api::make_object<api::error>(400, "TOPIC_ID_INVALID");
    // As in history(): from_message_id itself only with a negative offset. It may be of another topic.
    const auto from = request.from_message_id_;
    auto ids = std::vector<std::int64_t>();
    for (auto index = 1; index <= topic->last; ++index) ids.push_back(TopicMessageId(topic->id, index));
    auto position = from ? int(std::ranges::lower_bound(ids, from) - ids.begin()) : int(ids.size());
    const auto exists = position < int(ids.size()) && ids[position] == from;
    auto newest = exists ? (request.offset_ == 0 ? position - 1 : position - request.offset_) : position - 1 - request.offset_;
    newest = std::min(newest, int(ids.size()) - 1);
    auto result = api::make_object<api::messages>();
    for (auto i = newest; i >= 0 && int(result->messages_.size()) < request.limit_; --i) {
      const auto index = i + 1;
      result->messages_.push_back(topicMessage(topic->id, index, index > kTopicMessages ? _sentTexts[ids[i]] : ""));
    }
    result->total_count_ = int(ids.size());
    return result;
  }

  // "/in N text" and "/unforum" do what the other side would; anything else is sent into its topic.
  ObjectPtr forumSent(api::sendMessage &request) {
    const auto topicId = request.topic_id_ && request.topic_id_->get_id() == api::messageTopicForum::ID
        ? static_cast<api::messageTopicForum &>(*request.topic_id_).forum_topic_id_ : 1;
    auto text = std::string();
    if (request.input_message_content_ && request.input_message_content_->get_id() == api::inputMessageText::ID) {
      text = static_cast<api::inputMessageText &>(*request.input_message_content_).text_->text_;
    }
    if (text == "/unforum") {
      supergroup(false);
      return api::make_object<api::error>(400, "Scenario: the group has no topics now");
    }
    if (text.rfind("/pin ", 0) == 0) {
      setPinned(kForumId, TopicMessageId(topicId, std::atoi(text.c_str() + 5)), true);
      return api::make_object<api::error>(400, "Scenario: pinned by an admin");
    }
    auto incoming = text.rfind("/in ", 0) == 0;
    auto into = findTopic(topicId);
    if (incoming) {
      into = findTopic(std::atoi(text.c_str() + 4));
      const auto gap = text.find(' ', 4);
      text = gap == std::string::npos ? std::string("Hello") : text.substr(gap + 1);
    }
    if (!into) return api::make_object<api::error>(400, "TOPIC_ID_INVALID");
    // Every third index is this account's (SenderOf): skip to one of the right side.
    auto index = into->last + 1;
    while ((SenderOf(into->id, index) == kSelfId) != !incoming) ++index;
    into->last = index;
    const auto final = TopicMessageId(into->id, index);
    _sentTexts[final] = text;
    auto message = topicMessage(into->id, index, text);
    message->date_ = kNow;
    if (incoming) {
      update(api::make_object<api::updateNewMessage>(std::move(message)));
      update(api::make_object<api::updateChatLastMessage>(kForumId, forumLastMessage(),
                                                          std::vector<api::object_ptr<api::chatPosition>>()));
      forumCounters();
      return api::make_object<api::error>(400, "Scenario: a message came into topic " + std::to_string(into->id));
    }
    into->draft.clear();
    return sendInto(*into, index, std::move(message));
  }

  // This account's message `index` of a topic, sent there now.
  api::object_ptr<api::message> sendInto(Topic &into, int index, api::object_ptr<api::message> message) {
    const auto temporary = TopicMessageId(into.id, index) + 1;
    const auto text = _sentTexts[TopicMessageId(into.id, index)];
    auto pending = topicMessage(into.id, index, text);
    pending->id_ = temporary;
    pending->date_ = kNow;
    pending->sending_state_ = api::make_object<api::messageSendingStatePending>(0);
    auto shown = topicMessage(into.id, index, text);
    shown->id_ = temporary;
    shown->date_ = kNow;
    update(api::make_object<api::updateNewMessage>(std::move(shown)));
    update(api::make_object<api::updateMessageSendSucceeded>(std::move(message), temporary));
    update(api::make_object<api::updateChatLastMessage>(kForumId, forumLastMessage(),
                                                        std::vector<api::object_ptr<api::chatPosition>>()));
    topicChanged(into);
    return pending;
  }

  api::object_ptr<api::message> forumOutgoing(Topic &into, Entry entry) {
    auto index = into.last + 1;
    while (SenderOf(into.id, index) != kSelfId) ++index;
    into.last = index;
    const auto id = TopicMessageId(into.id, index);
    _sentTexts[id] = entry.text;
    _entries[{kForumId, id}] = std::move(entry);
    auto message = topicMessage(into.id, index, _sentTexts[id]);
    return sendInto(into, index, std::move(message));
  }

  // The requests about the group with topics; false for anything else.
  bool forumRequest(std::uint64_t id, api::Function &function) {
    switch (function.get_id()) {
      case api::getForumTopics::ID: {
        auto result = api::make_object<api::forumTopics>();
        if (static_cast<api::getForumTopics &>(function).chat_id_ == kForumId) {
          for (const auto &topic : _topics) result->topics_.push_back(forumTopic(topic));
        }
        result->total_count_ = int(result->topics_.size());
        answer(id, std::move(result));
        return true;
      }
      case api::getForumTopic::ID: {
        const auto &request = static_cast<api::getForumTopic &>(function);
        const auto topic = request.chat_id_ == kForumId ? findTopic(request.forum_topic_id_) : nullptr;
        if (topic) answer(id, forumTopic(*topic));
        else answer(id, api::make_object<api::error>(400, "TOPIC_ID_INVALID"));
        return true;
      }
      case api::getForumTopicHistory::ID:
        answer(id, forumHistory(static_cast<api::getForumTopicHistory &>(function)));
        return true;
      case api::setForumTopicNotificationSettings::ID: {
        auto &request = static_cast<api::setForumTopicNotificationSettings &>(function);
        if (const auto topic = findTopic(request.forum_topic_id_)) {
          const auto &settings = request.notification_settings_;
          topic->muted = settings && !settings->use_default_mute_for_ && settings->mute_for_ > 0;
          topicChanged(*topic);
        }
        answer(id, api::make_object<api::ok>());
        return true;
      }
      case api::readAllForumTopicMentions::ID: {
        if (const auto topic = findTopic(static_cast<api::readAllForumTopicMentions &>(function).forum_topic_id_)) {
          topic->mentions = 0;
          topicChanged(*topic);
          forumCounters();
        }
        answer(id, api::make_object<api::ok>());
        return true;
      }
      case api::readAllForumTopicReactions::ID:
        answer(id, api::make_object<api::ok>());
        return true;
      case api::sendMessage::ID: {
        auto &request = static_cast<api::sendMessage &>(function);
        if (request.chat_id_ != kForumId) return false;
        answer(id, forumSent(request));
        return true;
      }
      case api::viewMessages::ID: {
        auto &request = static_cast<api::viewMessages &>(function);
        if (request.chat_id_ != kForumId) return false;
        for (const auto message : request.message_ids_) {
          const auto topic = findTopic(TopicOfId(message));
          if (!topic || message <= topic->readInbox) continue;
          topic->readInbox = message;
          if (message >= TopicMessageId(topic->id, topic->last)) topic->mentions = 0;
          topicChanged(*topic);
        }
        forumCounters();
        answer(id, api::make_object<api::ok>());
        return true;
      }
      case api::setChatDraftMessage::ID: {
        auto &request = static_cast<api::setChatDraftMessage &>(function);
        if (request.chat_id_ != kForumId) return false;
        const auto forum = request.topic_id_ && request.topic_id_->get_id() == api::messageTopicForum::ID
            ? static_cast<api::messageTopicForum &>(*request.topic_id_).forum_topic_id_ : 0;
        if (const auto topic = findTopic(forum)) {
          topic->draft.clear();
          const auto &draft = request.draft_message_;
          if (draft && draft->content_ && draft->content_->get_id() == api::draftMessageContentText::ID) {
            const auto &text = static_cast<const api::draftMessageContentText &>(*draft->content_).text_;
            if (text) topic->draft = text->text_;
          }
          topicChanged(*topic);
        }
        answer(id, api::make_object<api::ok>());
        return true;
      }
      case api::getMessage::ID:
      case api::getRepliedMessage::ID: {
        const auto chat = function.get_id() == api::getMessage::ID
            ? static_cast<api::getMessage &>(function).chat_id_ : static_cast<api::getRepliedMessage &>(function).chat_id_;
        if (chat != kForumId) return false;
        const auto message = function.get_id() == api::getMessage::ID
            ? static_cast<api::getMessage &>(function).message_id_
            : static_cast<api::getRepliedMessage &>(function).message_id_;
        const auto topic = findTopic(TopicOfId(message));
        const auto index = IndexOfId(message);
        if (!topic || index < 1 || index > topic->last) {
          answer(id, api::make_object<api::error>(404, "Not Found"));
        } else {
          answer(id, topicMessage(topic->id, index, index > kTopicMessages ? _sentTexts[message] : ""));
        }
        return true;
      }
      case api::getChatHistory::ID:
        // Its messages are read in the topics.
        if (static_cast<api::getChatHistory &>(function).chat_id_ != kForumId) return false;
        answer(id, api::make_object<api::messages>(0, std::vector<api::object_ptr<api::message>>()));
        return true;
      default:
        return false;
    }
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
  std::vector<Topic> _topics;
  std::map<std::int64_t, std::string> _sentTexts;  // What was written in the topics, by message id.
  std::map<std::pair<std::int64_t, std::int64_t>, Entry> _entries;  // By chat and message id.
  std::map<std::int64_t, std::set<std::int64_t>> _pinned = {
      {1, {30 * kIdStep, kPinnedPhoto * kIdStep, 100 * kIdStep}},
      {2, {50 * kIdStep}},
      {kForumId, {(10 * 10 + 3) * kIdStep}},
  };
  bool _selfAdded = false;
};

}  // namespace

std::unique_ptr<Transport> MakeScenarioTransport() {
  return std::make_unique<ScenarioTransport>();
}

}  // namespace Contrail::Td
