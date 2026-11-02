#include "chat_view.hpp"

#include "main_window.hpp"
#include "memory_trim.hpp"
#include "message.hpp"
#include "scroll_helpers.hpp"
#include "td/chat_row.hpp"
#include "td/dates.hpp"
#include "td/message_text.hpp"
#include "td/strings.hpp"
#include "userpics.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>

namespace Contrail {
namespace {

namespace api = Td::api;

constexpr auto kMessagesPerPage = 50;
constexpr auto kPreloadHeight = 6000.;  // About eight phone screens: pages come before a fling gets there.
// Room above the messages for older pages: a page of a channel with photos is up to ~20000px high,
// so this lasts ten pages of flinging without a stop. Much larger and float rounding of the
// render transforms starts to blur the text.
constexpr auto kTopReserve = 200000;
constexpr auto kTopReserveLow = 100000;
constexpr auto kTopReserveHigh = 300000;  // Rows removed above the screen leave their height to the reserve.
// Rows within this distance from the screen stay when the list stands still (well beyond where the next ones are
// loaded, so the two don't take turns), and only when there are more rows than this.
constexpr auto kKeepHeight = 2.5 * kPreloadHeight;
constexpr auto kMaxRows = 300;
// A fling or a drag of the scroll bar that stops this far above the rows doesn't walk there page by page: the
// messages where it stopped are loaded at once (jumpUp()).
constexpr auto kJumpGap = kPreloadHeight;
constexpr auto kJumpRowMin = 40.;
constexpr auto kJumpRowMax = 400.;
constexpr auto kDraftDelay = 2000u;
constexpr auto kStatusPeriod = 30000u;
constexpr auto kTypingPeriod = gint64(5) * G_USEC_PER_SEC;
// A server message id is the server's number shifted by 20 bits. History asked from the first one is the start of a
// chat, whichever messages are left there.
constexpr auto kFirstMessageId = std::int64_t(1) << 20;

void OpenPath(const std::string &path) {
  if (auto *uri = g_filename_to_uri(path.c_str(), nullptr, nullptr)) {
    OpenUri(uri);
    g_free(uri);
  }
}

template <typename Type>
[[nodiscard]] const Type *Cast(const api::Object *object) {
  return (object && object->get_id() == Type::ID) ? static_cast<const Type *>(object) : nullptr;
}

[[nodiscard]] bool IsLocal(const MessageItem *item) {
  return item->local();
}

[[nodiscard]] api::object_ptr<api::formattedText> Formatted(const std::string &text) {
  return api::make_object<api::formattedText>(text, std::vector<api::object_ptr<api::textEntity>>());
}

[[nodiscard]] api::object_ptr<api::InputMessageContent> InputText(const std::string &text) {
  return api::make_object<api::inputMessageText>(Formatted(text), nullptr, true);
}

[[nodiscard]] api::object_ptr<api::InputMessageReplyTo> ReplyTo(std::int64_t id) {
  return id ? api::make_object<api::inputMessageReplyToMessage>(id, nullptr, 0, std::string()) : nullptr;
}

// The text a message has: its own, or the caption of its media.
[[nodiscard]] const api::formattedText *MessageTextOf(const api::message &message) {
  const auto content = message.content_.get();
  if (const auto text = Cast<api::messageText>(content)) return text->text_.get();
  if (const auto photo = Cast<api::messagePhoto>(content)) return photo->caption_.get();
  if (const auto video = Cast<api::messageVideo>(content)) return video->caption_.get();
  if (const auto document = Cast<api::messageDocument>(content)) return document->caption_.get();
  if (const auto audio = Cast<api::messageAudio>(content)) return audio->caption_.get();
  if (const auto animation = Cast<api::messageAnimation>(content)) return animation->caption_.get();
  if (const auto voice = Cast<api::messageVoiceNote>(content)) return voice->caption_.get();
  return nullptr;
}

[[nodiscard]] std::string PlainText(const api::message &message) {
  const auto text = MessageTextOf(message);
  return text ? text->text_ : std::string();
}

[[nodiscard]] std::int64_t SenderKey(const api::message &message) {
  if (const auto user = Cast<api::messageSenderUser>(message.sender_id_.get())) return user->user_id_;
  if (const auto chat = Cast<api::messageSenderChat>(message.sender_id_.get())) return chat->chat_id_;
  return 0;
}

// The name and the color of who sent a message.
void DescribeSender(const Td::Store &store, const api::message &message, std::string &name, int &color) {
  if (const auto sender = Cast<api::messageSenderUser>(message.sender_id_.get())) {
    if (const auto user = store.user(sender->user_id_)) {
      name = Td::DisplayName(*user);
      color = user->accentColor;
    }
  } else if (const auto sender = Cast<api::messageSenderChat>(message.sender_id_.get())) {
    if (const auto chat = store.chat(sender->chat_id_)) {
      name = chat->title;
      color = chat->accentColor;
    }
  }
}

// "online", "last seen today at 12:34".
[[nodiscard]] std::string OnlineText(const Td::User &user) {
  const auto now = Td::Now();
  if (user.onlineUntil > now) return Tr("lng_status_online");
  switch (user.lastSeen) {
    case 0: return Tr("lng_status_offline");
    case -1: return Tr("lng_status_recently");
    case -2: return Tr("lng_status_last_week");
    case -3: return Tr("lng_status_last_month");
    default: break;
  }
  const auto ago = now - user.lastSeen;
  if (ago < 60) return Tr("lng_status_lastseen_now");
  if (ago < 3600) return TrCount("lng_status_lastseen_minutes", ago / 60);
  const auto days = Td::DayNumber(now) - Td::DayNumber(user.lastSeen);
  if (days == 0 && ago < 12 * 3600) return TrCount("lng_status_lastseen_hours", ago / 3600);
  const auto time = Td::FormatTime(user.lastSeen);
  if (days == 0) return Tr("lng_status_lastseen_today", {{"time", time}});
  if (days == 1) return Tr("lng_status_lastseen_yesterday", {{"time", time}});
  return Tr("lng_status_lastseen_date", {{"date", Td::FormatDay(user.lastSeen)}});
}

[[nodiscard]] MessageWidget *RowMessage(GtkListBoxRow *row) {
  return row ? static_cast<MessageWidget *>(g_object_get_data(G_OBJECT(row), "contrail-message")) : nullptr;
}

// The row at `y` in the list, or the first shown one below when `y` falls on a date header (a row's allocation
// leaves its header out, and gtk_list_box_get_row_at_y finds nothing there) or above the first row. Null below the
// last one.
[[nodiscard]] GtkListBoxRow *RowFrom(GtkWidget *list, double y) {
  if (y > 0) {
    if (auto *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(list), int(y))) return row;
  }
  GtkListBoxRow *row = nullptr;
  for (auto i = 0; (row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), i)); ++i) {
    graphene_rect_t bounds;
    if (gtk_widget_get_visible(GTK_WIDGET(row)) && gtk_widget_compute_bounds(GTK_WIDGET(row), list, &bounds)
        && bounds.origin.y + bounds.size.height > y) {
      return row;
    }
  }
  return nullptr;
}

[[nodiscard]] GtkListBoxRow *NextRow(GtkListBoxRow *row) {
  return gtk_list_box_get_row_at_index(GTK_LIST_BOX(gtk_widget_get_parent(GTK_WIDGET(row))),
                                       gtk_list_box_row_get_index(row) + 1);
}

}  // namespace

void ChatView::Timer::callOnce(guint ms) {
  cancel();
  _source = g_timeout_add(ms, +[](gpointer data) -> gboolean {
    auto *self = static_cast<Timer *>(data);
    self->_source = 0;
    // The callback may close the chat, and this timer with it.
    auto callback = self->_callback;
    callback();
    return G_SOURCE_REMOVE;
  }, this);
}

void ChatView::Timer::cancel() {
  if (_source) g_source_remove(std::exchange(_source, 0));
}

