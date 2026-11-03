#include "notifications.hpp"

#include "local_state.hpp"
#include "main_window.hpp"
#include "td/message_text.hpp"
#include "td/strings.hpp"

#include <algorithm>

namespace Contrail {
namespace {

namespace api = Td::api;

constexpr auto kService = "org.freedesktop.Notifications";
constexpr auto kPath = "/org/freedesktop/Notifications";
// TDLib keeps this many chats with notifications, and this many newest messages of each.
constexpr auto kGroupsMax = 25;
constexpr auto kGroupSizeMax = 5;
// The newest messages shown in one notification: a phone shows two or three lines anyway.
constexpr auto kLinesShown = 3;
constexpr auto kDismissed = 2u;  // NotificationClosed: the user closed it.

template <typename Type>
const Type *Cast(const api::Object *object) {
  return object && object->get_id() == Type::ID ? static_cast<const Type *>(object) : nullptr;
}

}  // namespace

Notifier::Notifier(Td::Store &store, MainWindow &window) : _store(store), _window(window) {
  auto &client = _store.client();
  // Without these TDLib sends no notifications at all.
  client.send(api::make_object<api::setOption>("notification_group_count_max",
                                               api::make_object<api::optionValueInteger>(kGroupsMax)));
  client.send(api::make_object<api::setOption>("notification_group_size_max",
                                               api::make_object<api::optionValueInteger>(kGroupSizeMax)));
  client.subscribe([this](api::Update &update) { updated(update); }, _lifetime);

  _bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
  if (!_bus) return;
  _cancellable = g_cancellable_new();
  _subscription = g_dbus_connection_signal_subscribe(
      _bus, kService, kService, nullptr, kPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
      +[](GDBusConnection *, const char *, const char *, const char *, const char *name, GVariant *parameters,
          gpointer data) { static_cast<Notifier *>(data)->signal(name, parameters); },
      this, nullptr);
  g_dbus_connection_call(
      _bus, kService, kPath, kService, "GetCapabilities", nullptr, G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, -1,
      _cancellable,
      +[](GObject *bus, GAsyncResult *result, gpointer data) {
        auto *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(bus), result, nullptr);
        // No server, or the notifier has gone (cancelled): nothing to remember.
        if (!reply) return;
        auto *self = static_cast<Notifier *>(data);
        GVariantIter *list = nullptr;
        g_variant_get(reply, "(as)", &list);
        const char *capability = nullptr;
        while (g_variant_iter_next(list, "&s", &capability)) {
          if (g_str_equal(capability, "inline-reply")) self->_canReply = true;
          if (g_str_equal(capability, "body-markup")) self->_markup = true;
        }
        g_variant_iter_free(list);
        g_variant_unref(reply);
      },
      this);
}

Notifier::~Notifier() {
  // Their buttons would lead nowhere: Telegram Desktop takes its notifications away when it quits too.
  closeAll();
  if (_bus) {
    g_cancellable_cancel(_cancellable);
    g_dbus_connection_signal_unsubscribe(_bus, _subscription);
    g_dbus_connection_flush_sync(_bus, nullptr, nullptr);
    g_object_unref(_cancellable);
    g_object_unref(_bus);
  }
}

void Notifier::updated(api::Update &update) {
  switch (update.get_id()) {
    case api::updateNotificationGroup::ID: groupUpdated(static_cast<api::updateNotificationGroup &>(update)); break;
    case api::updateNotification::ID: noteUpdated(static_cast<api::updateNotification &>(update)); break;
    case api::updateActiveNotifications::ID: adopt(static_cast<api::updateActiveNotifications &>(update)); break;
    default: break;
  }
}

Notifier::Note Notifier::makeNote(const api::notification &notification) const {
  auto result = Note{.id = notification.id_, .silent = notification.is_silent_};
  if (const auto type = Cast<api::notificationTypeNewMessage>(notification.type_.get()); type && type->message_) {
    const auto &message = *type->message_;
    result.message = message.id_;
    result.topic = Td::MessageTopic(message);
    result.sender = Td::SenderPrefix(message, _store);
    result.text = Td::MessagePreview(message, _store);
    result.preview = type->show_preview_;
  }
  return result;
}

