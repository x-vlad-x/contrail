#include "td/store.hpp"

#include "td/chat_row.hpp"

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

template <typename Object>
const Object *As(const api::Object *object) {
  return object && object->get_id() == Object::ID ? static_cast<const Object *>(object) : nullptr;
}

FileId PhotoOf(const api::object_ptr<api::chatPhotoInfo> &photo) {
  return photo && photo->small_ ? photo->small_->id_ : 0;
}

int ScopeOf(ChatKind kind) {
  return kind == ChatKind::Channel ? 2 : kind == ChatKind::Group ? 1 : 0;
}

// Messages people write, as opposed to what Telegram reports in a chat. New kinds count as reports.
bool IsServiceContent(const api::MessageContent *content) {
  if (!content) return false;
  switch (content->get_id()) {
    case api::messageText::ID:
    case api::messageAnimation::ID:
    case api::messageAudio::ID:
    case api::messageDocument::ID:
    case api::messagePhoto::ID:
    case api::messageSticker::ID:
    case api::messageVideo::ID:
    case api::messageVideoNote::ID:
    case api::messageVoiceNote::ID:
    case api::messageExpiredPhoto::ID:
    case api::messageExpiredVideo::ID:
    case api::messageExpiredVideoNote::ID:
    case api::messageExpiredVoiceNote::ID:
    case api::messageLocation::ID:
    case api::messageVenue::ID:
    case api::messageContact::ID:
    case api::messageAnimatedEmoji::ID:
    case api::messageDice::ID:
    case api::messageGame::ID:
    case api::messagePoll::ID:
    case api::messageStory::ID:
    case api::messageInvoice::ID:
    case api::messageUnsupported::ID:
      return false;
    default:
      return true;
  }
}

std::int64_t DraftReplyTo(const api::object_ptr<api::draftMessage> &draft) {
  const auto reply = draft ? As<api::inputMessageReplyToMessage>(draft->reply_to_.get()) : nullptr;
  return reply ? reply->message_id_ : 0;
}

std::int32_t LastSeen(api::UserStatus *status) {
  if (const auto offline = As<api::userStatusOffline>(status)) return offline->was_online_;
  if (As<api::userStatusRecently>(status)) return -1;
  if (As<api::userStatusLastWeek>(status)) return -2;
  if (As<api::userStatusLastMonth>(status)) return -3;
  return 0;
}