ChatView::ChatView(Td::Store &store, Td::ChatId chat, MainWindow &window, std::int64_t showAt)
    : _store(store), _chat(chat), _window(window), _pendingShowAt(showAt), _readTimer([this] { markAsRead(); }),
      _statusTimer([this] {
        refreshHeader();
        _statusTimer.callOnce(kStatusPeriod);
      }),
      _reserveTimer([this] { maintainTopReserve(); }), _jumpTimer([this] { scheduleViewportUpdate(); }),
      _draftTimer([this] { saveDraft(); }) {
  _root = adw_toolbar_view_new();
  g_object_ref_sink(_root);
  adw_toolbar_view_set_bottom_bar_style(ADW_TOOLBAR_VIEW(_root), ADW_TOOLBAR_RAISED);
  buildHeader();
  buildList();
  buildComposer();
  refreshHeader();
  refreshComposerState();
  _statusTimer.callOnce(kStatusPeriod);
  _signals.connect<void(GtkWidget *)>(_root, "map", [this](GtkWidget *) { scheduleViewportUpdate(); });
  _signals.connect<void(GObject *, GParamSpec *)>(_window.widget(), "notify::is-active",
                                                  [this](GObject *, GParamSpec *) { _readTimer.callOnce(300); });
  subscribe();

  // TDLib keeps a chat's messages up to date while it is open, and needs that to count views.
  _store.client().send(api::make_object<api::openChat>(_chat));
  if (const auto known = _store.chat(_chat)) {
    if (known->unread > 0 && known->readInbox) _unreadAfter = known->readInbox;
    _savedDraft = known->draft;
    _savedDraftReply = known->draftReplyTo;
    if (!known->draft.empty()) setComposerText(known->draft);
    if (const auto reply = known->draftReplyTo) {
      _store.client().request<api::message>(
          api::make_object<api::getMessage>(_chat, reply), _lifetime,
          [this](api::message &message) {
            auto item = MessageItem();
            item.data = api::make_object<api::message>();
            item.data->id_ = message.id_;
            item.data->sender_id_ = std::move(message.sender_id_);
            item.data->content_ = std::move(message.content_);
            if (!_editing) replyTo(item);
          },
          [](api::error &) {});
    }
  }
  if (!_pendingShowAt) _pendingShowAt = _unreadAfter;
  _stickToBottom = !_pendingShowAt;
  loadInitial();
}

ChatView::~ChatView() {
  _signals.clear();
  if (_viewportSource) g_source_remove(_viewportSource);
  saveDraft();
  _store.client().send(api::make_object<api::closeChat>(_chat));
  _rows.clear();
  _itemRows.clear();
  _items.clear();
  g_object_unref(_root);
}

void ChatView::subscribe() {
  _store.client().subscribe([this](api::Update &update) { this->updated(update); }, _lifetime);
  _store.onChat([this](Td::ChatId id) {
    if (id != _chat) return;
    refreshHeader();
    refreshComposerState();
    refreshFooters();
    draftChanged();
  }, _lifetime);
  _store.onUser([this](Td::UserId id) {
    const auto known = _store.chat(_chat);
    if (known && known->kind == Td::ChatKind::Private && known->peerId == id) refreshHeader();
  }, _lifetime);
  _store.onFile([this](const Td::File &file) {
    for (const auto &[raw, row] : _rows) row->fileChanged(file);
  }, _lifetime);
}

void ChatView::updated(api::Update &update) {
  const auto refresh = [&](std::int64_t chat, std::int64_t id, auto change) {
    if (chat != _chat) return;
    if (const auto item = find(id)) {
      change(*item->data);
      refreshItem(item);
    }
  };
  switch (update.get_id()) {
    case api::updateNewMessage::ID: {
      auto &data = static_cast<api::updateNewMessage &>(update);
      if (data.message_->chat_id_ != _chat || data.message_->scheduling_state_) break;
      // Further back in the chat the newest messages aren't held: they come when the list gets there.
      if (!_reachedBottom && !data.message_->sending_state_) break;
      if (insert(std::move(data.message_))) {
        sync();
        if (_stickToBottom) _readTimer.callOnce(300);
      }
      break;
    }
    case api::updateMessageSendSucceeded::ID: {
      auto &data = static_cast<api::updateMessageSendSucceeded &>(update);
      messageSent(std::move(data.message_), data.old_message_id_);
      break;
    }
    case api::updateMessageSendFailed::ID: {
      auto &data = static_cast<api::updateMessageSendFailed &>(update);
      messageSent(std::move(data.message_), data.old_message_id_);
      break;
    }
    case api::updateMessageContent::ID: {
      auto &data = static_cast<api::updateMessageContent &>(update);
      refresh(data.chat_id_, data.message_id_,
              [&](api::message &message) { message.content_ = std::move(data.new_content_); });
      break;
    }
    case api::updateMessageEdited::ID: {
      auto &data = static_cast<api::updateMessageEdited &>(update);
      refresh(data.chat_id_, data.message_id_, [&](api::message &message) {
        message.edit_date_ = data.edit_date_;
        message.reply_markup_ = std::move(data.reply_markup_);
      });
      break;
    }
    case api::updateMessageInteractionInfo::ID: {
      auto &data = static_cast<api::updateMessageInteractionInfo &>(update);
      if (data.chat_id_ != _chat) break;
      if (const auto item = find(data.message_id_)) {
        item->data->interaction_info_ = std::move(data.interaction_info_);
        if (const auto row = rowFor(item)) row->refreshFooter();
      }
      break;
    }
    case api::updateMessageIsPinned::ID: {
      auto &data = static_cast<api::updateMessageIsPinned &>(update);
      if (data.chat_id_ != _chat) break;
      if (const auto item = find(data.message_id_)) item->data->is_pinned_ = data.is_pinned_;
      break;
    }
    case api::updateDeleteMessages::ID: {
      auto &data = static_cast<api::updateDeleteMessages &>(update);
      // Not permanent: TDLib only dropped them from its memory, or they were being sent.
      if (data.chat_id_ == _chat && data.is_permanent_) remove(data.message_ids_);
      break;
    }
    default: break;
  }
}

// The message being sent has its server id now, or failed: its object changes, its row stays.
void ChatView::messageSent(api::object_ptr<api::message> message, std::int64_t oldId) {
  if (!message || message->chat_id_ != _chat) return;
  if (const auto item = find(oldId)) {
    item->data = std::move(message);
    resort();
    refreshItem(item);
    sync();
  } else if (_reachedBottom) {
    insert(std::move(message));
    sync();
  }
}

// --- header ------------------------------------------------------------------

void ChatView::buildHeader() {
  auto *header = adw_header_bar_new();
  _title = adw_window_title_new("", "");
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), _title);
  auto *avatar = MakeAvatar(_store, _chat, 32, true);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), avatar);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(_root), header);
}

void ChatView::refreshHeader() {
  const auto known = _store.chat(_chat);
  if (!known) return;
  auto name = known->title;
  auto status = std::string();
  if (known->kind == Td::ChatKind::Private || known->kind == Td::ChatKind::Secret) {
    if (known->peerId == _store.selfId()) {
      name = Tr("lng_saved_messages");
    } else if (const auto user = _store.user(known->peerId)) {
      status = user->bot ? Tr("lng_status_bot") : OnlineText(*user);
    }
  } else if (const auto group = _store.group(*known); group && group->members > 0) {
    status = TrCount(known->kind == Td::ChatKind::Channel ? "lng_chat_status_subscribers" : "lng_chat_status_members",
                     group->members);
  }
  // "typing...", "sending a photo..." while someone does.
  if (const auto action = Td::TypingText(*known, _store); !action.empty()) status = action;
  adw_window_title_set_title(ADW_WINDOW_TITLE(_title), name.c_str());
  adw_window_title_set_subtitle(ADW_WINDOW_TITLE(_title), status.c_str());
}

// --- the list ----------------------------------------------------------------

int ChatView::compareRows(GtkListBoxRow *a, GtkListBoxRow *b, gpointer) {
  const auto first = RowMessage(a), second = RowMessage(b);
  if (!first || !second) return 0;
  const auto left = first->item(), right = second->item();
  // Messages being sent have local ids: they go after the others.
  if (IsLocal(left) != IsLocal(right)) return IsLocal(left) ? 1 : -1;
  return (left->id() < right->id()) ? -1 : (left->id() > right->id()) ? 1 : 0;
}

// Date separators and grouping of consecutive messages from the same sender.
void ChatView::updateRowHeader(GtkListBoxRow *row, GtkListBoxRow *before, gpointer data) {
  const auto self = static_cast<ChatView *>(data);
  const auto message = RowMessage(row), previous = RowMessage(before);
  if (!message) return;
  const auto &item = *message->item()->data;
  const auto day = Td::DayNumber(item.date_);
  const auto newDay = !previous || Td::DayNumber(previous->item()->data->date_) != day;
  const auto unreadBar = message == self->_unreadRow;
  // Headers are recomputed for all the rows on every change of the list: keep the ones that stay the same instead
  // of creating new widgets (and a new layout) each time.
  const auto key = (newDay || unreadBar) ? gsize(day * 2 + (unreadBar ? 1 : 0)) : gsize(0);
  const auto same = (key == GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(row), "contrail-header-key")))
                    && (!key || gtk_list_box_row_get_header(row));
  g_object_set_data(G_OBJECT(row), "contrail-header-key", GSIZE_TO_POINTER(key));
  if (same) {
  } else if (newDay || unreadBar) {
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    if (newDay) {
      auto *label = gtk_label_new(Td::FormatDay(item.date_).c_str());
      gtk_widget_add_css_class(label, "date-separator");
      gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
      gtk_box_append(GTK_BOX(box), label);
    }
    if (unreadBar) {
      auto *label = gtk_label_new(Tr("lng_unread_bar_some").c_str());
      gtk_widget_add_css_class(label, "unread-bar");
      gtk_box_append(GTK_BOX(box), label);
    }
    gtk_list_box_row_set_header(row, box);
  } else {
    gtk_list_box_row_set_header(row, nullptr);
  }
  const auto service = [](const MessageWidget *widget) {
    return Td::IsServiceMessage(*widget->item()->data->content_);
  };
  const auto sameSender = !newDay && previous && !service(previous) && !service(message)
                          && SenderKey(*previous->item()->data) == SenderKey(item)
                          && item.date_ - previous->lastItem()->data->date_ < 15 * 60;
  message->setFirstInGroup(!sameSender);
}