// New messages and mentions; calls and secret chats have notifications of their own (calls.cpp) or none.
void Notifier::groupUpdated(api::updateNotificationGroup &update) {
  const auto type = update.type_ ? update.type_->get_id() : 0;
  if (type != api::notificationGroupTypeMessages::ID && type != api::notificationGroupTypeMentions::ID) return;
  auto &group = _groups[update.notification_group_id_];
  group.id = update.notification_group_id_;
  group.chat = update.chat_id_;
  group.sound = update.notification_sound_id_;
  const auto &removed = update.removed_notification_ids_;
  std::erase_if(group.notes, [&](const Note &note) { return std::ranges::find(removed, note.id) != removed.end(); });
  for (const auto &added : update.added_notifications_) {
    if (added) group.notes.push_back(makeNote(*added));
  }
  if (group.notes.size() > std::size_t(kGroupSizeMax)) group.notes.erase(group.notes.begin(), group.notes.end() - kGroupSizeMax);
  if (group.notes.empty()) {
    close(group);
    _groups.erase(group.id);
    return;
  }
  if (!update.added_notifications_.empty()) {
    group.adopted = false;
    if (wanted(group)) show(group, true);
  } else if (group.shown || group.pending) {
    // Some were read: the newest that are left.
    show(group, false);
  }
}

// A message was edited: its line changes, without a sound.
void Notifier::noteUpdated(api::updateNotification &update) {
  const auto it = _groups.find(update.notification_group_id_);
  if (it == _groups.end() || !update.notification_) return;
  auto &group = it->second;
  for (auto &note : group.notes) {
    if (note.id == update.notification_->id_) note = makeNote(*update.notification_);
  }
  if (group.shown || group.pending) show(group, false);
}

// What was there when the app quit last time. Telegram Desktop doesn't show those again after a start; they are kept
// so that a new message in the chat shows the others with it.
void Notifier::adopt(api::updateActiveNotifications &update) {
  for (const auto &source : update.groups_) {
    if (!source || !source->type_) continue;
    const auto type = source->type_->get_id();
    if (type != api::notificationGroupTypeMessages::ID && type != api::notificationGroupTypeMentions::ID) continue;
    auto &group = _groups[source->id_];
    group = Group{.id = source->id_, .chat = source->chat_id_, .adopted = true};
    for (const auto &notification : source->notifications_) {
      if (notification) group.notes.push_back(makeNote(*notification));
    }
  }
}

// Nothing while the user reads the chat already, or doesn't want notifications at all.
bool Notifier::wanted(const Group &group) const {
  return Settings().notify && !group.adopted && !(_window.isActive() && _window.currentChat() == group.chat);
}