// What this account may do, from its status in a group or a channel.
void SetGroupStatus(Group &group, const api::ChatMemberStatus *status, bool channel) {
  group.member = true;
  group.canPost = !channel;
  if (!status) return;
  switch (status->get_id()) {
    case api::chatMemberStatusCreator::ID:
      group.member = static_cast<const api::chatMemberStatusCreator &>(*status).is_member_;
      group.canPost = true;
      break;
    case api::chatMemberStatusAdministrator::ID: {
      const auto &rights = static_cast<const api::chatMemberStatusAdministrator &>(*status).rights_;
      group.canPost = !channel || (rights && rights->can_post_messages_);
      break;
    }
    case api::chatMemberStatusRestricted::ID: {
      const auto &restricted = static_cast<const api::chatMemberStatusRestricted &>(*status);
      group.member = restricted.is_member_;
      group.canPost = restricted.permissions_ && restricted.permissions_->can_send_basic_messages_;
      break;
    }
    case api::chatMemberStatusLeft::ID:
    case api::chatMemberStatusBanned::ID:
      group.member = false;
      group.canPost = false;
      break;
    default: break;
  }
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

std::string Store::username(const Chat &chat) const {
  if (chat.kind == ChatKind::Private) {
    const auto found = user(chat.peerId);
    return found ? found->username : std::string();
  }
  const auto it = chat.supergroup ? _groupUsernames.find(chat.peerId) : _groupUsernames.end();
  return it == _groupUsernames.end() ? std::string() : it->second;
}

const std::vector<ChatAction> *Store::actions(ChatId id) const {
  const auto it = _actions.find(id);
  return it == _actions.end() ? nullptr : &it->second;
}

const Group *Store::group(const Chat &chat) const {
  if (chat.kind != ChatKind::Group && chat.kind != ChatKind::Channel) return nullptr;
  const auto &groups = chat.supergroup ? _supergroups : _basicGroups;
  const auto it = groups.find(chat.peerId);
  return it == groups.end() ? nullptr : &it->second;
}

bool Store::canWrite(const Chat &chat) const {
  if (chat.kind == ChatKind::Private || chat.kind == ChatKind::Secret) return true;
  const auto known = group(chat);
  if (chat.kind == ChatKind::Channel) return known && known->canPost;
  return chat.canSendBasic && (!known || known->canPost);
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
  // A file already on disk gets no update: the answer is all there is.
  _client.send(api::make_object<api::downloadFile>(id, priority, 0, 0, false),
               [this](ObjectPtr object) {
                 if (const auto file = As<api::file>(object.get())) addFile(*file);
               },
               _lifetime);
}

void Store::cancelDownload(FileId id) {
  _client.send(api::make_object<api::cancelDownloadFile>(id, false), nullptr);
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

// The chat of a group doesn't know of its status: the header and the field ask the store again.
void Store::groupChanged(std::int64_t peerId, bool supergroup) {
  for (const auto &[id, chat] : _chats) {
    if (chat.peerId != peerId || chat.supergroup != supergroup) continue;
    if (chat.kind != ChatKind::Group && chat.kind != ChatKind::Channel) continue;
    chatChanged(id);
    return;
  }
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
  chat.preview = message && _preview ? OneLine(_preview(*message)) : std::string();
  chat.lastDate = message ? message->date_ : 0;
  chat.lastMessageId = message ? message->id_ : 0;
  chat.lastOutgoing = message && message->is_outgoing_;
  const auto state = message ? message->sending_state_.get() : nullptr;
  chat.lastSending = state && state->get_id() == api::messageSendingStatePending::ID;
  chat.lastFailed = state && state->get_id() == api::messageSendingStateFailed::ID;
  chat.lastService = message && IsServiceContent(message->content_.get());
  const auto sender = message ? message->sender_id_.get() : nullptr;
  const auto user = As<api::messageSenderUser>(sender);
  const auto group = As<api::messageSenderChat>(sender);
  chat.lastSenderUser = user ? user->user_id_ : 0;
  chat.lastSenderChat = group ? group->chat_id_ : 0;
}

void Store::setNotificationSettings(Chat &chat, const api::chatNotificationSettings &settings) {
  chat.defaultMute = settings.use_default_mute_for_;
  chat.muted = chat.defaultMute ? _scopeMuted[ScopeOf(chat.kind)] : IsMuted(settings);
}

void Store::setScopeMuted(int scope, bool muted) {
  if (_scopeMuted[scope] == muted) return;
  _scopeMuted[scope] = muted;
  auto changed = std::vector<ChatId>();
  for (auto &[id, chat] : _chats) {
    if (!chat.defaultMute || ScopeOf(chat.kind) != scope) continue;
    chat.muted = muted;
    changed.push_back(id);
  }
  for (const auto id : changed) chatChanged(id);
}

void Store::setAction(api::updateChatAction &update) {
  const auto user = As<api::messageSenderUser>(update.sender_id_.get());
  if (!user || !update.action_ || !_chats.contains(update.chat_id_)) return;
  auto &list = _actions[update.chat_id_];
  std::erase_if(list, [&](const ChatAction &action) { return action.user == user->user_id_; });
  if (update.action_->get_id() != api::chatActionCancel::ID) list.push_back({user->user_id_, update.action_->get_id()});
  if (list.empty()) _actions.erase(update.chat_id_);
  chatChanged(update.chat_id_);
}

void Store::addChat(api::chat &source) {
  auto chat = Chat();
  chat.id = source.id_;
  chat.title = source.title_;
  chat.photo = PhotoOf(source.photo_);
  chat.accentColor = source.accent_color_id_;
  chat.unread = source.unread_count_;
  chat.unreadMentions = source.unread_mention_count_;
  chat.unreadReactions = source.unread_reaction_count_;
  chat.markedUnread = source.is_marked_as_unread_;
  chat.readInbox = source.last_read_inbox_message_id_;
  chat.readOutbox = source.last_read_outbox_message_id_;
  chat.draft = DraftText(source.draft_message_);
  chat.draftReplyTo = DraftReplyTo(source.draft_message_);
  chat.canSendBasic = !source.permissions_ || source.permissions_->can_send_basic_messages_;
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
        chat.supergroup = true;
        break;
      }
      case api::chatTypeSecret::ID:
        chat.kind = ChatKind::Secret;
        chat.peerId = static_cast<api::chatTypeSecret *>(type)->user_id_;
        break;
      default: break;
    }
  }
  if (source.notification_settings_) setNotificationSettings(chat, *source.notification_settings_);
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
  user.lastSeen = LastSeen(source.status_.get());
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
        chat.draftReplyTo = DraftReplyTo(data.draft_message_);
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
    case api::updateChatUnreadReactionCount::ID: {
      auto &data = static_cast<api::updateChatUnreadReactionCount &>(update);
      withChat(data.chat_id_, [&](Chat &chat) { chat.unreadReactions = data.unread_reaction_count_; });
      break;
    }
    case api::updateChatAction::ID:
      setAction(static_cast<api::updateChatAction &>(update));
      break;
    case api::updateScopeNotificationSettings::ID: {
      auto &data = static_cast<api::updateScopeNotificationSettings &>(update);
      if (!data.scope_ || !data.notification_settings_) break;
      const auto id = data.scope_->get_id();
      setScopeMuted(id == api::notificationSettingsScopeChannelChats::ID ? 2
                        : id == api::notificationSettingsScopeGroupChats::ID ? 1
                                                                             : 0,
                    data.notification_settings_->mute_for_ > 0);
      break;
    }
    case api::updateSupergroup::ID: {
      const auto &group = *static_cast<api::updateSupergroup &>(update).supergroup_;
      if (group.usernames_ && !group.usernames_->active_usernames_.empty()) {
        _groupUsernames[group.id_] = group.usernames_->active_usernames_.front();
      } else {
        _groupUsernames.erase(group.id_);
      }
      auto &known = _supergroups[group.id_];
      known.members = group.member_count_;
      SetGroupStatus(known, group.status_.get(), group.is_channel_);
      groupChanged(group.id_, true);
      break;
    }
    case api::updateBasicGroup::ID: {
      const auto &group = *static_cast<api::updateBasicGroup &>(update).basic_group_;
      auto &known = _basicGroups[group.id_];
      known.members = group.member_count_;
      SetGroupStatus(known, group.status_.get(), false);
      groupChanged(group.id_, false);
      break;
    }
    case api::updateChatPermissions::ID: {
      auto &data = static_cast<api::updateChatPermissions &>(update);
      withChat(data.chat_id_, [&](Chat &chat) {
        chat.canSendBasic = !data.permissions_ || data.permissions_->can_send_basic_messages_;
      });
      break;
    }
    case api::updateChatIsMarkedAsUnread::ID: {
      auto &data = static_cast<api::updateChatIsMarkedAsUnread &>(update);
      withChat(data.chat_id_, [&](Chat &chat) { chat.markedUnread = data.is_marked_as_unread_; });
      break;
    }
    case api::updateChatNotificationSettings::ID: {
      auto &data = static_cast<api::updateChatNotificationSettings &>(update);
      withChat(data.chat_id_, [&](Chat &chat) { setNotificationSettings(chat, *data.notification_settings_); });
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
      it->second.lastSeen = LastSeen(data.status_.get());
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
      } else if (data.name_ == "replies_bot_chat_id") {
        if (const auto value = As<api::optionValueInteger>(data.value_.get())) _repliesChatId = value->value_;
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