void ChatView::buildList() {
  _list = gtk_list_box_new();
  gtk_widget_add_css_class(_list, "message-list");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(_list), GTK_SELECTION_NONE);
  gtk_widget_set_valign(_list, GTK_ALIGN_END);
  gtk_list_box_set_sort_func(GTK_LIST_BOX(_list), compareRows, this, nullptr);
  gtk_list_box_set_header_func(GTK_LIST_BOX(_list), updateRowHeader, this, nullptr);

  auto *clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 720);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 560);
  adw_clamp_set_child(ADW_CLAMP(clamp), _list);

  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  // Room for the older messages above the loaded ones (see setTopReserve).
  _topSpacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(box), _topSpacer);
  // The spinner and the retry button float over the messages.
  auto *spinner = adw_spinner_new();
  gtk_widget_set_size_request(spinner, 24, 24);
  _loading = adw_bin_new();
  adw_bin_set_child(ADW_BIN(_loading), spinner);
  gtk_widget_add_css_class(_loading, "osd");
  gtk_widget_add_css_class(_loading, "history-loading");
  gtk_widget_set_halign(_loading, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(_loading, GTK_ALIGN_START);
  gtk_widget_set_margin_top(_loading, 10);
  gtk_widget_set_can_target(_loading, FALSE);
  gtk_widget_set_visible(_loading, FALSE);
  _retry = gtk_button_new_with_label(Tr("lng_bot_download_retry").c_str());
  gtk_widget_add_css_class(_retry, "osd");
  gtk_widget_add_css_class(_retry, "pill");
  gtk_widget_set_halign(_retry, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(_retry, GTK_ALIGN_START);
  gtk_widget_set_margin_top(_retry, 10);
  gtk_widget_set_visible(_retry, FALSE);
  _signals.clicked(_retry, [this] {
    _loadFailed = false;
    gtk_widget_set_visible(_retry, FALSE);
    if (!_initialLoaded) loadInitial();
    else scheduleViewportUpdate();
  });
  gtk_box_append(GTK_BOX(box), clamp);
  gtk_widget_set_valign(box, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom(box, 6);

  _scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(_scroll), box);
  auto *viewport = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(_scroll));
  if (GTK_IS_VIEWPORT(viewport)) gtk_viewport_set_scroll_to_focus(GTK_VIEWPORT(viewport), FALSE);
  gtk_widget_set_vexpand(_scroll, TRUE);
  gtk_widget_add_css_class(_scroll, "chat-background");
  auto *touch = gtk_event_controller_legacy_new();
  gtk_event_controller_set_propagation_phase(touch, GTK_PHASE_CAPTURE);
  _signals.connect<gboolean(GtkEventControllerLegacy *, GdkEvent *)>(touch, "event",
      [this](GtkEventControllerLegacy *, GdkEvent *event) -> gboolean {
        const auto type = gdk_event_get_event_type(event);
        if (type == GDK_TOUCH_BEGIN || type == GDK_BUTTON_PRESS) {
          _touchActive = true;
        } else if (type == GDK_TOUCH_END || type == GDK_TOUCH_CANCEL || type == GDK_BUTTON_RELEASE) {
          _touchActive = false;
          _reserveTimer.callOnce(300);
        }
        if (type == GDK_TOUCH_UPDATE || type == GDK_SCROLL
            || (type == GDK_MOTION_NOTIFY && (gdk_event_get_modifier_state(event) & GDK_BUTTON1_MASK))) {
          // A real drag takes precedence over the initial unread/reply positioning.
          _scrollTarget = 0;
          _scrollTargetY = std::nullopt;
          _stickToBottom = false;
        }
        return FALSE;
      });
  gtk_widget_add_controller(_scroll, touch);
  _adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(_scroll));
  _signals.connect<void(GtkAdjustment *)>(_adjustment, "value-changed", [this](GtkAdjustment *) {
    if (!_adjustingScroll) NoteListMotion();
    PostponeMemoryTrim();
    scrolled();
  });
  _signals.connect<void(GObject *, GParamSpec *)>(_adjustment, "notify::upper", [this](GObject *, GParamSpec *) {
    // Emitted while the viewport is allocated, before the child is placed and anything is drawn:
    // a shift requested with the content change lands in the same frame, without a visible jump.
    if (_pendingShift != 0.) {
      const auto shift = std::exchange(_pendingShift, 0.);
      // A shrinking reserve may already have clamped the position.
      const auto base = (shift < 0) ? _shiftBase : gtk_adjustment_get_value(_adjustment);
      if (!scrollIdleIgnoringShift()) {
        // The reserve ran out during a fling: GTK's deceleration would put its own position back.
        gtk_scrolled_window_set_kinetic_scrolling(GTK_SCROLLED_WINDOW(_scroll), FALSE);
        gtk_scrolled_window_set_kinetic_scrolling(GTK_SCROLLED_WINDOW(_scroll), TRUE);
      }
      _adjustingScroll = true;
      gtk_adjustment_set_value(_adjustment, base + shift);
      _adjustingScroll = false;
    }
    scheduleViewportUpdate();
  });
  _signals.connect<void(GObject *, GParamSpec *)>(_adjustment, "notify::page-size",
                                                  [this](GObject *, GParamSpec *) { scheduleViewportUpdate(); });

  auto *overlay = gtk_overlay_new();
  gtk_overlay_set_child(GTK_OVERLAY(overlay), _scroll);
  _toBottom = gtk_button_new_from_icon_name("go-bottom-symbolic");
  gtk_widget_add_css_class(_toBottom, "circular");
  gtk_widget_add_css_class(_toBottom, "osd");
  gtk_widget_add_css_class(_toBottom, "to-bottom");
  gtk_widget_set_halign(_toBottom, GTK_ALIGN_END);
  gtk_widget_set_valign(_toBottom, GTK_ALIGN_END);
  gtk_widget_set_margin_end(_toBottom, 18);
  gtk_widget_set_margin_bottom(_toBottom, 18);
  gtk_widget_set_visible(_toBottom, FALSE);
  _signals.clicked(_toBottom, [this] {
    // Jumped somewhere into the past: the end of the chat comes again.
    if (!_reachedBottom) reload(0);
    else scrollToBottom();
  });
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), _toBottom);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), _loading);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), _retry);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(_root), overlay);
}

// --- pages of history ----------------------------------------------------------

void ChatView::loadInitial() {
  if (_loadingOlder || _loadingNewer || _applyingPage) return;
  if (_pendingShowAt > 0) {
    requestHistory(_pendingShowAt, -kMessagesPerPage / 2, kMessagesPerPage, Page::Initial);
  } else {
    requestHistory(0, 0, kMessagesPerPage, Page::Initial);
  }
}

void ChatView::loadOlder() {
  if (_loadingOlder || _loadingNewer || _applyingPage || _loadFailed || !_initialLoaded || _reachedTop) return;
  if (const auto oldest = oldestId()) requestHistory(oldest, 0, kMessagesPerPage, Page::Older);
}

void ChatView::loadNewer() {
  if (_loadingOlder || _loadingNewer || _applyingPage || _loadFailed || !_initialLoaded || _reachedBottom) return;
  if (const auto newest = newestId()) requestHistory(newest, -kMessagesPerPage + 1, kMessagesPerPage, Page::Newer);
}

void ChatView::requestHistory(std::int64_t from, int offset, int limit, Page page) {
  gtk_widget_set_visible(_loading, TRUE);
  gtk_widget_set_visible(_retry, FALSE);
  (page == Page::Newer ? _loadingNewer : _loadingOlder) = true;
  _store.client().request<api::messages>(
      api::make_object<api::getChatHistory>(_chat, from, offset, limit, false), _pageLifetime,
      [this, page](api::messages &messages) { applyPage(messages, page); },
      [this, page](api::error &error) {
        (page == Page::Newer ? _loadingNewer : _loadingOlder) = false;
        pageFailed(error.message_);
      });
}

void ChatView::pageFailed(const std::string &text) {
  _loadFailed = true;
  gtk_widget_set_visible(_loading, FALSE);
  gtk_widget_set_visible(_retry, TRUE);
  ShowToast(text);
}