void Notifier::show(Group &group, bool sound) {
  if (!_bus) return;
  if (group.pending) {
    group.again = true;
    group.againSound = group.againSound || sound;
    return;
  }
  const auto *chat = _store.chat(group.chat);
  const auto summary = chat ? chat->title : std::string("Contrail");
  auto body = std::string();
  const auto preview = Settings().preview
      && std::ranges::all_of(group.notes, [](const Note &note) { return note.preview; });
  if (preview) {
    const auto from = group.notes.size() > std::size_t(kLinesShown) ? group.notes.end() - kLinesShown : group.notes.begin();
    for (auto i = from; i != group.notes.end(); ++i) {
      if (!body.empty()) body += '\n';
      body += (i->sender.empty() ? std::string() : i->sender + ": ") + i->text;
    }
  } else {
    body = Tr("lng_notification_preview");
  }
  if (_markup) body = Escape(body);

  auto *actions = g_variant_builder_new(G_VARIANT_TYPE("as"));
  const auto action = [&](const char *key, const std::string &label) {
    g_variant_builder_add(actions, "s", key);
    g_variant_builder_add(actions, "s", label.c_str());
  };
  action("default", "");
  action("mark-read", Tr("lng_context_mark_read"));
  if (_canReply) action("inline-reply", Tr("lng_notification_reply"));

  auto *hints = g_variant_builder_new(G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(hints, "{sv}", "desktop-entry", g_variant_new_string(kAppId));
  g_variant_builder_add(hints, "{sv}", "category", g_variant_new_string("im.received"));
  g_variant_builder_add(hints, "{sv}", "urgency", g_variant_new_byte(1));
  if (sound && Settings().sound && group.sound && !group.notes.back().silent) {
    g_variant_builder_add(hints, "{sv}", "sound-name", g_variant_new_string("message-new-instant"));
  } else {
    g_variant_builder_add(hints, "{sv}", "suppress-sound", g_variant_new_boolean(TRUE));
  }
  if (chat && chat->photo) {
    if (const auto *file = _store.file(chat->photo); file && file->complete) {
      g_variant_builder_add(hints, "{sv}", "image-path", g_variant_new_string(file->path.c_str()));
    } else {
      _store.download(chat->photo);
    }
  }
  group.pending = true;
  struct Call {
    Notifier *self = nullptr;
    std::int32_t group = 0;
  };
  g_dbus_connection_call(
      _bus, kService, kPath, kService, "Notify",
      g_variant_new("(susssasa{sv}i)", "Contrail", group.shown, kAppId, summary.c_str(), body.c_str(), actions, hints,
                    -1),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, _cancellable,
      +[](GObject *bus, GAsyncResult *result, gpointer data) {
        const auto call = std::unique_ptr<Call>(static_cast<Call *>(data));
        GError *error = nullptr;
        auto *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(bus), result, &error);
        if (!reply) {
          const auto cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
          if (!cancelled) g_message("notify: not shown: %s", error->message);
          g_error_free(error);
          if (!cancelled) call->self->shown(call->group, 0);
          return;
        }
        auto id = guint32();
        g_variant_get(reply, "(u)", &id);
        g_variant_unref(reply);
        call->self->shown(call->group, id);
      },
      new Call{this, group.id});
  g_variant_builder_unref(actions);
  g_variant_builder_unref(hints);
}

