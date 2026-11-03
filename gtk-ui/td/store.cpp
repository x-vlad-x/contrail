#include "td/store.hpp"

#include "td/chat_row.hpp"

#include <glib.h>

#include <algorithm>
#include <limits>
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

// Past a year TDLib takes any value as "until it's turned back on".
constexpr auto kMuteForever = std::numeric_limits<std::int32_t>::max();

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
  group.manageTopics = false;
  group.canPin = false;
  group.pinAsMembers = !channel;
  if (!status) return;
  switch (status->get_id()) {
    case api::chatMemberStatusCreator::ID:
      group.member = static_cast<const api::chatMemberStatusCreator &>(*status).is_member_;
      group.canPost = true;
      group.manageTopics = true;
      group.canPin = true;
      group.pinAsMembers = false;
      break;
    case api::chatMemberStatusAdministrator::ID: {
      const auto &rights = static_cast<const api::chatMemberStatusAdministrator &>(*status).rights_;
      group.canPost = !channel || (rights && rights->can_post_messages_);
      group.manageTopics = rights && rights->can_manage_topics_;
      // In a channel pinning goes with editing messages.
      group.canPin = rights && (channel ? rights->can_edit_messages_ : rights->can_pin_messages_);
      group.pinAsMembers = false;
      break;
    }
    case api::chatMemberStatusRestricted::ID: {
      const auto &restricted = static_cast<const api::chatMemberStatusRestricted &>(*status);
      group.member = restricted.is_member_;
      group.canPost = restricted.permissions_ && restricted.permissions_->can_send_basic_messages_;
      group.canPin = restricted.permissions_ && restricted.permissions_->can_pin_messages_;
      group.pinAsMembers = false;
      break;
    }
    case api::chatMemberStatusLeft::ID:
    case api::chatMemberStatusBanned::ID:
      group.member = false;
      group.canPost = false;
      group.pinAsMembers = false;
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

bool Store::canPin(const Chat &chat) const {
  if (chat.kind == ChatKind::Private) return true;
  if (chat.kind == ChatKind::Secret) return false;
  const auto known = group(chat);
  if (!known) return false;
  return known->pinAsMembers ? chat.canPinAll : known->canPin;
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

void Store::setMuted(ChatId id, bool muted, Failed failed) {
  // The settings also hold the sound and the previews, which the store doesn't keep: only the mute may change.
  _client.request<api::chat>(
      api::make_object<api::getChat>(id), _lifetime,
      [this, id, muted, failed](api::chat &chat) {
        if (!chat.notification_settings_) return;
        auto settings = std::move(chat.notification_settings_);
        settings->use_default_mute_for_ = false;
        settings->mute_for_ = muted ? kMuteForever : 0;
        _client.request<api::ok>(api::make_object<api::setChatNotificationSettings>(id, std::move(settings)), _lifetime,
                                 [](api::ok &) {}, failed);
      },
      failed);
}

void Store::markRead(ChatId id, Failed failed) {
  const auto found = chat(id);
  if (!found) return;
  const auto send = [&](FunctionPtr function) {
    _client.request<api::ok>(std::move(function), _lifetime, [](api::ok &) {}, failed);
  };
  if (found->lastMessageId) {
    auto messages = std::vector<std::int64_t>{found->lastMessageId};
    send(api::make_object<api::viewMessages>(id, std::move(messages), api::make_object<api::messageSourceChatList>(), true));
  }
  if (found->unreadMentions > 0) send(api::make_object<api::readAllChatMentions>(id));
  if (found->unreadReactions > 0) send(api::make_object<api::readAllChatReactions>(id));
  if (found->markedUnread) send(api::make_object<api::toggleChatIsMarkedAsUnread>(id, false));
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
  for (auto &[id, chat] : _chats) {
    if (chat.peerId != peerId || chat.supergroup != supergroup) continue;
    if (chat.kind != ChatKind::Group && chat.kind != ChatKind::Channel) continue;
    chat.forum = supergroup && _supergroups[peerId].forum;
    chatChanged(id);
    return;
  }
}

void Store::chatChanged(ChatId id) {
  // Topics without settings of their own are muted with the group.
  if (const auto held = forum(id)) {
    const auto &chat = _chats.at(id);
    auto changed = std::vector<std::int32_t>();
    for (auto &[topicId, topic] : held->topics) {
      if (!topic.row.defaultMute || topic.row.muted == chat.muted) continue;
      topic.row.muted = chat.muted;
      changed.push_back(topicId);
    }
    for (const auto topicId : changed) notify(_topicHandlers, id, topicId);
  }
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

void Store::setLastMessage(Chat &chat, const api::message *message) {
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
  chat.canPinAll = source.permissions_ && source.permissions_->can_pin_messages_;
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
        if (const auto known = _supergroups.find(chat.peerId); known != _supergroups.end()) {
          chat.forum = known->second.forum;
        }
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
  if (!_forums.empty()) topicsUpdated(update);
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
      known.forum = group.is_forum_;
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
        chat.canPinAll = data.permissions_ && data.permissions_->can_pin_messages_;
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
      if (id != api::chatListMain::ID && id != api::chatListArchive::ID) break;
      const auto kind = id == api::chatListMain::ID ? ListKind::Main : ListKind::Archive;
      _lists[int(kind)].unreadChats = data.unread_count_;
      // The order is the same, so nothing to sort: the archive's row only shows this number.
      notify(_listHandlers, kind);
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

// --- topics of groups with topics ------------------------------------------------

std::int32_t MessageTopic(const api::message &message) {
  const auto topic = As<api::messageTopicForum>(message.topic_id_.get());
  return topic ? topic->forum_topic_id_ : 0;
}

Store::Forum *Store::forum(ChatId chat) {
  const auto it = _forums.find(chat);
  return it == _forums.end() ? nullptr : &it->second;
}

std::shared_ptr<void> Store::holdTopics(ChatId chat) {
  auto &held = _forums[chat];
  if (auto holder = held.holder.lock()) return holder;
  // The holders are views, which go before the store does; this checks anyway.
  const auto token = _lifetime.token();
  auto holder = std::shared_ptr<void>(new char(0), [this, token, chat](void *pointer) {
    delete static_cast<char *>(pointer);
    if (!token.expired()) dropForum(chat);
  });
  held.holder = holder;
  return holder;
}

void Store::dropForum(ChatId chat) {
  _forums.erase(chat);
}

const Topic *Store::topic(ChatId chat, std::int32_t id) const {
  const auto held = _forums.find(chat);
  if (held == _forums.end()) return nullptr;
  const auto it = held->second.topics.find(id);
  return it == held->second.topics.end() ? nullptr : &it->second;
}

const std::vector<std::int32_t> &Store::topics(ChatId chat) {
  static const auto empty = std::vector<std::int32_t>();
  const auto held = forum(chat);
  if (!held) return empty;
  if (held->dirty) {
    held->dirty = false;
    held->order.clear();
    for (const auto &[id, topic] : held->topics) held->order.push_back(id);
    std::ranges::sort(held->order, [&](std::int32_t a, std::int32_t b) {
      const auto &left = held->topics.at(a);
      const auto &right = held->topics.at(b);
      if (left.pinned != right.pinned) return left.pinned;
      return left.order != right.order ? left.order > right.order : a > b;
    });
  }
  return held->order;
}

bool Store::topicsLoaded(ChatId chat) const {
  const auto held = _forums.find(chat);
  return held != _forums.end() && held->second.loaded;
}

bool Store::topicsComplete(ChatId chat) const {
  const auto held = _forums.find(chat);
  return held != _forums.end() && held->second.complete;
}

void Store::loadTopics(ChatId chat) {
  const auto held = forum(chat);
  if (!held || held->loading || held->complete) return;
  held->loading = true;
  const auto finished = [this, chat](bool complete) {
    const auto held = forum(chat);
    if (!held) return;
    held->loading = false;
    held->loaded = true;
    held->complete = held->complete || complete;
    held->dirty = true;
    notify(_topicHandlers, chat, std::int32_t(0));
  };
  _client.request<api::forumTopics>(
      api::make_object<api::getForumTopics>(chat, std::string(), held->nextDate, held->nextMessage, held->nextTopic, 100),
      _lifetime,
      [this, chat, finished](api::forumTopics &answer) {
        const auto held = forum(chat);
        if (!held) return;
        for (auto &topic : answer.topics_) {
          if (topic && topic->info_) setTopic(*held, *topic);
        }
        held->nextDate = answer.next_offset_date_;
        held->nextMessage = answer.next_offset_message_id_;
        held->nextTopic = answer.next_offset_forum_topic_id_;
        const auto end = answer.topics_.empty() || int(held->topics.size()) >= answer.total_count_
            || (!held->nextDate && !held->nextMessage && !held->nextTopic);
        finished(end);
      },
      [this, finished](api::error &error) {
        _client.reportError(error);
        finished(true);
      });
}

void Store::setTopic(Forum &held, api::forumTopic &source) {
  const auto id = source.info_->forum_topic_id_;
  const auto added = !held.topics.contains(id);
  auto &topic = held.topics[id];
  const auto before = std::pair(topic.pinned, topic.order);
  topic.id = id;
  topic.row.id = source.info_->chat_id_;
  topic.row.kind = ChatKind::Group;
  setTopicInfo(topic, *source.info_);
  topic.order = source.order_;
  topic.pinned = source.is_pinned_;
  topic.row.unread = source.unread_count_;
  topic.row.readInbox = source.last_read_inbox_message_id_;
  topic.row.readOutbox = source.last_read_outbox_message_id_;
  topic.row.unreadMentions = source.unread_mention_count_;
  topic.row.unreadReactions = source.unread_reaction_count_;
  topic.row.draft = DraftText(source.draft_message_);
  topic.row.draftReplyTo = DraftReplyTo(source.draft_message_);
  setLastMessage(topic.row, source.last_message_.get());
  if (const auto group = chat(topic.row.id); group && source.notification_settings_) {
    setTopicSettings(topic, *group, *source.notification_settings_);
  }
  if (added || before != std::pair(topic.pinned, topic.order)) held.dirty = true;
}

void Store::setTopicInfo(Topic &topic, const api::forumTopicInfo &info) {
  topic.row.title = info.name_;
  topic.general = info.is_general_;
  topic.closed = info.is_closed_;
}

void Store::setTopicSettings(Topic &topic, const Chat &group, const api::chatNotificationSettings &settings) {
  topic.row.defaultMute = settings.use_default_mute_for_;
  topic.row.muted = topic.row.defaultMute ? group.muted : IsMuted(settings);
}

void Store::topicChanged(ChatId chat, std::int32_t id) {
  const auto held = forum(chat);
  const auto reordered = held && held->dirty;
  notify(_topicHandlers, chat, id);
  if (reordered) notify(_topicHandlers, chat, std::int32_t(0));
}

void Store::refreshTopic(ChatId chat, std::int32_t id, std::function<void()> done) {
  const auto held = forum(chat);
  if (!held) {
    if (done) done();
    return;
  }
  if (const auto it = held->asked.find(id); it != held->asked.end()) {
    if (done) it->second.push_back(std::move(done));
    held->askAgain.insert(id);
    return;
  }
  auto &waiting = held->asked[id];
  if (done) waiting.push_back(std::move(done));
  // What waits runs last: it may let the group go.
  const auto answered = [this, chat, id](auto apply) {
    const auto held = forum(chat);
    if (!held) return;
    auto waiting = std::move(held->asked[id]);
    held->asked.erase(id);
    const auto again = held->askAgain.erase(id) > 0;
    apply(*held);
    topicChanged(chat, id);
    for (const auto &callback : waiting) callback();
    if (again) refreshTopic(chat, id);
  };
  _client.request<api::forumTopic>(
      api::make_object<api::getForumTopic>(chat, id), _lifetime,
      [this, answered](api::forumTopic &topic) {
        answered([&](Forum &held) {
          if (topic.info_) setTopic(held, topic);
        });
      },
      [answered, id](api::error &error) {
        // The topic was deleted.
        answered([&](Forum &held) {
          if (error.code_ == 400 && held.topics.erase(id)) held.dirty = true;
        });
      });
}

void Store::topicMessage(ChatId chat, const api::message &message) {
  const auto held = forum(chat);
  if (!held) return;
  auto id = MessageTopic(message);
  if (!id) {
    // A message without a topic in a group with topics is one of General's.
    const auto general = std::ranges::find_if(held->topics, [](const auto &pair) { return pair.second.general; });
    if (general == held->topics.end()) return;
    id = general->first;
  }
  const auto it = held->topics.find(id);
  if (it != held->topics.end()) {
    // Shown at once; the order and the count come with the topic asked for again.
    auto &row = it->second.row;
    if (message.id_ >= row.lastMessageId) setLastMessage(row, &message);
    if (!message.is_outgoing_ && message.id_ > row.readInbox && !message.sending_state_) ++row.unread;
    topicChanged(chat, id);
  }
  refreshTopic(chat, id);
}

void Store::topicsUpdated(api::Update &update) {
  switch (update.get_id()) {
    case api::updateForumTopicInfo::ID: {
      const auto &info = static_cast<api::updateForumTopicInfo &>(update).info_;
      const auto held = info ? forum(info->chat_id_) : nullptr;
      if (!held) break;
      if (const auto it = held->topics.find(info->forum_topic_id_); it != held->topics.end()) {
        setTopicInfo(it->second, *info);
        topicChanged(info->chat_id_, info->forum_topic_id_);
      } else if (held->loaded) {
        refreshTopic(info->chat_id_, info->forum_topic_id_);  // A new topic.
      }
      break;
    }
    case api::updateForumTopic::ID: {
      auto &data = static_cast<api::updateForumTopic &>(update);
      const auto held = forum(data.chat_id_);
      if (!held) break;
      const auto it = held->topics.find(data.forum_topic_id_);
      if (it == held->topics.end()) break;
      auto &topic = it->second;
      if (topic.pinned != data.is_pinned_) held->dirty = true;
      topic.pinned = data.is_pinned_;
      const auto read = topic.row.readInbox != data.last_read_inbox_message_id_;
      topic.row.readInbox = data.last_read_inbox_message_id_;
      topic.row.readOutbox = data.last_read_outbox_message_id_;
      topic.row.unreadMentions = data.unread_mention_count_;
      topic.row.unreadReactions = data.unread_reaction_count_;
      topic.row.draft = DraftText(data.draft_message_);
      topic.row.draftReplyTo = DraftReplyTo(data.draft_message_);
      if (const auto group = chat(data.chat_id_); group && data.notification_settings_) {
        setTopicSettings(topic, *group, *data.notification_settings_);
      }
      // Nothing tells how many are left unread.
      if (read && topic.row.readInbox >= topic.row.lastMessageId) topic.row.unread = 0;
      topicChanged(data.chat_id_, data.forum_topic_id_);
      if (read && topic.row.unread) refreshTopic(data.chat_id_, data.forum_topic_id_);
      break;
    }
    case api::updateNewMessage::ID: {
      const auto &message = static_cast<api::updateNewMessage &>(update).message_;
      if (message && !message->scheduling_state_) topicMessage(message->chat_id_, *message);
      break;
    }
    case api::updateMessageSendSucceeded::ID:
    case api::updateMessageSendFailed::ID: {
      const auto &message = update.get_id() == api::updateMessageSendSucceeded::ID
          ? static_cast<api::updateMessageSendSucceeded &>(update).message_
          : static_cast<api::updateMessageSendFailed &>(update).message_;
      const auto topic = message ? MessageTopic(*message) : 0;
      if (topic && forum(message->chat_id_)) refreshTopic(message->chat_id_, topic);
      break;
    }
    case api::updateMessageContent::ID:
    case api::updateDeleteMessages::ID: {
      // The last message of a topic changed or went: its row shows another one.
      auto chat = ChatId(0);
      auto ids = std::vector<std::int64_t>();
      if (update.get_id() == api::updateMessageContent::ID) {
        const auto &data = static_cast<api::updateMessageContent &>(update);
        chat = data.chat_id_;
        ids.push_back(data.message_id_);
      } else {
        const auto &data = static_cast<api::updateDeleteMessages &>(update);
        if (!data.is_permanent_) break;
        chat = data.chat_id_;
        ids = data.message_ids_;
      }
      const auto held = forum(chat);
      if (!held) break;
      auto changed = std::vector<std::int32_t>();
      for (const auto &[id, topic] : held->topics) {
        if (topic.row.lastMessageId && std::ranges::count(ids, topic.row.lastMessageId)) changed.push_back(id);
      }
      for (const auto id : changed) refreshTopic(chat, id);
      break;
    }
    default: break;
  }
}

void Store::setTopicMuted(ChatId chat, std::int32_t id, bool muted, Failed failed) {
  // As with a chat: only the mute may change of the settings.
  _client.request<api::forumTopic>(
      api::make_object<api::getForumTopic>(chat, id), _lifetime,
      [this, chat, id, muted, failed](api::forumTopic &topic) {
        if (!topic.notification_settings_) return;
        auto settings = std::move(topic.notification_settings_);
        settings->use_default_mute_for_ = false;
        settings->mute_for_ = muted ? kMuteForever : 0;
        _client.request<api::ok>(api::make_object<api::setForumTopicNotificationSettings>(chat, id, std::move(settings)),
                                 _lifetime, [](api::ok &) {}, failed);
      },
      failed);
}

void Store::markTopicRead(ChatId chat, std::int32_t id, Failed failed) {
  const auto found = topic(chat, id);
  if (!found) return;
  const auto send = [&](FunctionPtr function) {
    _client.request<api::ok>(std::move(function), _lifetime, [](api::ok &) {}, failed);
  };
  if (found->row.lastMessageId) {
    auto messages = std::vector<std::int64_t>{found->row.lastMessageId};
    send(api::make_object<api::viewMessages>(chat, std::move(messages),
                                             api::make_object<api::messageSourceForumTopicHistory>(), true));
  }
  if (found->row.unreadMentions > 0) send(api::make_object<api::readAllForumTopicMentions>(chat, id));
  if (found->row.unreadReactions > 0) send(api::make_object<api::readAllForumTopicReactions>(chat, id));
}

void Store::onTopic(TopicHandler handler, const Lifetime &lifetime) {
  _topicHandlers.push_back({std::move(handler), lifetime.token()});
}

}  // namespace Contrail::Td