void ChatView::applyPage(api::messages &messages, Page page) {
  _applyingPage = true;
  const auto first = !_initialLoaded;
  // An older page goes above the messages on the screen: measure how much it adds.
  const auto keepPosition = page == Page::Older && !first;
  const auto heightBefore = keepPosition ? listHeight() : 0;
  auto added = 0;
  for (auto &message : messages.messages_) {
    if (message && !message->scheduling_state_ && insert(std::move(message))) ++added;
  }
  // TDLib gives as many messages as it has at hand, sometimes one: only nothing new means the end.
  if (page == Page::Older && !added) _reachedTop = true;
  if (page == Page::Newer && !added) _reachedBottom = true;
  if (page == Page::Initial && !_pendingShowAt) _reachedBottom = true;
  if (first && _items.empty()) _reachedTop = _reachedBottom = true;
  const auto known = _store.chat(_chat);
  if (known && newestId() >= known->lastMessageId) _reachedBottom = true;
  _initialLoaded = true;
  sync();
  if (keepPosition) {
    if (const auto grown = listHeight() - heightBefore; grown > 0) {
      const auto fromReserve = std::min(grown, _topReserve);
      // Without enough room above, the rest moves the content and the position with it.
      setTopReserve(_topReserve - fromReserve, grown - fromReserve);
    }
  }
  if (first) {
    setTopReserve(_reachedTop ? 0 : kTopReserve, 0);
    if (page == Page::Start) {
      // Whichever message is the first one left in the chat, at the top of the screen.
      _scrollTargetY = 0.;
      if (!scrollToLoaded(oldestId())) scrollToBottom();
    } else if (_pendingShowAt > 0) {
      if (!scrollToLoaded(std::exchange(_pendingShowAt, 0))) scrollToBottom();
    } else {
      scrollToBottom();
    }
    _readTimer.callOnce(500);
  }
  (page == Page::Newer ? _loadingNewer : _loadingOlder) = false;
  _applyingPage = false;
  gtk_widget_set_visible(_loading, FALSE);
  if (g_getenv("CONTRAIL_DEBUG")) {
    g_message("History page: %s received=%zu added=%d rows=%zu top=%d bottom=%d",
              page == Page::Older ? "older" : page == Page::Newer ? "newer" : "first", messages.messages_.size(),
              added, _rows.size(), int(_reachedTop), int(_reachedBottom));
  }
  scheduleViewportUpdate();
}

void ChatView::reload(std::int64_t showAt) {
  unload();
  _pendingShowAt = showAt;
  _stickToBottom = !showAt;
  loadInitial();
}

// No rows, no requests: the chat starts loading again.
void ChatView::unload() {
  _pageLifetime.reset();
  _initialLoaded = false;
  _loadFailed = false;
  _loadingOlder = _loadingNewer = false;
  _reachedTop = _reachedBottom = false;
  _scrollTarget = 0;
  _pendingShift = 0;
  setTopReserve(0, 0);
  _unreadRow = nullptr;
  _itemRows.clear();
  _rows.clear();
  _items.clear();
  _viewed.clear();
}

// A fling or the scroll bar stopped `gap` px above the rows: the messages about that far back come at once. TDLib
// counts history only forward from a message, so the way back goes by date: as many seconds back as that many
// messages took around here.
void ChatView::jumpUp(double gap) {
  const auto value = gtk_adjustment_get_value(_adjustment);
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  // At the top of the scroll bar: the start of the chat.
  if (value < page) {
    jumpToStart();
    return;
  }
  const auto rowHeight = std::clamp(double(listHeight()) / std::max(_rows.size(), size_t(1)), kJumpRowMin, kJumpRowMax);
  const auto back = (gap + page / 2) / rowHeight;
  auto regular = Items();
  for (const auto &item : _items) {
    if (!IsLocal(item.get())) regular.push_back(item.get());
  }
  if (regular.size() < 2) {
    jumpToStart();
    return;
  }
  const auto oldest = regular.front()->data->date_;
  const auto span = regular.back()->data->date_ - oldest;
  const auto seconds = std::max(double(span) / double(regular.size() - 1), 1.);
  const auto date = std::int32_t(std::max(oldest - back * seconds, 1.));
  _pageLifetime.reset();
  _loadingOlder = true;
  gtk_widget_set_visible(_loading, TRUE);
  _store.client().request<api::message>(
      api::make_object<api::getChatMessageByDate>(_chat, date), _pageLifetime,
      [this, page](api::message &message) {
        _loadingOlder = false;
        jumpToMessage(message.id_, page / 2);
      },
      // Before the chat began.
      [this](api::error &) {
        _loadingOlder = false;
        jumpToStart();
      });
}

void ChatView::jumpToMessage(std::int64_t id, double y) {
  unload();
  _stickToBottom = false;
  _pendingShowAt = id;
  _scrollTargetY = y;
  loadInitial();
}

void ChatView::jumpToStart() {
  unload();
  _stickToBottom = false;
  requestHistory(kFirstMessageId, -kMessagesPerPage + 1, kMessagesPerPage, Page::Start);
}

// --- messages ------------------------------------------------------------------

MessageItem *ChatView::find(std::int64_t id) const {
  const auto i = std::ranges::find_if(_items, [&](const auto &item) { return item->id() == id; });
  return i != _items.end() ? i->get() : nullptr;
}

std::int64_t ChatView::oldestId() const {
  for (const auto &item : _items) {
    if (!item->local()) return item->id();
  }
  return 0;
}

std::int64_t ChatView::newestId() const {
  for (auto i = _items.rbegin(); i != _items.rend(); ++i) {
    if (!(*i)->local()) return (*i)->id();
  }
  return 0;
}

// Null when it is here already.
MessageItem *ChatView::insert(api::object_ptr<api::message> message) {
  if (find(message->id_)) return nullptr;
  auto item = std::make_unique<MessageItem>();
  item->data = std::move(message);
  const auto raw = item.get();
  const auto before = [](const std::unique_ptr<MessageItem> &a, const std::unique_ptr<MessageItem> &b) {
    return std::pair(a->local(), a->id()) < std::pair(b->local(), b->id());
  };
  const auto place = std::upper_bound(_items.begin(), _items.end(), item, before);
  _items.insert(place, std::move(item));
  return raw;
}

void ChatView::resort() {
  std::stable_sort(_items.begin(), _items.end(), [](const auto &a, const auto &b) {
    return std::pair(a->local(), a->id()) < std::pair(b->local(), b->id());
  });
}

void ChatView::remove(const std::vector<std::int64_t> &ids) {
  const auto gone = std::unordered_set<std::int64_t>(ids.begin(), ids.end());
  // The rows let go of the messages first.
  auto removed = std::vector<std::unique_ptr<MessageItem>>();
  for (auto i = _items.begin(); i != _items.end();) {
    if (gone.contains((*i)->id())) {
      removed.push_back(std::move(*i));
      i = _items.erase(i);
    } else {
      ++i;
    }
  }
  if (removed.empty()) return;
  sync();
}

// The rows follow the messages: one per message, one per album. A row keeps its widgets when its album grows (the
// rest of it comes with the next page) or shrinks (a message deleted).
void ChatView::sync() {
  auto units = std::vector<Items>();
  auto unitOfAlbum = std::unordered_map<std::int64_t, std::size_t>();
  for (const auto &item : _items) {
    const auto &message = *item->data;
    // A group became a supergroup: Telegram Desktop shows no line for what has no text.
    if (Td::IsServiceMessage(*message.content_) && Td::ServiceText(message, _store).empty()) continue;
    if (const auto album = message.media_album_id_) {
      const auto [i, added] = unitOfAlbum.emplace(album, units.size());
      if (!added) {
        units[i->second].push_back(item.get());
        continue;
      }
    }
    units.push_back({item.get()});
  }
  auto used = std::set<MessageWidget *>();
  for (auto &unit : units) {
    MessageWidget *row = nullptr;
    for (const auto item : unit) {
      if (const auto i = _itemRows.find(item); i != _itemRows.end() && !used.contains(i->second)) {
        row = i->second;
        break;
      }
    }
    if (row) {
      setRowItems(row, std::move(unit));
    } else {
      row = addRow(std::move(unit));
    }
    used.insert(row);
  }
  _unreadRow = nullptr;
  for (auto i = _rows.begin(); i != _rows.end();) {
    if (!used.contains(i->first)) {
      forgetRow(i->first);
      i = _rows.erase(i);
    } else {
      ++i;
    }
  }
  // The bar goes before the first message the user hasn't read, however his own ones lie around it.
  _unreadRow = nullptr;
  if (_unreadAfter) {
    for (const auto &item : _items) {
      if (item->id() > _unreadAfter && !item->local() && !item->data->is_outgoing_) {
        _unreadRow = rowFor(item.get());
        if (_unreadRow) break;
      }
    }
  }
  // Also recomputes the headers.
  gtk_list_box_invalidate_sort(GTK_LIST_BOX(_list));
  gtk_list_box_invalidate_headers(GTK_LIST_BOX(_list));
  scheduleViewportUpdate();
}

MessageWidget *ChatView::addRow(Items items) {
  auto row = std::make_unique<MessageWidget>(std::move(items), *this);
  const auto raw = row.get();
  gtk_list_box_append(GTK_LIST_BOX(_list), raw->row());
  for (const auto item : raw->items()) _itemRows[item] = raw;
  _rows.emplace(raw, std::move(row));
  return raw;
}

void ChatView::setRowItems(MessageWidget *row, Items items) {
  if (row->items() == items) return;
  for (const auto item : row->items()) {
    if (const auto i = _itemRows.find(item); i != _itemRows.end() && i->second == row) _itemRows.erase(i);
  }
  row->setItems(std::move(items));
  for (const auto item : row->items()) _itemRows[item] = row;
  gtk_list_box_row_changed(GTK_LIST_BOX_ROW(row->row()));
}

