#include "td/store.hpp"

#include <glib.h>

#include <algorithm>
#include <utility>

namespace Contrail::Td {
namespace {

// A chat with notifications muted for longer than this counts as muted for good; the list only shows the icon.
bool IsMuted(const api::chatNotificationSettings &settings) {
  return settings.mute_for_ > 0;
}

template <typename Object>
Object *As(api::Object *object) {
  return object && object->get_id() == Object::ID ? static_cast<Object *>(object) : nullptr;
}

FileId PhotoOf(const api::object_ptr<api::chatPhotoInfo> &photo) {
  return photo && photo->small_ ? photo->small_->id_ : 0;
}

std::string DraftText(const api::object_ptr<api::draftMessage> &draft) {
  if (!draft || !draft->content_) return {};
  if (draft->content_->get_id() != api::draftMessageContentText::ID) return {};
  const auto &text = static_cast<const api::draftMessageContentText &>(*draft->content_).text_;
  return text ? text->text_ : std::string();
}

}  // namespace

std::string DisplayName(const User &user) {
  if (user.lastName.empty()) return user.firstName;
  if (user.firstName.empty()) return user.lastName;
  return user.firstName + " " + user.lastName;
}

Store::Store(Client &client, Preview preview) : _client(client), _preview(std::move(preview)) {
  _client.subscribe([this](api::Update &update) { updated(update); }, _lifetime);
}

const Chat *Store::chat(ChatId id) const {
  const auto it = _chats.find(id);
  return it == _chats.end() ? nullptr : &it->second;
}

const User *Store::user(UserId id) const {
  const auto it = _users.find(id);
  return it == _users.end() ? nullptr : &it->second;
}

const File *Store::file(FileId id) const {
  const auto it = _files.find(id);
  return it == _files.end() ? nullptr : &it->second;
}

const std::vector<ChatId> &Store::list(ListKind kind) {
  auto &state = _lists[int(kind)];
  if (state.dirty) {
    state.dirty = false;
    const auto key = [&](ChatId id) -> const Position & {
      const auto &chat = _chats.at(id);
      return kind == ListKind::Main ? chat.main : chat.archive;
    };
    state.ids.clear();
    for (const auto &[id, chat] : _chats) {
      if (key(id).order) state.ids.push_back(id);
    }
    // Telegram's own order: `order` already puts pinned chats first. Ties go by id, as TDLib's.
    std::ranges::sort(state.ids, [&](ChatId a, ChatId b) {
      const auto left = key(a).order;
      const auto right = key(b).order;
      return left != right ? left > right : a > b;
    });
  }
  return state.ids;
}

void Store::loadMore(ListKind kind, int limit) {
  auto &state = _lists[int(kind)];
  if (state.loading || state.complete) return;
  state.loading = true;
  auto list = kind == ListKind::Main ? api::object_ptr<api::ChatList>(api::make_object<api::chatListMain>())
                                     : api::object_ptr<api::ChatList>(api::make_object<api::chatListArchive>());
  _client.send(api::make_object<api::loadChats>(std::move(list), limit),
               [this, kind](ObjectPtr object) {
                 auto &state = _lists[int(kind)];
                 state.loading = false;
                 if (const auto error = As<api::error>(object.get())) {
                   // 404: there are no more chats to load.
                   if (error->code_ == 404) {
                     state.complete = true;
                     listChanged(kind);
                   } else {
                     _client.reportError(*error);
                   }
                 }
               },
               _lifetime);
}

void Store::download(FileId id, int priority) {
  if (const auto known = file(id); known && known->complete) return;
  _client.send(api::make_object<api::downloadFile>(id, priority, 0, 0, false), nullptr);
}

void Store::onChat(ChatHandler handler, const Lifetime &lifetime) {
  _chatHandlers.push_back({std::move(handler), lifetime.token()});
}

void Store::onList(ListHandler handler, const Lifetime &lifetime) {
  _listHandlers.push_back({std::move(handler), lifetime.token()});
}

void Store::onUser(UserHandler handler, const Lifetime &lifetime) {
  _userHandlers.push_back({std::move(handler), lifetime.token()});
}

void Store::onFile(FileHandler handler, const Lifetime &lifetime) {
  _fileHandlers.push_back({std::move(handler), lifetime.token()});
}

void Store::onConnection(std::function<void()> handler, const Lifetime &lifetime) {
  _connectionHandlers.push_back({std::move(handler), lifetime.token()});
}

template <typename Handler, typename... Arguments>
void Store::notify(std::vector<Subscriber<Handler>> &subscribers, const Arguments &...arguments) {
  // A handler can subscribe while this runs (a new row): those see the next change.
  const auto count = subscribers.size();
  for (auto i = std::size_t(0); i < count; ++i) {
    if (subscribers[i].token.expired()) continue;
    const auto handler = subscribers[i].handler;
    handler(arguments...);
  }
  std::erase_if(subscribers, [](const Subscriber<Handler> &item) { return item.token.expired(); });
}

void Store::chatChanged(ChatId id) {
  notify(_chatHandlers, id);
}

void Store::listChanged(ListKind kind) {
  _lists[int(kind)].dirty = true;
  notify(_listHandlers, kind);
}

void Store::setPosition(Chat &chat, api::chatPosition &position) {
  if (!position.list_) return;
  const auto listId = position.list_->get_id();
  if (listId != api::chatListMain::ID && listId != api::chatListArchive::ID) return;  // Folders aren't shown.
  const auto kind = listId == api::chatListMain::ID ? ListKind::Main : ListKind::Archive;
  auto &target = kind == ListKind::Main ? chat.main : chat.archive;
  const auto changed = target.order != position.order_ || target.pinned != position.is_pinned_;
  target = {position.order_, position.is_pinned_};
  if (changed) listChanged(kind);
}

void Store::setPositions(Chat &chat, std::vector<api::object_ptr<api::chatPosition>> &positions) {
  for (auto &position : positions) {
    if (position) setPosition(chat, *position);
  }
}

void Store::setLastMessage(Chat &chat, api::message *message) {
  chat.preview = message && _preview ? _preview(*message) : std::string();
  chat.lastDate = message ? message->date_ : 0;
  chat.lastMessageId = message ? message->id_ : 0;
  chat.lastOutgoing = message && message->is_outgoing_;
}

void Store::addChat(api::chat &source) {
  auto chat = Chat();
  chat.id = source.id_;
  chat.title = source.title_;
  chat.photo = PhotoOf(source.photo_);
  chat.accentColor = source.accent_color_id_;
  chat.unread = source.unread_count_;
  chat.unreadMentions = source.unread_mention_count_;
  chat.markedUnread = source.is_marked_as_unread_;
  chat.readInbox = source.last_read_inbox_message_id_;
  chat.readOutbox = source.last_read_outbox_message_id_;
  chat.muted = source.notification_settings_ && IsMuted(*source.notification_settings_);
  chat.draft = DraftText(source.draft_message_);
  chat.forum = source.view_as_topics_;
  if (const auto type = source.type_.get()) {
    switch (type->get_id()) {
      case api::chatTypePrivate::ID:
        chat.kind = ChatKind::Private;
        chat.peerId = static_cast<api::chatTypePrivate *>(type)->user_id_;
        break;
      case api::chatTypeBasicGroup::ID:
        chat.kind = ChatKind::Group;
        chat.peerId = static_cast<api::chatTypeBasicGroup *>(type)->basic_group_id_;
        break;
      case api::chatTypeSupergroup::ID: {
        const auto group = static_cast<api::chatTypeSupergroup *>(type);
        chat.kind = group->is_channel_ ? ChatKind::Channel : ChatKind::Group;
        chat.peerId = group->supergroup_id_;
        break;
      }
      case api::chatTypeSecret::ID:
        chat.kind = ChatKind::Secret;
        chat.peerId = static_cast<api::chatTypeSecret *>(type)->user_id_;
        break;
      default: break;
    }
  }
  setLastMessage(chat, source.last_message_.get());
  auto &stored = _chats.insert_or_assign(chat.id, std::move(chat)).first->second;
  setPositions(stored, source.positions_);
  chatChanged(stored.id);
}

void Store::addUser(api::user &source) {
  auto &user = _users[source.id_];
  user.id = source.id_;
  user.firstName = source.first_name_;
  user.lastName = source.last_name_;
  user.username = source.usernames_ && !source.usernames_->active_usernames_.empty()
      ? source.usernames_->active_usernames_.front()
      : std::string();
  user.photo = source.profile_photo_ && source.profile_photo_->small_ ? source.profile_photo_->small_->id_ : 0;
  user.accentColor = source.accent_color_id_;
  user.premium = source.is_premium_;
  user.bot = source.type_ && source.type_->get_id() == api::userTypeBot::ID;
  user.deleted = source.type_ && source.type_->get_id() == api::userTypeDeleted::ID;
  user.onlineUntil = 0;
  user.lastSeen = 0;
  if (const auto online = As<api::userStatusOnline>(source.status_.get())) user.onlineUntil = online->expires_;
  if (const auto offline = As<api::userStatusOffline>(source.status_.get())) user.lastSeen = offline->was_online_;
  notify(_userHandlers, user.id);
}

void Store::addFile(const api::file &source) {
  auto &file = _files[source.id_];
  file.id = source.id_;
  file.size = source.size_ ? source.size_ : source.expected_size_;
  if (source.local_) {
    file.downloaded = source.local_->downloaded_size_;
    file.downloading = source.local_->is_downloading_active_;
    file.complete = source.local_->is_downloading_completed_;
    file.path = file.complete ? source.local_->path_ : std::string();
  }
  notify(_fileHandlers, file);
}

void Store::updated(api::Update &update) {
  const auto withChat = [&](ChatId id, auto apply) {
    const auto it = _chats.find(id);
    if (it == _chats.end()) return;  // TDLib always sends updateNewChat first.
    apply(it->second);
    chatChanged(id);
  };
  switch (update.get_id()) {
    case api::updateNewChat::ID:
      addChat(*static_cast<api::updateNewChat &>(update).chat_);
      break;
    case api::updateChatTitle::ID: {
      auto &data = static_cast<api::updateChatTitle &>(update);
      withChat(data.chat_id_, [&](Chat &chat) { chat.title = data.title_; });
      break;
    }
    case api::updateChatPhoto::ID: {
      auto &data = static_cast<api::updateChatPhoto &>(update);
      withChat(data.chat_id_, [&](Chat &chat) { chat.photo = PhotoOf(data.photo_); });
      break;
    }
    case api::updateChatLastMessage::ID: {
      auto &data = static_cast<api::updateChatLastMessage &>(update);
      withChat(data.chat_id_, [&](Chat &chat) {
        setLastMessage(chat, data.last_message_.get());
        setPositions(chat, data.positions_);
      });
      break;
    }
    case api::updateChatPosition::ID: {
      auto &data = static_cast<api::updateChatPosition &>(update);
      withChat(data.chat_id_, [&](Chat &chat) { setPosition(chat, *data.position_); });
      break;
    }
    case api::updateChatDraftMessage::ID: {
      auto &data = static_cast<api::updateChatDraftMessage &>(update);
      withChat(data.chat_id_, [&](Chat &chat) {
        chat.draft = DraftText(data.draft_message_);
        setPositions(chat, data.positions_);
      });
      break;
    }
    case api::updateChatReadInbox::ID: {
      auto &data = static_cast<api::updateChatReadInbox &>(update);
      withChat(data.chat_id_, [&](Chat &chat) {
        chat.readInbox = data.last_read_inbox_message_id_;
        chat.unread = data.unread_count_;
      });
      break;
    }
    case api::updateChatReadOutbox::ID: {
      auto &data = static_cast<api::updateChatReadOutbox &>(update);
      withChat(data.chat_id_, [&](Chat &chat) { chat.readOutbox = data.last_read_outbox_message_id_; });
      break;
    }
    case api::updateChatUnreadMentionCount::ID: {
      auto &data = static_cast<api::updateChatUnreadMentionCount &>(update);
      withChat(data.chat_id_, [&](Chat &chat) { chat.unreadMentions = data.unread_mention_count_; });
      break;
    }
    case api::updateChatIsMarkedAsUnread::ID: {
      auto &data = static_cast<api::updateChatIsMarkedAsUnread &>(update);
      withChat(data.chat_id_, [&](Chat &chat) { chat.markedUnread = data.is_marked_as_unread_; });
      break;
    }
    case api::updateChatNotificationSettings::ID: {
      auto &data = static_cast<api::updateChatNotificationSettings &>(update);
      withChat(data.chat_id_, [&](Chat &chat) { chat.muted = IsMuted(*data.notification_settings_); });
      break;
    }
    case api::updateUser::ID:
      addUser(*static_cast<api::updateUser &>(update).user_);
      break;
    case api::updateUserStatus::ID: {
      auto &data = static_cast<api::updateUserStatus &>(update);
      const auto it = _users.find(data.user_id_);
      if (it == _users.end()) break;
      it->second.onlineUntil = 0;
      it->second.lastSeen = 0;
      if (const auto online = As<api::userStatusOnline>(data.status_.get())) it->second.onlineUntil = online->expires_;
      if (const auto offline = As<api::userStatusOffline>(data.status_.get())) it->second.lastSeen = offline->was_online_;
      notify(_userHandlers, data.user_id_);
      break;
    }
    case api::updateFile::ID:
      addFile(*static_cast<api::updateFile &>(update).file_);
      break;
    case api::updateUnreadChatCount::ID: {
      auto &data = static_cast<api::updateUnreadChatCount &>(update);
      if (!data.chat_list_) break;
      const auto id = data.chat_list_->get_id();
      if (id == api::chatListMain::ID) _lists[int(ListKind::Main)].unreadChats = data.unread_count_;
      if (id == api::chatListArchive::ID) _lists[int(ListKind::Archive)].unreadChats = data.unread_count_;
      break;
    }
    case api::updateOption::ID: {
      auto &data = static_cast<api::updateOption &>(update);
      if (data.name_ == "my_id") {
        if (const auto value = As<api::optionValueInteger>(data.value_.get())) _selfId = value->value_;
      }
      break;
    }
    case api::updateConnectionState::ID:
      _connection = std::move(static_cast<api::updateConnectionState &>(update).state_);
      notify(_connectionHandlers);
      break;
    default: break;
  }
}

}  // namespace Contrail::Td