void Notifier::shown(std::int32_t groupId, std::uint32_t id) {
  const auto it = _groups.find(groupId);
  if (it == _groups.end()) {
    // Read while it was being shown.
    if (id) {
      g_dbus_connection_call(_bus, kService, kPath, kService, "CloseNotification", g_variant_new("(u)", id), nullptr,
                             G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
    }
    return;
  }
  auto &group = it->second;
  group.pending = false;
  group.shown = id;
  if (group.again) {
    group.again = false;
    show(group, std::exchange(group.againSound, false));
  }
}

void Notifier::close(Group &group) {
  if (group.shown && _bus) {
    g_dbus_connection_call(_bus, kService, kPath, kService, "CloseNotification", g_variant_new("(u)", group.shown),
                           nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
  }
  group.shown = 0;
  group.again = false;
}

void Notifier::closeAll() {
  for (auto &[id, group] : _groups) close(group);
  _groups.clear();
}

void Notifier::settingsChanged() {
  if (Settings().notify) return;
  for (auto &[id, group] : _groups) {
    close(group);
    // Turned on again, they come back with the next message.
    group.adopted = true;
  }
}

Notifier::Group *Notifier::byShown(std::uint32_t id) {
  if (!id) return nullptr;
  for (auto &[groupId, group] : _groups) {
    if (group.shown == id) return &group;
  }
  return nullptr;
}

void Notifier::signal(const char *name, GVariant *parameters) {
  auto id = guint32();
  if (g_str_equal(name, "ActionInvoked") && g_variant_is_of_type(parameters, G_VARIANT_TYPE("(us)"))) {
    const char *action = nullptr;
    g_variant_get(parameters, "(u&s)", &id, &action);
    activated(id, action);
  } else if (g_str_equal(name, "NotificationReplied") && g_variant_is_of_type(parameters, G_VARIANT_TYPE("(us)"))) {
    const char *text = nullptr;
    g_variant_get(parameters, "(u&s)", &id, &text);
    if (const auto *group = byShown(id)) reply(*group, text);
  } else if (g_str_equal(name, "ActivationToken") && g_variant_is_of_type(parameters, G_VARIANT_TYPE("(us)"))) {
    const char *token = nullptr;
    g_variant_get(parameters, "(u&s)", &id, &token);
    if (byShown(id)) _activationToken = token;
  } else if (g_str_equal(name, "NotificationClosed") && g_variant_is_of_type(parameters, G_VARIANT_TYPE("(uu)"))) {
    auto reason = guint32();
    g_variant_get(parameters, "(uu)", &id, &reason);
    auto *group = byShown(id);
    if (!group) return;
    group->shown = 0;
    // Swiped away: TDLib forgets them too, so that they don't come back with the next message.
    if (reason == kDismissed) {
      _store.client().send(api::make_object<api::removeNotificationGroup>(group->id, group->notes.back().id));
      group->adopted = true;
    }
  }
}

void Notifier::activated(std::uint32_t id, const std::string &action) {
  const auto *found = byShown(id);
  if (!found) return;
  const auto group = *found;
  if (action == "default") open(group);
  else if (action == "mark-read") markRead(group);
}

// The chat at its newest message; the window comes back first if the app was in the background.
void Notifier::open(const Group &group) {
  const auto &newest = group.notes.back();
  _window.present();
  if (!_activationToken.empty() && _window.widget()) {
    gtk_window_set_startup_id(GTK_WINDOW(_window.widget()), _activationToken.c_str());
  }
  _activationToken.clear();
  if (newest.topic) _window.openTopic(group.chat, newest.topic, newest.message);
  else _window.openChat(group.chat, newest.message);
  g_message("notify: opened chat=%lld message=%lld", static_cast<long long>(group.chat),
            static_cast<long long>(newest.message));
}

void Notifier::markRead(const Group &group) {
  _store.markRead(group.chat);
  g_message("notify: read chat=%lld", static_cast<long long>(group.chat));
}

// In a group the answer replies to the newest message, as Telegram Desktop's does.
void Notifier::reply(const Group &group, const std::string &text) {
  if (text.empty()) return;
  const auto &newest = group.notes.back();
  auto topic = newest.topic ? api::make_object<api::messageTopicForum>(newest.topic) : nullptr;
  auto replyTo = newest.sender.empty() || !newest.message
      ? nullptr
      : api::make_object<api::inputMessageReplyToMessage>(newest.message, nullptr, 0, std::string());
  auto content = api::make_object<api::inputMessageText>(
      api::make_object<api::formattedText>(text, std::vector<api::object_ptr<api::textEntity>>()), nullptr, false);
  _store.client().send(api::make_object<api::sendMessage>(group.chat, std::move(topic), std::move(replyTo), nullptr,
                                                          nullptr, std::move(content)));
  g_message("notify: replied chat=%lld", static_cast<long long>(group.chat));
  markRead(group);
}

void Notifier::debugState() const {
  auto line = std::string("notify: enabled=") + (Settings().notify ? "1" : "0") + " reply=" + (_canReply ? "1" : "0")
      + " groups=" + std::to_string(_groups.size());
  for (const auto &[id, group] : _groups) {
    line += " [group=" + std::to_string(id) + " chat=" + std::to_string(group.chat) + " shown="
        + std::to_string(group.shown) + " count=" + std::to_string(group.notes.size())
        + " adopted=" + (group.adopted ? "1" : "0") + "]";
  }
  g_message("%s", line.c_str());
}

void Notifier::debugAction(Td::ChatId chat, const std::string &action, const std::string &text) {
  for (const auto &[id, found] : _groups) {
    if (found.chat != chat) continue;
    const auto group = found;
    if (action == "reply") reply(group, text);
    else if (action == "read") markRead(group);
    else open(group);
    return;
  }
  g_message("notify: nothing from chat %lld", static_cast<long long>(chat));
}

}  // namespace Contrail