void ChatView::forgetRow(MessageWidget *row) {
  for (const auto item : row->items()) {
    if (const auto i = _itemRows.find(item); i != _itemRows.end() && i->second == row) _itemRows.erase(i);
  }
}

MessageWidget *ChatView::rowFor(const MessageItem *item) const {
  const auto i = _itemRows.find(item);
  return i != _itemRows.end() ? i->second : nullptr;
}

void ChatView::refreshItem(const MessageItem *item) {
  if (auto *row = rowFor(item)) {
    row->refresh();
    gtk_list_box_row_changed(GTK_LIST_BOX_ROW(row->row()));
  }
}

void ChatView::refreshFooters() {
  for (const auto &[raw, row] : _rows) row->refreshFooter();
}

// The rows far from the screen go while the list stands still, with their messages: TDLib has them. Above the
// screen they leave their height to the room reserved there, so nothing on the screen moves.
void ChatView::trimRows() {
  if (int(_rows.size()) <= kMaxRows || _scrollTarget || !_initialLoaded) return;
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  const graphene_point_t zero = GRAPHENE_POINT_INIT_ZERO;
  graphene_point_t top;
  if (page <= 0 || !gtk_widget_compute_point(_scroll, _list, &zero, &top)) return;
  auto removed = std::vector<std::unique_ptr<MessageItem>>();
  const auto drop = [&](auto condition) {
    for (auto i = _items.begin(); i != _items.end();) {
      if (condition(**i)) {
        removed.push_back(std::move(*i));
        i = _items.erase(i);
      } else {
        ++i;
      }
    }
  };
  const auto keptBelow = RowMessage(RowFrom(_list, top.y + page + kKeepHeight));
  if (keptBelow && !_stickToBottom
      && gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(keptBelow->row())) < int(_rows.size()) - 1) {
    const auto newest = keptBelow->lastItem()->id();
    drop([&](const MessageItem &item) { return item.local() || item.id() > newest; });
    _reachedBottom = false;
    sync();
  }
  const auto aboveY = int(top.y - kKeepHeight);
  const auto keptAbove = (aboveY > 0) ? RowMessage(RowFrom(_list, aboveY)) : nullptr;
  if (keptAbove && gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(keptAbove->row())) > 0) {
    const auto oldest = keptAbove->item()->id();
    const auto heightBefore = listHeight();
    drop([&](const MessageItem &item) { return item.id() < oldest; });
    _reachedTop = false;
    sync();
    if (const auto freed = heightBefore - listHeight(); freed > 0) setTopReserve(_topReserve + freed, 0);
  }
  if (g_getenv("CONTRAIL_DEBUG")) g_message("History trimmed: rows=%zu reserve=%d", _rows.size(), _topReserve);
}

// --- replies -------------------------------------------------------------------

ReplyPreview ChatView::replyPreview(const MessageItem &item) const {
  auto result = ReplyPreview();
  const auto reply = Cast<api::messageReplyToMessage>(item.data->reply_to_.get());
  if (!reply) return result;
  const auto here = !reply->chat_id_ || reply->chat_id_ == _chat;
  const api::message *message = nullptr;
  if (const auto loaded = here ? find(reply->message_id_) : nullptr) {
    message = loaded->data.get();
  } else if (const auto i = _replied.find(item.id()); i != _replied.end()) {
    if (!i->second) {
      result.ready = true;  // Deleted.
      return result;
    }
    message = i->second.get();
  }
  if (!message) {
    // Comes once; the rows waiting for it are made again then.
    if (_repliedAsked.insert(item.id()).second) {
      const auto id = item.id();
      auto self = const_cast<ChatView *>(this);
      _store.client().send(
          api::make_object<api::getRepliedMessage>(_chat, id),
          [self, id](Td::ObjectPtr object) {
            auto message = api::object_ptr<api::message>();
            if (object && object->get_id() == api::message::ID) {
              message.reset(static_cast<api::message *>(object.release()));
            }
            self->_replied[id] = std::move(message);
            self->repliedLoaded(id);
          },
          _lifetime);
    }
    return result;
  }
  result.ready = true;
  DescribeSender(_store, *message, result.name, result.color);
  result.text = Td::MessagePreview(*message, _store);
  result.target = (message->chat_id_ == _chat) ? message->id_ : 0;
  return result;
}

void ChatView::repliedLoaded(std::int64_t id) {
  if (const auto item = find(id)) refreshItem(item);
}

// --- scrolling -----------------------------------------------------------------

void ChatView::scrollToBottom() {
  _stickToBottom = true;
  _scrollTarget = 0;
  scheduleViewportUpdate();
}

void ChatView::scrollToItem(const MessageItem *item, bool highlight) {
  _stickToBottom = false;
  _scrollTarget = item->id();
  // Coming back to where the chat was: the same place on the screen, nothing lit up.
  _scrollTargetHighlight = highlight && !_scrollTargetY;
  scheduleViewportUpdate();
}

bool ChatView::scrollToLoaded(std::int64_t id) {
  if (const auto item = find(id); item && rowFor(item)) {
    // The place the chat opens at, the first unread message after it: nothing to point out there.
    if (id != _unreadAfter) {
      scrollToItem(item, true);
      return true;
    }
  }
  if (_unreadAfter && id == _unreadAfter) {
    if (_unreadRow) {
      scrollToItem(_unreadRow->item(), false);
      return true;
    }
    if (const auto item = find(id); item && rowFor(item)) {
      scrollToItem(item, false);
      return true;
    }
  }
  // The next message that exists, after a jump to a place that has none.
  if (_scrollTargetY) {
    for (const auto &item : _items) {
      if (item->id() >= id && rowFor(item.get())) {
        scrollToItem(item.get(), false);
        return true;
      }
    }
  }
  return false;
}

void ChatView::showAt(std::int64_t id) {
  _scrollTargetY = std::nullopt;
  if (scrollToLoaded(id)) return;
  reload(id);
}

bool ChatView::atBottom() const {
  const auto value = gtk_adjustment_get_value(_adjustment);
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  return value + page >= gtk_adjustment_get_upper(_adjustment) - 40 && _reachedBottom;
}

bool ChatView::scrollIdle() const {
  return scrollIdleIgnoringShift() && _pendingShift == 0.;
}

bool ChatView::scrollIdleIgnoringShift() const {
  return !_touchActive && !ListMoving();
}

void ChatView::scrolled() {
  if (_adjustingScroll || _applyingPage || !_initialLoaded) return;
  const auto upper = gtk_adjustment_get_upper(_adjustment);
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  const auto bottom = atBottom();
  // GTK also changes the adjustment while allocating newly inserted rows. Those changes do not
  // represent a user drag and must not drop the bottom/anchor/target requested before layout.
  // Only input events opt out of bottom following. Nor is a reserve shrinking at the start of the chat a way to
  // the bottom: GTK clamps the position to the smaller list before the shift waiting for it puts it back.
  if (!_viewportSource && !_scrollTarget && bottom && _pendingShift == 0.) _stickToBottom = true;
  gtk_widget_set_visible(_toBottom, !bottom && upper > page);
  if (!_viewportSource) scheduleViewportUpdate();
  if (!_readTimer.active()) _readTimer.callOnce(300);
}

int ChatView::listHeight() const {
  const auto width = gtk_widget_get_width(_list);
  if (width <= 0) return 0;
  // GtkListBox gives each row its minimum height for the width.
  auto minimum = 0;
  gtk_widget_measure(_list, GTK_ORIENTATION_VERTICAL, width, &minimum, nullptr, nullptr, nullptr);
  return minimum;
}

// GTK's touch scrolling and fling keep their own absolute position: changing the scroll position
// while the finger or a fling moves the list makes it jump back, and loads more pages on the way.
// So older pages don't move the messages on the screen: they take the place of empty room reserved
// above them. The room is topped up (and removed at the start of the chat) only while the list
// stands still, together with the matching scroll position, in the same frame.
void ChatView::setTopReserve(int reserve, double shift) {
  reserve = std::max(reserve, 0);
  if (reserve != _topReserve) {
    _topReserve = reserve;
    gtk_widget_set_size_request(_topSpacer, -1, reserve);
  }
  if (shift != 0.) {
    if (_pendingShift == 0.) _shiftBase = gtk_adjustment_get_value(_adjustment);
    _pendingShift += shift;
  }
}

void ChatView::maintainTopReserve() {
  if (!_initialLoaded || _applyingPage || _loadingOlder) return;
  const auto wanted = [&] {
    return _reachedTop ? 0
           : (_topReserve < kTopReserveLow || _topReserve > kTopReserveHigh) ? kTopReserve
                                                                             : _topReserve;
  };
  const auto trim = int(_rows.size()) > kMaxRows && !_scrollTarget;
  if (wanted() == _topReserve && !trim) return;
  if (!scrollIdle()) {
    _reserveTimer.callOnce(300);
    return;
  }
  if (trim) trimRows();
  if (const auto now = wanted(); now != _topReserve) setTopReserve(now, now - _topReserve);
}

