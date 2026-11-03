#include "chat_live_locations.hpp"

#include "live_location.hpp"
#include "live_map.hpp"
#include "location_ui.hpp"
#include "main_window.hpp"
#include "scroll_helpers.hpp"
#include "td/dates.hpp"
#include "td/strings.hpp"

#include <algorithm>
#include <utility>

namespace Contrail {
namespace api = Td::api;
namespace {

constexpr auto kRecentLimit = 100;

std::int32_t UpdateDate(const api::message &message) {
  return message.edit_date_ ? message.edit_date_ : message.date_;
}

bool IsLiveLocation(const api::message &message) {
  auto info = LocationInfo();
  return MessageLocation(message, &info) && info.livePeriod > 0;
}

}  // namespace

ChatLiveLocations::ChatLiveLocations(Td::Store &store, Td::ChatId chat) : _store(store), _chat(chat) {
  _store.client().subscribe([this](api::Update &update) { updated(update); }, _lifetime);
  // One started or stopped here: the list is asked for again (a message being sent may have been taken by the open
  // chat before this saw it).
  Live().onChanged([this] { request(); }, _lifetime);
  request();
}

ChatLiveLocations::~ChatLiveLocations() {
  if (_expireTimer) g_source_remove(_expireTimer);
}

bool ChatLiveLocations::isMine(const api::message &message) const {
  if (message.is_outgoing_) return true;
  const auto user = message.sender_id_ && message.sender_id_->get_id() == api::messageSenderUser::ID
                        ? static_cast<const api::messageSenderUser &>(*message.sender_id_).user_id_
                        : 0;
  return user && user == _store.selfId();
}

const api::message *ChatLiveLocations::mine() const {
  const auto it = std::ranges::find_if(_active, [this](const api::message *message) { return isMine(*message); });
  return it == _active.end() ? nullptr : *it;
}

void ChatLiveLocations::request() {
  _requestLifetime.reset();
  _store.client().request<api::messages>(
      api::make_object<api::searchChatRecentLocationMessages>(_chat, kRecentLimit), _requestLifetime,
      [this](api::messages &messages) {
        for (const auto &message : messages.messages_) {
          if (message) consider(*message);
        }
        refresh();
      },
      [](api::error &) {});
}

void ChatLiveLocations::updated(api::Update &update) {
  switch (update.get_id()) {
    case api::updateNewMessage::ID: {
      const auto &message = static_cast<api::updateNewMessage &>(update).message_;
      if (message && message->chat_id_ == _chat && IsLiveLocation(*message)) {
        consider(*message);
        refresh();
      }
      break;
    }
    case api::updateMessageSendSucceeded::ID: {
      auto &data = static_cast<api::updateMessageSendSucceeded &>(update);
      if (!data.message_ || data.message_->chat_id_ != _chat) break;
      std::erase_if(_known, [&](const auto &known) { return known->id_ == data.old_message_id_; });
      if (IsLiveLocation(*data.message_)) consider(*data.message_);
      refresh();
      break;
    }
    case api::updateMessageContent::ID:
    case api::updateMessageEdited::ID: {
      // Moved or stopped. The open chat may have taken the new content already: the message is asked for again
      // (TDLib has it in memory).
      const auto chat = update.get_id() == api::updateMessageEdited::ID
                            ? static_cast<api::updateMessageEdited &>(update).chat_id_
                            : static_cast<api::updateMessageContent &>(update).chat_id_;
      const auto id = update.get_id() == api::updateMessageEdited::ID
                          ? static_cast<api::updateMessageEdited &>(update).message_id_
                          : static_cast<api::updateMessageContent &>(update).message_id_;
      if (chat != _chat || std::ranges::none_of(_known, [&](const auto &known) { return known->id_ == id; })) break;
      _store.client().request<api::message>(api::make_object<api::getMessage>(_chat, id), _lifetime,
                                            [this](api::message &message) {
                                              for (auto &known : _known) {
                                                if (known->id_ == message.id_) known = CopyLocationMessage(message);
                                              }
                                              refresh();
                                            },
                                            [](api::error &) {});
      break;
    }
    case api::updateDeleteMessages::ID: {
      auto &data = static_cast<api::updateDeleteMessages &>(update);
      if (data.chat_id_ != _chat || !data.is_permanent_) break;
      const auto before = _known.size();
      std::erase_if(_known, [&](const auto &known) {
        return std::ranges::find(data.message_ids_, known->id_) != data.message_ids_.end();
      });
      if (_known.size() != before) refresh();
      break;
    }
    default: break;
  }
}

void ChatLiveLocations::consider(const api::message &message) {
  if (!IsLiveLocation(message)) return;
  auto copy = CopyLocationMessage(message);
  for (auto &known : _known) {
    if (known->id_ == message.id_) {
      known = std::move(copy);
      return;
    }
  }
  _known.push_back(std::move(copy));
}

void ChatLiveLocations::refresh() {
  auto active = std::vector<const api::message *>();
  auto next = std::int32_t(0);  // When the first of them ends.
  const auto now = Td::Now();
  for (const auto &known : _known) {
    if (!known->content_ || !LiveLocationActive(*known)) continue;
    const auto sender = [](const api::message &message) -> std::int64_t {
      if (!message.sender_id_) return 0;
      if (message.sender_id_->get_id() == api::messageSenderUser::ID) {
        return static_cast<const api::messageSenderUser &>(*message.sender_id_).user_id_;
      }
      return -static_cast<const api::messageSenderChat &>(*message.sender_id_).chat_id_;
    };
    const auto same = std::ranges::find_if(active, [&](const api::message *other) { return sender(*other) == sender(*known); });
    if (same != active.end()) {
      if ((*same)->date_ < known->date_) *same = known.get();
    } else {
      active.push_back(known.get());
    }
    if (auto info = LocationInfo(); MessageLocation(*known, &info) && info.livePeriod != kUntilOff) {
      const auto end = known->date_ + info.livePeriod;
      if (!next || end < next) next = end;
    }
  }
  std::ranges::sort(active, [this](const api::message *a, const api::message *b) {
    const auto mineA = isMine(*a), mineB = isMine(*b);
    if (mineA != mineB) return mineA;
    return UpdateDate(*a) > UpdateDate(*b);
  });
  if (_expireTimer) g_source_remove(std::exchange(_expireTimer, 0));
  if (next) {
    _expireTimer = g_timeout_add_seconds(guint(std::clamp(next - now + 1, 1, 3600)), +[](gpointer data) -> gboolean {
      auto *self = static_cast<ChatLiveLocations *>(data);
      self->_expireTimer = 0;
      self->refresh();
      return G_SOURCE_REMOVE;
    }, this);
  }
  _active = std::move(active);
  if (_changed) _changed();
}

std::string LiveLocationNames(const Td::Store &store, const ChatLiveLocations &locations) {
  auto names = std::vector<std::string>();
  auto mine = false;
  for (const auto *message : locations.active()) {
    if (locations.isMine(*message)) {
      mine = true;
      continue;
    }
    auto name = std::string();
    if (message->sender_id_ && message->sender_id_->get_id() == api::messageSenderUser::ID) {
      const auto user = store.user(static_cast<const api::messageSenderUser &>(*message->sender_id_).user_id_);
      if (user) name = user->firstName.empty() ? Td::DisplayName(*user) : user->firstName;
    } else {
      name = SenderName(store, *message);
    }
    names.push_back(name);
  }
  if (mine) names.push_back(Translated("you", "вы", "ви"));
  if (names.empty()) return {};
  if (names.size() == 1) return mine ? Tr("lng_from_you") : names.front();
  const auto last = names.back();
  names.pop_back();
  auto first = std::string();
  for (const auto &name : names) first += (first.empty() ? "" : ", ") + name;
  auto result = Translated("{first} and {last}", "{first} и {last}", "{first} і {last}");
  result.replace(result.find("{first}"), 7, first);
  result.replace(result.find("{last}"), 6, last);
  return result;
}

ChatLiveBar::ChatLiveBar(MainWindow &window, Td::Store &store, Td::ChatId chat)
    : _window(window), _store(store), _chat(chat) {
  auto *icon = gtk_image_new_from_icon_name("contrail-live-location-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 20);
  gtk_widget_add_css_class(icon, "location-row-icon");
  gtk_widget_add_css_class(icon, "live-location");
  gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
  auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(box, "chat-top-bar");
  gtk_widget_add_css_class(box, "live-bar");
  gtk_box_append(GTK_BOX(box), icon);
  auto *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(column, "top-bar-text");
  gtk_widget_set_hexpand(column, TRUE);
  gtk_widget_set_valign(column, GTK_ALIGN_CENTER);
  _title = gtk_label_new(Tr("lng_live_location").c_str());
  gtk_widget_add_css_class(_title, "top-bar-title");
  gtk_label_set_xalign(GTK_LABEL(_title), 0);
  gtk_box_append(GTK_BOX(column), _title);
  _text = gtk_label_new(nullptr);
  gtk_label_set_xalign(GTK_LABEL(_text), 0);
  gtk_label_set_ellipsize(GTK_LABEL(_text), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode(GTK_LABEL(_text), TRUE);
  gtk_box_append(GTK_BOX(column), _text);
  gtk_box_append(GTK_BOX(box), column);
  // One's own sharing stops right from the bar, as in the phone apps.
  _stop = MakeIconButton("window-close-symbolic", Tr("lng_group_call_screen_share_stop").c_str(), "flat");
  gtk_widget_add_css_class(_stop, "circular");
  gtk_widget_set_valign(_stop, GTK_ALIGN_CENTER);
  _signals.clicked(_stop, [this] {
    if (const auto id = Live().sharingIn(_chat)) Live().stop(_chat, id);
  });
  gtk_box_append(GTK_BOX(box), _stop);
  auto *click = gtk_gesture_click_new();
  _signals.connect<void(GtkGestureClick *, int, double, double)>(
      click, "released", [this](GtkGestureClick *, int, double, double) {
        if (TapAllowed()) ShowLiveMap(_window, _chat);
      });
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));
  _revealer = gtk_revealer_new();
  g_object_ref_sink(_revealer);
  gtk_revealer_set_transition_type(GTK_REVEALER(_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
  gtk_revealer_set_child(GTK_REVEALER(_revealer), box);
  _locations = std::make_unique<ChatLiveLocations>(store, chat);
  _locations->onChanged([this] { refresh(); });
  Live().onChanged([this] { refresh(); }, _lifetime);
  refresh();
}

ChatLiveBar::~ChatLiveBar() {
  _signals.clear();
  _locations = nullptr;
  g_object_unref(_revealer);
}

void ChatLiveBar::refresh() {
  const auto names = LiveLocationNames(_store, *_locations);
  gtk_label_set_text(GTK_LABEL(_text), names.c_str());
  gtk_widget_set_visible(_stop, Live().sharingIn(_chat) != 0);
  gtk_revealer_set_reveal_child(GTK_REVEALER(_revealer), !_locations->active().empty());
}

void ChatLiveBar::debugTap() {
  ShowLiveMap(_window, _chat);
}

std::string ChatLiveBar::debugState() const {
  return std::string("revealed=") + (gtk_revealer_get_reveal_child(GTK_REVEALER(_revealer)) ? "1" : "0") + " text=\""
         + gtk_label_get_text(GTK_LABEL(_text)) + "\" stop=" + (gtk_widget_get_visible(_stop) ? "1" : "0")
         + " active=" + std::to_string(_locations->active().size());
}

}  // namespace Contrail