void ChatView::scheduleViewportUpdate() {
  if (_viewportSource) return;
  // Waits for GTK's next frame to allocate new rows before restoring scroll coordinates.
  _viewportSource = g_timeout_add(20, +[](gpointer data) -> gboolean {
    auto *self = static_cast<ChatView *>(data);
    self->_viewportSource = 0;
    self->updateViewport();
    return G_SOURCE_REMOVE;
  }, this);
}

void ChatView::updateViewport() {
  if (_applyingPage || !_initialLoaded) return;
  if (!gtk_widget_get_mapped(_root)) return;
  if (gtk_adjustment_get_page_size(_adjustment) <= 0
      || std::ranges::any_of(_rows, [](const auto &pair) {
           // A hidden row (an empty service message) has no height and never gets one.
           auto *row = pair.second->row();
           return gtk_widget_get_visible(row) && gtk_widget_get_height(row) <= 0;
         })) {
    scheduleViewportUpdate();
    return;
  }
  _adjustingScroll = true;
  if (_scrollTarget) {
    const auto item = find(_scrollTarget);
    if (const auto row = item ? rowFor(item) : nullptr) {
      // A message of an album: its cell, not the album's top.
      const auto y = _scrollTargetY.value_or(gtk_adjustment_get_page_size(_adjustment) / 3);
      if (ScrollWidgetToY(row->widgetFor(item), _scroll, _adjustment, y)) {
        if (_scrollTargetHighlight) row->highlight(item);
        _scrollTarget = 0;
        _scrollTargetY = std::nullopt;
      }
    } else {
      _scrollTarget = 0;
    }
  } else if (_stickToBottom) {
    gtk_adjustment_set_value(_adjustment,
                             gtk_adjustment_get_upper(_adjustment) - gtk_adjustment_get_page_size(_adjustment));
  }
  _adjustingScroll = false;
  maintainTopReserve();
  const auto upper = gtk_adjustment_get_upper(_adjustment);
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  gtk_widget_set_visible(_toBottom, !atBottom() && upper > page);
  preloadVisibleEdges();
  if (!_readTimer.active()) _readTimer.callOnce(300);
}

void ChatView::preloadVisibleEdges() {
  if (_applyingPage || _loadingOlder || _loadingNewer || !_initialLoaded || _loadFailed || _scrollTarget) return;
  const auto value = gtk_adjustment_get_value(_adjustment);
  const auto upper = gtk_adjustment_get_upper(_adjustment);
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  if (value - _topReserve < kPreloadHeight && !_reachedTop) {
    if (value + page < _topReserve - kJumpGap) {
      // Far above the rows: the messages there are loaded when the list stops, not every page on the way.
      if (scrollIdle()) {
        jumpUp(_topReserve - value - page);
      } else {
        _jumpTimer.callOnce(300);
      }
      return;
    }
    loadOlder();
  } else if (value + page > upper - kPreloadHeight) {
    loadNewer();
  }
}

std::vector<MessageWidget *> ChatView::visibleRows() const {
  auto result = std::vector<MessageWidget *>();
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  const graphene_point_t zero = GRAPHENE_POINT_INIT_ZERO;
  graphene_point_t top;
  if (page <= 0 || !gtk_widget_compute_point(_scroll, _list, &zero, &top)) return result;
  // Above the first message (the room for older pages) the list starts lower on the screen; a date header at the
  // top belongs to the row below it.
  for (auto *row = RowFrom(_list, top.y); row; row = NextRow(row)) {
    graphene_rect_t bounds;
    if (!gtk_widget_get_visible(GTK_WIDGET(row))) continue;
    if (!gtk_widget_compute_bounds(GTK_WIDGET(row), _list, &bounds)) break;
    if (bounds.origin.y >= top.y + page) break;
    if (auto *message = RowMessage(row)) result.push_back(message);
  }
  return result;
}

// What was seen whole goes to TDLib, which counts the views and moves the read mark. At the end of the chat
// everything is read, whatever was not on the screen.
void ChatView::markAsRead() {
  if (!gtk_window_is_active(GTK_WINDOW(_window.widget())) || _window.currentChat() != _chat || !_initialLoaded
      || _scrollTarget || !gtk_widget_get_mapped(_root)) {
    return;
  }
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  const auto known = _store.chat(_chat);
  if (page <= 0 || !known) return;
  auto ids = std::vector<std::int64_t>();
  for (const auto row : visibleRows()) {
    for (const auto item : row->items()) {
      if (item->local() || _viewed.contains(item->id())) continue;
      double y = 0;
      auto *widget = row->widgetFor(item);
      const auto height = gtk_widget_get_height(widget);
      if (height > 0 && WidgetViewportY(widget, _scroll, y) && y + height >= 0 && y + height <= page) {
        ids.push_back(item->id());
        _viewed.insert(item->id());
      }
    }
  }
  if ((_stickToBottom || atBottom()) && known->unread > 0 && known->lastMessageId
      && !std::ranges::count(ids, known->lastMessageId)) {
    ids.push_back(known->lastMessageId);
  }
  if ((_stickToBottom || atBottom()) && known->markedUnread) {
    _store.client().send(api::make_object<api::toggleChatIsMarkedAsUnread>(_chat, false));
  }
  if (ids.empty()) return;
  _store.client().send(api::make_object<api::viewMessages>(_chat, std::move(ids),
                                                           api::make_object<api::messageSourceChatHistory>(), false));
}

// --- the message field ---------------------------------------------------------

void ChatView::buildComposer() {
  auto *composer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  // Replying to / editing a message.
  _replyBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(_replyBar, "reply-bar");
  _replyIcon = gtk_image_new_from_icon_name("mail-reply-sender-symbolic");
  gtk_widget_add_css_class(_replyIcon, "accent");
  gtk_box_append(GTK_BOX(_replyBar), _replyIcon);
  auto *replyColumn = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(replyColumn, TRUE);
  _replyTitle = gtk_label_new(nullptr);
  gtk_widget_add_css_class(_replyTitle, "reply-name");
  gtk_label_set_xalign(GTK_LABEL(_replyTitle), 0);
  gtk_label_set_ellipsize(GTK_LABEL(_replyTitle), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(replyColumn), _replyTitle);
  _replyText = gtk_label_new(nullptr);
  gtk_label_set_xalign(GTK_LABEL(_replyText), 0);
  gtk_label_set_ellipsize(GTK_LABEL(_replyText), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode(GTK_LABEL(_replyText), TRUE);
  gtk_widget_add_css_class(_replyText, "dim-label");
  gtk_box_append(GTK_BOX(replyColumn), _replyText);
  gtk_box_append(GTK_BOX(_replyBar), replyColumn);
  auto *cancel = MakeIconButton("window-close-symbolic", Tr("lng_cancel").c_str(), "flat");
  gtk_widget_add_css_class(cancel, "circular");
  _signals.clicked(cancel, [this] { cancelReplyOrEdit(); });
  gtk_box_append(GTK_BOX(_replyBar), cancel);
  gtk_widget_set_visible(_replyBar, FALSE);
  gtk_box_append(GTK_BOX(composer), _replyBar);

  // The message field.
  auto *field = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(field, "composer");
  _input = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(_input), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(_input), FALSE);
  gtk_text_view_set_input_hints(GTK_TEXT_VIEW(_input),
                                GtkInputHints(GTK_INPUT_HINT_SPELLCHECK | GTK_INPUT_HINT_WORD_COMPLETION
                                              | GTK_INPUT_HINT_EMOJI | GTK_INPUT_HINT_UPPERCASE_SENTENCES));
  gtk_widget_add_css_class(_input, "message-input");
  auto *inputScroll = gtk_scrolled_window_new();
  // No scrollbar (a long text still scrolls with the finger): its minimum height made the empty
  // field almost twice as high as one line.
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(inputScroll), GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(inputScroll), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(inputScroll), 160);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(inputScroll), 32);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(inputScroll), _input);
  gtk_widget_set_hexpand(inputScroll, TRUE);
  gtk_widget_set_valign(inputScroll, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(inputScroll, "message-input-frame");
  gtk_box_append(GTK_BOX(field), inputScroll);

  // Placeholder: a label over the empty field.
  const auto known = _store.chat(_chat);
  const auto broadcast = known && known->kind == Td::ChatKind::Channel;
  const auto placeholderText = Tr(broadcast ? "lng_broadcast_ph" : "lng_message_ph");
  auto *placeholder = gtk_label_new(placeholderText.c_str());
  // GtkTextView gives overlay children their minimum size, which an ellipsized label has none of.
  gtk_label_set_width_chars(GTK_LABEL(placeholder), std::min(int(g_utf8_strlen(placeholderText.c_str(), -1)), 24));
  gtk_widget_add_css_class(placeholder, "dim-label");
  gtk_widget_set_can_target(placeholder, FALSE);
  gtk_label_set_xalign(GTK_LABEL(placeholder), 0);
  gtk_label_set_ellipsize(GTK_LABEL(placeholder), PANGO_ELLIPSIZE_END);
  gtk_text_view_add_overlay(GTK_TEXT_VIEW(_input), placeholder, 0, 0);
  auto *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(_input));
  _signals.connect<void(GtkTextBuffer *)>(buffer, "changed", [this, placeholder](GtkTextBuffer *changed) {
    gtk_widget_set_visible(placeholder, gtk_text_buffer_get_char_count(changed) == 0);
    composerChanged();
  });

  _sendButton = MakeIconButton("contrail-send-symbolic", Tr("lng_send_button").c_str(), "suggested-action");
  gtk_widget_add_css_class(_sendButton, "circular");
  gtk_widget_set_valign(_sendButton, GTK_ALIGN_END);
  _signals.clicked(_sendButton, [this] { send(); });
  gtk_box_append(GTK_BOX(field), _sendButton);
  refreshSendButton();

  // Enter sends, Shift+Enter starts a new line.
  auto *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  _signals.connect<gboolean(GtkEventControllerKey *, guint, guint, GdkModifierType)>(
      keys, "key-pressed", [this](GtkEventControllerKey *, guint key, guint, GdkModifierType state) -> gboolean {
        if ((key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) && !(state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK))) {
          send();
          return TRUE;
        }
        if (key == GDK_KEY_Up && composerText().empty() && !_editing) {
          editLast();
          return TRUE;
        }
        return FALSE;
      });
  gtk_widget_add_controller(_input, keys);

  _composerStack = gtk_stack_new();
  gtk_stack_set_hhomogeneous(GTK_STACK(_composerStack), FALSE);
  gtk_stack_set_vhomogeneous(GTK_STACK(_composerStack), FALSE);
  gtk_stack_add_named(GTK_STACK(_composerStack), field, "field");
  _joinButton = gtk_button_new_with_label("");
  gtk_widget_add_css_class(_joinButton, "flat");
  gtk_widget_add_css_class(_joinButton, "join-button");
  _signals.clicked(_joinButton, [this] {
    const auto known = _store.chat(_chat);
    const auto group = known ? _store.group(*known) : nullptr;
    if (group && !group->member) {
      _store.client().send(api::make_object<api::joinChat>(_chat));
    } else {
      if (known) _store.setMuted(_chat, !known->muted);
    }
  });
  gtk_stack_add_named(GTK_STACK(_composerStack), _joinButton, "join");
  gtk_box_append(GTK_BOX(composer), _composerStack);
  adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(_root), composer);
}

void ChatView::refreshComposerState() {
  const auto known = _store.chat(_chat);
  if (!known) return;
  const auto group = _store.group(*known);
  const auto broadcast = known->kind == Td::ChatKind::Channel;
  if (group && !group->member) {
    gtk_button_set_label(GTK_BUTTON(_joinButton),
                         Tr(broadcast ? "lng_profile_join_channel" : "lng_profile_join_group").c_str());
    gtk_stack_set_visible_child_name(GTK_STACK(_composerStack), "join");
  } else if (!_store.canWrite(*known)) {
    gtk_button_set_label(GTK_BUTTON(_joinButton), Tr(known->muted ? "lng_channel_unmute" : "lng_channel_mute").c_str());
    gtk_stack_set_visible_child_name(GTK_STACK(_composerStack), "join");
  } else {
    gtk_stack_set_visible_child_name(GTK_STACK(_composerStack), "field");
  }
}

std::string ChatView::composerText() const {
  auto *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(_input));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  auto *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  auto result = std::string(text);
  g_free(text);
  return result;
}

void ChatView::setComposerText(const std::string &text) {
  auto *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(_input));
  _settingComposerText = true;
  gtk_text_buffer_set_text(buffer, text.data(), int(text.size()));
  _settingComposerText = false;
}

void ChatView::refreshSendButton() {
  const auto empty = gtk_text_buffer_get_char_count(gtk_text_view_get_buffer(GTK_TEXT_VIEW(_input))) == 0;
  gtk_widget_set_sensitive(_sendButton, !empty && !_editPending);
}

void ChatView::composerChanged() {
  refreshSendButton();
  if (_settingComposerText) return;
  _draftTimer.callOnce(kDraftDelay);
  // Telegram shows "typing" for a few seconds from one request.
  const auto now = g_get_monotonic_time();
  if (composerText().empty() || now - _lastTyping < kTypingPeriod) return;
  _lastTyping = now;
  _store.client().send(api::make_object<api::sendChatAction>(_chat, nullptr, std::string(),
                                                             api::make_object<api::chatActionTyping>()));
}

// The text being written goes to Telegram, and with it to the other devices. An edit is no draft.
void ChatView::saveDraft() {
  _draftTimer.cancel();
  if (_editing) return;
  const auto text = composerText();
  if (text == _savedDraft && _replyToId == _savedDraftReply) return;
  _savedDraft = text;
  _savedDraftReply = _replyToId;
  auto draft = api::object_ptr<api::draftMessage>();
  if (!text.empty() || _replyToId) {
    draft = api::make_object<api::draftMessage>(
        ReplyTo(_replyToId), 0, api::make_object<api::draftMessageContentText>(Formatted(text), nullptr), 0, nullptr);
  }
  _store.client().send(api::make_object<api::setChatDraftMessage>(_chat, nullptr, std::move(draft)));
}

// Written on another device, while this field has nothing of its own.
void ChatView::draftChanged() {
  const auto known = _store.chat(_chat);
  if (!known || known->draft == _savedDraft || _editing || _draftTimer.active()) return;
  if (composerText() != _savedDraft) return;
  _savedDraft = known->draft;
  _savedDraftReply = known->draftReplyTo;
  setComposerText(known->draft);
}

void ChatView::showReplyBar(const std::string &title, const std::string &text, const char *icon) {
  gtk_image_set_from_icon_name(GTK_IMAGE(_replyIcon), icon);
  gtk_label_set_text(GTK_LABEL(_replyTitle), title.c_str());
  gtk_label_set_text(GTK_LABEL(_replyText), Td::OneLine(text).c_str());
  gtk_widget_set_visible(_replyBar, TRUE);
  gtk_widget_grab_focus(_input);
}

void ChatView::replyTo(const MessageItem &item) {
  if (item.local()) return;
  if (_editing) cancelReplyOrEdit();
  _replyToId = item.id();
  auto name = std::string();
  auto color = 0;
  DescribeSender(_store, *item.data, name, color);
  showReplyBar(name, Td::MessagePreview(*item.data, _store), "mail-reply-sender-symbolic");
  _draftTimer.callOnce(kDraftDelay);
}

void ChatView::edit(const MessageItem &item) {
  if (_editPending) return;
  if (!_editing) {
    _beforeEditText = composerText();
    _beforeEditReplyTo = _replyToId;
  }
  _draftTimer.cancel();
  _replyToId = 0;
  _editing = item.id();
  const auto text = PlainText(*item.data);
  setComposerText(text);
  showReplyBar(Tr("lng_edit_message"), text, "document-edit-symbolic");
}

// The up arrow in an empty field: the last message that can still be edited.
void ChatView::editLast() {
  for (auto i = _items.rbegin(); i != _items.rend(); ++i) {
    const auto &item = **i;
    if (item.local() || !item.data->is_outgoing_ || PlainText(*item.data).empty()) continue;
    const auto id = item.id();
    _store.client().request<api::messageProperties>(
        api::make_object<api::getMessageProperties>(_chat, id), _lifetime,
        [this, id](api::messageProperties &properties) {
          if (const auto found = find(id); found && properties.can_be_edited_) edit(*found);
        },
        [](api::error &) {});
    return;
  }
}

void ChatView::cancelReplyOrEdit() {
  const auto wasEditing = _editing != 0;
  _replyToId = 0;
  _editing = 0;
  _editPending = false;
  gtk_text_view_set_editable(GTK_TEXT_VIEW(_input), TRUE);
  gtk_widget_set_visible(_replyBar, FALSE);
  if (wasEditing) {
    setComposerText(std::exchange(_beforeEditText, std::string()));
    if (const auto reply = std::exchange(_beforeEditReplyTo, 0)) {
      if (const auto item = find(reply)) replyTo(*item);
    }
  } else {
    _draftTimer.callOnce(kDraftDelay);
  }
  refreshSendButton();
}

bool ChatView::handleEscape() {
  if (!_editing && !_replyToId) return false;
  cancelReplyOrEdit();
  return true;
}

void ChatView::applyEdit(const std::string &text) {
  const auto item = find(_editing);
  if (!item) {
    cancelReplyOrEdit();
    return;
  }
  _editPending = true;
  refreshSendButton();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(_input), FALSE);
  const auto editing = _editing;
  const auto finished = [this, editing] {
    if (_editing == editing) cancelReplyOrEdit();
  };
  const auto failed = [this, editing, finished](api::error &error) {
    if (_editing != editing) return;
    if (error.message_ == "MESSAGE_NOT_MODIFIED") {
      finished();
      return;
    }
    _editPending = false;
    gtk_text_view_set_editable(GTK_TEXT_VIEW(_input), TRUE);
    refreshSendButton();
    ShowToast(error.message_);
  };
  auto function = Td::FunctionPtr();
  if (item->data->content_->get_id() == api::messageText::ID) {
    function = api::make_object<api::editMessageText>(
        _chat, editing, nullptr, api::make_object<api::inputMessageText>(Formatted(text), nullptr, false));
  } else {
    function = api::make_object<api::editMessageCaption>(_chat, editing, nullptr, Formatted(text), false);
  }
  _store.client().request<api::message>(std::move(function), _lifetime,
                                        [finished](api::message &) { finished(); }, failed);
}

void ChatView::send() {
  if (_editPending) return;
  const auto known = _store.chat(_chat);
  if (!known || !_store.canWrite(*known)) return;
  auto text = composerText();
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return;
  text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
  if (_editing) {
    applyEdit(text);
    return;
  }
  _store.client().send(api::make_object<api::sendMessage>(_chat, nullptr, ReplyTo(_replyToId), nullptr, nullptr,
                                                          InputText(text)));
  setComposerText(std::string());
  _draftTimer.cancel();
  // clear_draft has done it on the server.
  _savedDraft.clear();
  _savedDraftReply = 0;
  cancelReplyOrEdit();
  _draftTimer.cancel();
  if (_reachedBottom) scrollToBottom();
  else reload(0);
}

void ChatView::sendText(const std::string &text) {
  const auto known = _store.chat(_chat);
  if (!known || !_store.canWrite(*known) || text.empty()) return;
  _store.client().send(api::make_object<api::sendMessage>(_chat, nullptr, nullptr, nullptr, nullptr, InputText(text)));
  if (_reachedBottom) scrollToBottom();
  else reload(0);
}

// --- the menu of a message -----------------------------------------------------

void ChatView::showMessageMenu(const MessageItem &item, GtkWidget *widget, double x, double y) {
  const auto id = item.id();
  // What may be done with a message is TDLib's to say. A message being sent has nothing to ask about.
  const auto show = [this, id, widget, x, y](bool canEdit, bool forAll, bool onlyForSelf) {
    const auto found = find(id);
    if (!found || !gtk_widget_get_mapped(widget)) return;
    const auto known = _store.chat(_chat);
    const auto failed = Cast<api::messageSendingStateFailed>(found->data->sending_state_.get());
    const auto service = Td::IsServiceMessage(*found->data->content_);
    const auto text = PlainText(*found->data);
    auto *menu = g_menu_new();
    if (failed) g_menu_append(menu, Tr("lng_bot_download_retry").c_str(), "msg.retry");
    if (!found->local() && !service && known && _store.canWrite(*known)) {
      g_menu_append(menu, Tr("lng_context_reply_msg").c_str(), "msg.reply");
    }
    if (!text.empty()) g_menu_append(menu, Tr("lng_context_copy_text").c_str(), "msg.copy");
    if (canEdit) g_menu_append(menu, Tr("lng_context_edit_msg").c_str(), "msg.edit");
    if (forAll || onlyForSelf) g_menu_append(menu, Tr("lng_context_delete_msg").c_str(), "msg.delete");
    if (!g_menu_model_get_n_items(G_MENU_MODEL(menu))) {
      g_object_unref(menu);
      return;
    }
    auto *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    g_object_unref(menu);
    gtk_widget_set_parent(popover, widget);
    const GdkRectangle rect = {int(x), int(y), 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
    DestroyPopoverOnClose(popover);
    auto *group = g_simple_action_group_new();
    const auto add = [&](const char *name, std::function<void()> callback) {
      auto *action = g_simple_action_new(name, nullptr);
      Connect<void(GSimpleAction *, GVariant *)>(action, "activate",
                                                 [callback](GSimpleAction *, GVariant *) { callback(); });
      g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
      g_object_unref(action);
    };
    add("retry", [this, id] {
      _store.client().send(
          api::make_object<api::resendMessages>(_chat, std::vector<std::int64_t>{id}, nullptr, 0));
    });
    add("reply", [this, id] {
      if (const auto item = find(id)) replyTo(*item);
    });
    add("copy", [text] { CopyToClipboard(text); });
    add("edit", [this, id] {
      if (const auto item = find(id)) edit(*item);
    });
    add("delete", [this, id, forAll, onlyForSelf] { confirmDelete(id, forAll, onlyForSelf); });
    gtk_widget_insert_action_group(popover, "msg", G_ACTION_GROUP(group));
    g_object_unref(group);
    gtk_popover_popup(GTK_POPOVER(popover));
  };
  if (item.local()) {
    show(false, false, true);
    return;
  }
  _store.client().request<api::messageProperties>(
      api::make_object<api::getMessageProperties>(_chat, id), _lifetime,
      [show](api::messageProperties &properties) {
        show(properties.can_be_edited_, properties.can_be_deleted_for_all_users_,
             properties.can_be_deleted_only_for_self_);
      },
      [show](api::error &) { show(false, false, false); });
}

void ChatView::confirmDelete(std::int64_t id, bool forAll, bool onlyForSelf) {
  auto *dialog = adw_alert_dialog_new(Tr("lng_selected_delete_sure_this").c_str(), nullptr);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", Tr("lng_cancel").c_str(), "delete",
                                 Tr("lng_box_delete").c_str(), nullptr);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  GtkWidget *revoke = nullptr;
  if (forAll && onlyForSelf) {
    revoke = gtk_check_button_new_with_label(Tr("lng_delete_for_everyone_check").c_str());
    gtk_check_button_set_active(GTK_CHECK_BUTTON(revoke), TRUE);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), revoke);
  }
  auto &client = _store.client();
  const auto chat = _chat;
  Connect<void(AdwAlertDialog *, const char *)>(dialog, "response", [=, &client](AdwAlertDialog *, const char *response) {
    if (g_strcmp0(response, "delete") != 0) return;
    // Channels and supergroups: deletion is always for everyone.
    const auto all = revoke ? bool(gtk_check_button_get_active(GTK_CHECK_BUTTON(revoke))) : forAll;
    client.send(api::make_object<api::deleteMessages>(chat, std::vector<std::int64_t>{id}, all));
  });
  adw_dialog_present(ADW_DIALOG(dialog), _window.widget());
}

// --- what the messages ask for -------------------------------------------------

void ChatView::openChat(Td::ChatId chat) {
  _window.openChat(chat);
}

// There is no photo viewer yet: the photo opens outside, once the row has it.
void ChatView::openPhoto(const MessageItem &, Td::FileId file) {
  if (const auto known = _store.file(file); known && known->complete) OpenPath(known->path);
}

void ChatView::openDocument(const MessageItem &, Td::FileId file) {
  const auto known = _store.file(file);
  if (known && known->complete) {
    OpenPath(known->path);
  } else if (known && known->downloading) {
    _store.cancelDownload(file);
  } else {
    _store.download(file, 32);
  }
}

bool ChatView::opensWhenLoaded(Td::FileId) const {
  return false;
}

// --- debug ---------------------------------------------------------------------

void ChatView::debugState() const {
  const auto value = gtk_adjustment_get_value(_adjustment);
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  const auto upper = gtk_adjustment_get_upper(_adjustment);
  const auto oldest = oldestId(), newest = newestId();
  g_message("chat state: messages=%zu rows=%zu oldest=%lld newest=%lld top=%d bottom=%d stick=%d reserve=%d value=%.0f "
            "page=%.0f upper=%.0f toBottom=%d replyTo=%lld editing=%lld draft=\"%s\"",
            _items.size(), _rows.size(), (long long)(oldest >> 20), (long long)(newest >> 20), int(_reachedTop),
            int(_reachedBottom), int(_stickToBottom), _topReserve, value, page, upper,
            int(gtk_widget_get_visible(_toBottom)), (long long)(_replyToId >> 20), (long long)(_editing >> 20),
            _savedDraft.c_str());
}

void ChatView::debugScroll(const std::string &where) {
  _scrollTarget = 0;
  _stickToBottom = false;
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  if (where == "top") {
    gtk_adjustment_set_value(_adjustment, 0);
  } else if (where == "bottom") {
    gtk_adjustment_set_value(_adjustment, gtk_adjustment_get_upper(_adjustment) - page);
  } else {
    gtk_adjustment_set_value(_adjustment, gtk_adjustment_get_value(_adjustment) + std::atof(where.c_str()));
  }
}

void ChatView::debugSend(const std::string &text) {
  setComposerText(text);
  send();
}

void ChatView::debugReply() {
  if (const auto newest = find(newestId())) replyTo(*newest);
}

void ChatView::debugEdit(const std::string &text) {
  for (auto i = _items.rbegin(); i != _items.rend(); ++i) {
    if ((*i)->data->is_outgoing_ && !(*i)->local()) {
      edit(**i);
      debugSend(text);
      return;
    }
  }
}

void ChatView::debugDelete() {
  if (const auto newest = newestId()) {
    _store.client().send(api::make_object<api::deleteMessages>(_chat, std::vector<std::int64_t>{newest}, true));
  }
}

}  // namespace Contrail
