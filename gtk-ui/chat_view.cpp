#include "chat_view.hpp"

#include "main_window.hpp"
#include "message.hpp"
#include "scroll_helpers.hpp"
#include "text.hpp"
#include "userpics.hpp"

#include "api/api_common.h"
#include "api/api_editing.h"
#include "api/api_sending.h"
#include "apiwrap.h"
#include "base/unixtime.h"
#include "chat_helpers/message_field.h"
#include "core/application.h"
#include "core/mime_type.h"
#include "window/notifications_manager.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_chat_participant_status.h"
#include "data/components/sponsored_messages.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "data/data_histories.h"
#include "data/data_replies_list.h"
#include "data/data_peer_values.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_send_action.h"
#include "history/view/history_view_send_action.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/notify/data_notify_settings.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "history/view/history_view_item_preview.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/audio/media_audio_capture.h"
#include "media/audio/media_audio_capture_common.h"
#include "media/player/media_player_instance.h"
#include "ui/text/format_values.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/image/image.h"
#include "ui/text/text_utilities.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>

namespace Contrail {
namespace {

constexpr auto kMessagesPerPage = 50;
constexpr auto kPreloadHeight = 6000.;  // About eight phone screens: pages come before a fling gets there.
// Room above the messages for older pages: a page of a channel with photos is up to ~20000px high,
// so this lasts ten pages of flinging without a stop. Much larger and float rounding of the
// render transforms starts to blur the text.
constexpr auto kTopReserve = 200000;
constexpr auto kTopReserveLow = 100000;
// Voice messages are recorded while the microphone button is held, as in the phone apps.
constexpr auto kRecordCancelDistance = 120.;  // Slide left to cancel.
constexpr auto kRecordLockDistance = 90.;     // Slide up to keep recording without holding.
constexpr auto kRecordMinHold = crl::time(400);
constexpr auto kRecordHintMargin = 16;  // "Slide to cancel" right of the timer, left of the microphone.

QString UniqueDownloadPath(const QString &name) {
  auto dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  if (dir.isEmpty()) {
    dir = QDir::homePath();
  }
  QDir().mkpath(dir);
  const auto info = QFileInfo(name.isEmpty() ? u"file"_q : name);
  auto path = dir + '/' + info.fileName();
  for (auto i = 1; QFileInfo::exists(path); ++i) {
    path = dir + '/' + info.completeBaseName() + u" ("_q + QString::number(i) + ')'
           + (info.suffix().isEmpty() ? QString() : '.' + info.suffix());
  }
  return path;
}

void LaunchFile(const QString &path) {
  auto *file = g_file_new_for_path(path.toUtf8().constData());
  auto *launcher = gtk_file_launcher_new(file);
  auto *parent = GetApp().window ? GTK_WINDOW(GetApp().window->widget()) : nullptr;
  gtk_file_launcher_launch(launcher, parent, nullptr, nullptr, nullptr);
  g_object_unref(launcher);
  g_object_unref(file);
}

}  // namespace

ChatView::ChatView(not_null<Data::Thread *> thread, not_null<MainWindow *> window, MsgId showAt)
    : _thread(thread), _history(thread->owningHistory()), _topic(thread->asTopic()), _window(window),
      _pendingShowAt(showAt), _readTimer([=] { markAsRead(); }),
      _statusTimer([=] { refreshHeader(); }), _reserveTimer([=] { maintainTopReserve(); }) {
  _root = adw_toolbar_view_new();
  g_object_ref_sink(_root);
  adw_toolbar_view_set_bottom_bar_style(ADW_TOOLBAR_VIEW(_root), ADW_TOOLBAR_RAISED);
  buildHeader();
  buildList();
  buildComposer();
  refreshHeader();
  refreshComposerState();
  _statusTimer.callEach(30 * 1000);
  _signals.connect<void(GtkWidget *)>(_root, "map", [=](GtkWidget *) { scheduleViewportUpdate(); });

  const auto session = &_history->session();
  auto &data = session->data();
  data.newItemAdded() | rpl::filter([=](not_null<HistoryItem *> item) {
    return item->history() == _history && (!_topic || item->topicRootId() == _topic->rootId());
  }) | rpl::on_next([=](not_null<HistoryItem *> item) {
          // Seen right away while the chat is in front (HistoryWidget::newItemAdded): no notification.
          if (item->showNotification() && _stickToBottom && _window->isActive()
              && _window->currentThread() == _thread) {
            if (_topic) {
              // The topic's list gets the message a moment later: read when it is shown (markAsRead).
              Core::App().notifications().clearFromTopic(_topic);
            } else {
              _history->owner().histories().readInboxOnNewMessage(item);
              Core::App().notifications().clearFromHistory(_history);
            }
          }
          if (!_syncScheduled) {
            _syncScheduled = true;
            crl::on_main(this, [=] {
              _syncScheduled = false;
              sync();
            });
          }
        }, _lifetime);
  data.itemRemoved() | rpl::on_next([=](not_null<const HistoryItem *> item) { removeRow(item); }, _lifetime);
  data.itemDataChanges() | rpl::on_next([=](not_null<HistoryItem *> item) { refreshItem(item); }, _lifetime);
  data.itemViewRefreshRequest() | rpl::on_next([=](not_null<const HistoryItem *> item) { refreshItem(item); }, _lifetime);
  session->changes().messageUpdates(Data::MessageUpdate::Flag::Edited | Data::MessageUpdate::Flag::NewAdded)
      | rpl::on_next([=](const Data::MessageUpdate &update) { refreshItem(update.item); }, _lifetime);
  using HistoryFlag = Data::HistoryUpdate::Flag;
  session->changes().historyUpdates(_history, HistoryFlag::OutboxRead | HistoryFlag::MessageSent)
      | rpl::on_next([=] {
          for (const auto &[item, row] : _rows) {
            row->refreshFooter();
          }
          sync();
        }, _lifetime);
  using PeerFlag = Data::PeerUpdate::Flag;
  session->changes().peerUpdates(_history->peer, PeerFlag::Name | PeerFlag::OnlineStatus | PeerFlag::Members
                                                     | PeerFlag::Rights | PeerFlag::ChannelAmIn | PeerFlag::Notifications)
      | rpl::on_next([=] {
          refreshHeader();
          refreshComposerState();
        }, _lifetime);
  if (_topic) {
    using TopicFlag = Data::TopicUpdate::Flag;
    session->changes().topicUpdates(_topic, TopicFlag::Title | TopicFlag::Closed | TopicFlag::Notifications)
        | rpl::on_next([=] {
            refreshHeader();
            refreshComposerState();
          }, _lifetime);
    // A deleted topic: nothing left to show.
    _topic->destroyed() | rpl::on_next([=] {
      crl::on_main(_window.get(), [window = _window, thread = _thread] {
        if (window->currentThread() == thread) window->closeChat();
      });
    }, _lifetime);
  }
  data.sendActionManager().animationUpdated()
      | rpl::filter([=](const Data::SendActionManager::AnimationUpdate &update) {
          return update.textUpdated && update.thread == _thread.get();
        })
      | rpl::on_next([=] { refreshHeader(); }, _lifetime);
  session->downloaderTaskFinished() | rpl::on_next([=] {
    if (_sponsoredMediaLoading) {
      showSponsored();
    }
    for (auto i = _waitingMedia.begin(); i != _waitingMedia.end();) {
      if ((*i)->refreshMedia()) {
        i = _waitingMedia.erase(i);
      } else {
        ++i;
      }
    }
    for (auto i = _openWhenLoaded.begin(); i != _openWhenLoaded.end();) {
      const auto path = (*i)->filepath(true);
      if (!path.isEmpty()) {
        LaunchFile(path);
        i = _openWhenLoaded.erase(i);
      } else if (!(*i)->loading()) {
        i = _openWhenLoaded.erase(i);
      } else {
        ++i;
      }
    }
  }, _lifetime);
  Media::Player::instance()->updatedNotifier() | rpl::on_next([=](const Media::Player::TrackState &state) {
    for (const auto &[item, row] : _rows) {
      row->updatePlayback(state);
    }
  }, _lifetime);

  // Read the chat while the window is in front.
  _signals.connect<void(GObject *, GParamSpec *)>(_window->widget(), "notify::is-active", [this](GObject *, GParamSpec *) {
    if (_window->isActive()) {
      _readTimer.callOnce(300);
    }
  });

  // Opening a chat with unread messages starts at the first of them, under an "Unread messages" bar.
  if (_pendingShowAt) {
  } else if (_topic) {
    const auto replies = _topic->replies();
    if (const auto readTill = replies->computeInboxReadTillFull();
        replies->unreadCountKnown() && replies->unreadCountCurrent() > 0 && readTill > 0) {
      _unreadAfter = readTill;
      _pendingShowAt = _unreadAfter + 1;
    }
  } else if (_history->unreadCount() > 0 && _history->inboxReadTillId() > 0) {
    _unreadAfter = _history->inboxReadTillId();
    _pendingShowAt = _unreadAfter + 1;
  }
  g_object_set_data(G_OBJECT(_list), "contrail-unread-after", GINT_TO_POINTER(int(_unreadAfter.bare)));

  loadInitial();
}

ChatView::~ChatView() {
  invalidate_weak_ptrs(this);
  // Like HistoryWidget: the sponsored messages shown here leave the history with the chat.
  _history->session().sponsoredMessages().clearItems(_history);
  if (_viewportSource) g_source_remove(_viewportSource);
  _readTimer.cancel();
  _statusTimer.cancel();
  _reserveTimer.cancel();
  _signals.clear();
  // Removing rows must not query messages that are being destroyed.
  gtk_list_box_set_header_func(GTK_LIST_BOX(_list), nullptr, nullptr, nullptr);
  gtk_list_box_set_sort_func(GTK_LIST_BOX(_list), nullptr, nullptr, nullptr);
  if (_recording) {
    _recordLifetime.destroy();
    ::Media::Capture::instance()->stop();
  }
  _topicViewer.destroy();
  _lifetime.destroy();
  if (_olderRequest) {
    _history->owner().histories().cancelRequest(_olderRequest);
  }
  if (_newerRequest) {
    _history->owner().histories().cancelRequest(_newerRequest);
  }
  // The draft stays with the chat, as in Telegram Desktop; saved only when it was changed here.
  const auto text = _editing ? _beforeEditText : composerText();
  const auto reply = _editing ? _beforeEditReplyTo : _replyTo;
  if (text != _loadedDraft || reply != _loadedReplyTo) {
    if (text.trimmed().isEmpty()) {
      _history->clearLocalDraft(_thread->topicRootId(), _thread->monoforumPeerId());
    } else {
      auto replyTo = FullReplyTo{reply};
      replyTo.topicRootId = _thread->topicRootId();
      replyTo.monoforumPeerId = _thread->monoforumPeerId();
      _history->setLocalDraft(std::make_unique<Data::Draft>(
          TextWithTags{text, {}}, replyTo, SuggestOptions(), MessageCursor(), Data::WebPageDraft()));
    }
    _history->session().api().saveDraftToCloudDelayed(_thread);
  }
  _waitingMedia.clear();
  _rows.clear();
  g_object_unref(_root);
}

// --- header ------------------------------------------------------------------

void ChatView::buildHeader() {
  auto *header = adw_header_bar_new();
  _title = adw_window_title_new("", "");
  auto *titleButton = gtk_button_new();
  gtk_button_set_child(GTK_BUTTON(titleButton), _title);
  gtk_widget_add_css_class(titleButton, "flat");
  _signals.clicked(titleButton, [this] { _window->openProfile(_history->peer); });
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), titleButton);

  auto *avatar = MakeAvatar(_history->peer, 32, true);
  auto *avatarButton = gtk_button_new();
  gtk_button_set_child(GTK_BUTTON(avatarButton), avatar);
  gtk_widget_add_css_class(avatarButton, "flat");
  gtk_widget_add_css_class(avatarButton, "circular");
  _signals.clicked(avatarButton, [this] { _window->openProfile(_history->peer); });
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), avatarButton);
  if (const auto user = _history->peer->asUser(); user && !user->isSelf() && !user->isBot()) {
    auto *call = MakeIconButton("call-start-symbolic", U8(tr::lng_call_start(tr::now)).c_str(), "flat");
    _signals.clicked(call, [=] { _window->startCall(_history->peer); });
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), call);
  }
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(_root), header);
}

void ChatView::refreshHeader() {
  const auto peer = _history->peer;
  auto name = peer->isSelf() ? tr::lng_saved_messages(tr::now) : peer->name();
  QString status;
  if (_topic) {
    // The topic, and the group it is in.
    name = _topic->title();
    status = peer->name();
  } else if (const auto user = peer->asUser()) {
    if (user->isSelf()) {
      status = QString();
    } else if (user->isBot()) {
      status = tr::lng_status_bot(tr::now);
    } else {
      status = Data::OnlineText(user, base::unixtime::now());
    }
  } else if (const auto chat = peer->asChat()) {
    status = tr::lng_chat_status_members(tr::now, lt_count, chat->count);
  } else if (const auto channel = peer->asChannel()) {
    const auto count = channel->membersCount();
    if (count > 0) {
      status = channel->isBroadcast() ? tr::lng_chat_status_subscribers(tr::now, lt_count, count)
                                      : tr::lng_chat_status_members(tr::now, lt_count, count);
    }
  }
  // "typing...", "sending a photo..." while someone does.
  if (const auto action = _thread->sendActionPainter()->actionText(); !action.isEmpty()) {
    status = action;
  }
  adw_window_title_set_title(ADW_WINDOW_TITLE(_title), U8(name).c_str());
  adw_window_title_set_subtitle(ADW_WINDOW_TITLE(_title), U8(status).c_str());
}

// --- messages ----------------------------------------------------------------

void ChatView::buildList() {
  _list = gtk_list_box_new();
  gtk_widget_add_css_class(_list, "message-list");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(_list), GTK_SELECTION_NONE);
  gtk_widget_set_valign(_list, GTK_ALIGN_END);
  // Order of the messages in the chat.
  gtk_list_box_set_sort_func(
      GTK_LIST_BOX(_list),
      +[](GtkListBoxRow *a, GtkListBoxRow *b, gpointer data) -> int {
        auto *ma = static_cast<MessageWidget *>(g_object_get_data(G_OBJECT(a), "contrail-message"));
        auto *mb = static_cast<MessageWidget *>(g_object_get_data(G_OBJECT(b), "contrail-message"));
        if (!ma || !mb) {
          return 0;
        }
        const auto ia = ma->item(), ib = mb->item();
        // A sponsored message has a local id: it goes right after the message it follows in the history.
        if (ia->isSponsored() || ib->isSponsored()) {
          const auto &after = static_cast<ChatView *>(data)->_sponsoredAfter;
          const auto key = [&](const HistoryItem *item) {
            const auto i = after.find(item);
            return (i != after.end()) ? std::pair(i->second, 1) : std::pair(item->id, 0);
          };
          const auto ka = key(ia), kb = key(ib);
          return (ka < kb) ? -1 : (kb < ka) ? 1 : 0;
        }
        // Messages being sent have local ids: they go after the others.
        const auto la = ia->isSending() || ia->hasFailed(), lb = ib->isSending() || ib->hasFailed();
        if (la != lb) {
          return la ? 1 : -1;
        }
        if (la && ia->date() != ib->date()) {
          return ia->date() < ib->date() ? -1 : 1;
        }
        // Server ids grow with time within a chat.
        return (ia->id < ib->id) ? -1 : (ia->id > ib->id) ? 1 : 0;
      },
      this, nullptr);
  // Date separators and grouping of consecutive messages from the same sender.
  gtk_list_box_set_header_func(
      GTK_LIST_BOX(_list),
      +[](GtkListBoxRow *row, GtkListBoxRow *before, gpointer) {
        auto *m = static_cast<MessageWidget *>(g_object_get_data(G_OBJECT(row), "contrail-message"));
        auto *p = before ? static_cast<MessageWidget *>(g_object_get_data(G_OBJECT(before), "contrail-message")) : nullptr;
        if (!m) {
          return;
        }
        const auto item = m->item();
        const auto day = base::unixtime::parse(item->date()).date();
        const auto newDay = !p || base::unixtime::parse(p->item()->date()).date() != day;
        const auto unreadAfter = MsgId(GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(gtk_widget_get_parent(GTK_WIDGET(row))), "contrail-unread-after")));
        const auto unreadBar = unreadAfter && !item->out() && item->id > unreadAfter && item->isRegular()
                               && (!p || p->item()->id <= unreadAfter);
        // Headers are recomputed for all the rows on every change of the list: keep the ones that
        // stay the same instead of creating new widgets (and a new layout) each time.
        const auto key = (newDay || unreadBar) ? gsize(day.toJulianDay() * 2 + (unreadBar ? 1 : 0)) : gsize(0);
        const auto same = (key == GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(row), "contrail-header-key")))
                          && (!key || gtk_list_box_row_get_header(row));
        g_object_set_data(G_OBJECT(row), "contrail-header-key", GSIZE_TO_POINTER(key));
        if (same) {
        } else if (newDay || unreadBar) {
          auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
          if (newDay) {
            auto *label = gtk_label_new(U8(FormatDay(item->date())).c_str());
            gtk_widget_add_css_class(label, "date-separator");
            gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(box), label);
          }
          if (unreadBar) {
            auto *label = gtk_label_new(U8(tr::lng_unread_bar_some(tr::now)).c_str());
            gtk_widget_add_css_class(label, "unread-bar");
            gtk_box_append(GTK_BOX(box), label);
          }
          gtk_list_box_row_set_header(row, box);
        } else {
          gtk_list_box_row_set_header(row, nullptr);
        }
        const auto sameSender = !newDay && p && !p->item()->isService() && !item->isService()
                                && p->item()->from() == item->from() && item->date() - p->item()->date() < 15 * 60;
        m->setFirstInGroup(!sameSender);
      },
      nullptr, nullptr);

  auto *clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 720);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 560);
  adw_clamp_set_child(ADW_CLAMP(clamp), _list);

  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  // Room for the older messages above the loaded ones (see setTopReserve).
  _topSpacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(box), _topSpacer);
  // The spinner and the retry button float over the messages: in the list they would move them.
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
  _retry = gtk_button_new_with_label(U8(tr::lng_bot_download_retry(tr::now)).c_str());
  gtk_widget_add_css_class(_retry, "osd");
  gtk_widget_add_css_class(_retry, "pill");
  gtk_widget_set_halign(_retry, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(_retry, GTK_ALIGN_START);
  gtk_widget_set_margin_top(_retry, 10);
  gtk_widget_set_visible(_retry, FALSE);
  _signals.clicked(_retry, [=] {
    _loadFailed = false;
    _emptyReloadTried = false;
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
      [=](GtkEventControllerLegacy *, GdkEvent *event) -> gboolean {
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
          _scrollTarget = FullMsgId();
          _stickToBottom = false;
        }
        if (g_getenv("CONTRAIL_DEBUG") && (type == GDK_TOUCH_BEGIN || type == GDK_TOUCH_END)) {
          g_message("Chat touch: %s", type == GDK_TOUCH_BEGIN ? "begin" : "end");
        }
        return FALSE;
      });
  gtk_widget_add_controller(_scroll, touch);
  _adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(_scroll));
  _signals.connect<void(GtkAdjustment *)>(_adjustment, "value-changed", [this](GtkAdjustment *) {
    if (!_adjustingScroll) NoteListMotion();
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
  _signals.connect<void(GObject *, GParamSpec *)>(_adjustment, "notify::page-size", [this](GObject *, GParamSpec *) {
    scheduleViewportUpdate();
  });

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
    if (!loadedAtBottom()) {
      // Jumped somewhere into the past: reload the end of the chat.
      reload(0);
    } else {
      scrollToBottom();
    }
  });
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), _toBottom);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), _loading);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), _retry);
  // Slide up to lock: shown above the microphone while a voice message is being recorded.
  _lockHint = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class(_lockHint, "osd");
  gtk_widget_add_css_class(_lockHint, "record-lock");
  gtk_box_append(GTK_BOX(_lockHint), gtk_image_new_from_icon_name("changes-prevent-symbolic"));
  gtk_box_append(GTK_BOX(_lockHint), gtk_image_new_from_icon_name("go-up-symbolic"));
  gtk_widget_set_tooltip_text(_lockHint, U8(tr::lng_record_lock(tr::now)).c_str());
  gtk_widget_set_halign(_lockHint, GTK_ALIGN_END);
  gtk_widget_set_valign(_lockHint, GTK_ALIGN_END);
  gtk_widget_set_margin_end(_lockHint, 12);
  gtk_widget_set_can_target(_lockHint, FALSE);
  gtk_widget_set_visible(_lockHint, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), _lockHint);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(_root), overlay);
}

void ChatView::loadInitial() {
  if (_olderRequest || _newerRequest || _applyingPage) return;
  if (_topic) {
    _topicAround = (_pendingShowAt > 0)
        ? Data::MessagePosition{.fullId = FullMsgId(_history->peer->id, _pendingShowAt)}
        : Data::MaxMessagePosition;
    _topicLimitBefore = _topicLimitAfter = kMessagesPerPage;
    viewTopic();
    return;
  }
  if (_pendingShowAt > 0) {
    if (_history->owner().message(_history->peer, _pendingShowAt)) {
      sync();
      if (scrollToLoaded(_pendingShowAt)) {
        _initialLoaded = true;
        _pendingShowAt = 0;
        return;
      }
    }
    _history->getReadyFor(_pendingShowAt);
    requestHistory(_pendingShowAt, -kMessagesPerPage / 2, kMessagesPerPage, true, true);
    return;
  }
  if (!_history->isEmpty() && _history->loadedAtBottom()) {
    _initialLoaded = true;
    sync();
    setTopReserve(_history->loadedAtTop() ? 0 : kTopReserve, 0);
    scrollToBottom();
    _readTimer.callOnce(500);
    if (_rows.size() < 20) {
      loadOlder();
    }
    return;
  }
  _history->getReadyFor(ShowAtTheEndMsgId);
  requestHistory(0, 0, kMessagesPerPage, true, true);
}

void ChatView::loadOlder() {
  if (_olderRequest || _newerRequest || _applyingPage || _loadFailed || !_initialLoaded || loadedAtTop()) {
    return;
  }
  if (_topic) {
    if (!_topicOlderWaiting) {
      _topicOlderWaiting = true;
      _topicLimitBefore += kMessagesPerPage;
      viewTopic();
    }
    return;
  }
  const auto minId = _history->minMsgId();
  if (!minId) {
    return;
  }
  requestHistory(minId, 0, kMessagesPerPage, true, false);
}

void ChatView::loadNewer() {
  if (_olderRequest || _newerRequest || _applyingPage || _loadFailed || !_initialLoaded || loadedAtBottom()) {
    return;
  }
  if (_topic) {
    if (!_topicNewerWaiting) {
      _topicNewerWaiting = true;
      _topicLimitAfter += kMessagesPerPage;
      viewTopic();
    }
    return;
  }
  const auto maxId = _history->maxMsgId();
  if (!maxId) {
    return;
  }
  requestHistory(maxId + 1, -kMessagesPerPage, kMessagesPerPage, false, false);
}

// HistoryWidget::firstLoadMessages() / loadMessages() / loadMessagesDown().
void ChatView::requestHistory(MsgId offsetId, int addOffset, int limit, bool older, bool initial) {
  const auto history = _history;
  const auto weak = base::make_weak(this);
  const auto epoch = _loadEpoch;
  gtk_widget_set_visible(_loading, TRUE);
  gtk_widget_set_visible(_retry, FALSE);
  auto &request = older ? _olderRequest : _newerRequest;
  request = history->owner().histories().sendRequest(
      history, Data::Histories::RequestType::History, [=](Fn<void()> finish) {
        return history->session().api()
            .request(MTPmessages_GetHistory(history->peer->input(), MTP_int(offsetId), MTP_int(0), MTP_int(addOffset),
                                            MTP_int(limit), MTP_int(0), MTP_int(0), MTP_long(0)))
            .done([=](const MTPmessages_Messages &result) {
              const auto self = weak.get();
              if (!self || self->_loadEpoch != epoch) { finish(); return; }
              self->_applyingPage = true;
              const QVector<MTPMessage> *list = nullptr;
              auto total = 0;
              result.match(
                  [&](const MTPDmessages_messagesNotModified &) {},
                  [&](const auto &data) {
                    history->owner().processUsers(data.vusers());
                    history->owner().processChats(data.vchats());
                    history->peer->processTopics(data.vtopics());
                    list = &data.vmessages().v;
                  });
              result.match([&](const MTPDmessages_messages &data) { total = data.vmessages().v.size(); },
                  [&](const MTPDmessages_messagesNotModified &) {},
                  [&](const auto &data) { total = data.vcount().v; });
              result.match([&](const MTPDmessages_channelMessages &data) {
                if (const auto channel = history->peer->asChannel()) channel->ptsReceived(data.vpts().v);
              }, [](const auto &) {});
              // An older page goes above the messages on the screen: measure how much it adds.
              const auto keepPosition = list && older && !initial && self->_initialLoaded;
              const auto heightBefore = keepPosition ? self->listHeight() : 0;
              if (list) {
                if (older || initial) {
                  history->addOlderSlice(*list);
                } else {
                  history->addNewerSlice(*list);
                }
              }
              const auto first = !self->_initialLoaded;
              self->_initialLoaded = true;
              self->sync();
              if (keepPosition) {
                if (const auto added = self->listHeight() - heightBefore; added > 0) {
                  const auto fromReserve = std::min(added, self->_topReserve);
                  // Without enough room above, the rest moves the content and the position with it.
                  self->setTopReserve(self->_topReserve - fromReserve, added - fromReserve);
                }
              }
              if (first) {
                self->setTopReserve(history->loadedAtTop() ? 0 : kTopReserve, 0);
                if (self->_pendingShowAt > 0) {
                  if (!self->scrollToLoaded(base::take(self->_pendingShowAt))) self->scrollToBottom();
                } else {
                  self->scrollToBottom();
                  self->_readTimer.callOnce(500);
                }
              }
              (older ? self->_olderRequest : self->_newerRequest) = 0;
              self->_applyingPage = false;
              gtk_widget_set_visible(self->_loading, FALSE);
              if (g_getenv("CONTRAIL_DEBUG")) g_message("History page: %s offset=%d received=%d rows=%zu top=%d bottom=%d",
                  initial ? "initial" : older ? "older" : "newer", int(offsetId.bare), list ? int(list->size()) : -1,
                  self->_rows.size(), int(history->loadedAtTop()), int(history->loadedAtBottom()));
              if (!list || (first && self->_rows.empty() && total > 0)) {
                if (list && !self->_emptyReloadTried) {
                  // The core may have cached messages in another view without attaching them to
                  // this history. Retry once after unloading those views, as HistoryWidget does.
                  self->_emptyReloadTried = true;
                  history->clear(History::ClearType::Unload);
                  self->_initialLoaded = false;
                  crl::on_main(self, [=] { self->loadInitial(); });
                  finish();
                  return;
                }
                self->_loadFailed = true;
                self->_initialLoaded = !first;
                gtk_widget_set_visible(self->_retry, TRUE);
              }
              self->scheduleViewportUpdate();
              finish();
            })
            .fail([=](const MTP::Error &error) {
              if (const auto self = weak.get(); self && self->_loadEpoch == epoch) {
                (older ? self->_olderRequest : self->_newerRequest) = 0;
                self->_loadFailed = true;
                gtk_widget_set_visible(self->_loading, FALSE);
                gtk_widget_set_visible(self->_retry, TRUE);
                ShowToast(error.type());
                if (g_getenv("CONTRAIL_DEBUG")) g_warning("History request failed: %s", U8(error.type()).c_str());
              }
              finish();
            })
            .send();
      });
}

// A topic's messages are a slice of the topic's list; it asks the server for more by itself when
// the slice is wider than what it has (RepliesList::buildFromData), and sends a new one.
void ChatView::viewTopic() {
  _topicViewer.destroy();
  gtk_widget_set_visible(_loading, TRUE);
  gtk_widget_set_visible(_retry, FALSE);
  _topic->replies()->source(_topicAround, _topicLimitBefore, _topicLimitAfter)
      | rpl::on_next([=](Data::MessagesSlice &&slice) { applyTopicSlice(std::move(slice)); }, _topicViewer);
}

void ChatView::applyTopicSlice(Data::MessagesSlice &&slice) {
  const auto first = !_initialLoaded;
  // Older messages go above the ones on the screen: measure how much they add, as for a page.
  const auto olderAdded = !slice.ids.empty() && (_topicIds.empty() || slice.ids.front() < _topicIds.front());
  const auto newerAdded = !slice.ids.empty() && (_topicIds.empty() || _topicIds.back() < slice.ids.back());
  const auto keepPosition = !first && olderAdded && !_topicIds.empty();
  const auto heightBefore = keepPosition ? listHeight() : 0;
  _topicIds = std::move(slice.ids);
  _topicSkippedBefore = slice.skippedBefore;
  _topicSkippedAfter = slice.skippedAfter;
  if (olderAdded || loadedAtTop()) _topicOlderWaiting = false;
  if (newerAdded || loadedAtBottom()) _topicNewerWaiting = false;
  _initialLoaded = true;
  _applyingPage = true;
  sync();
  if (keepPosition) {
    if (const auto added = listHeight() - heightBefore; added > 0) {
      const auto fromReserve = std::min(added, _topReserve);
      setTopReserve(_topReserve - fromReserve, added - fromReserve);
    }
  }
  if (first) {
    setTopReserve(loadedAtTop() ? 0 : kTopReserve, 0);
    if (_pendingShowAt > 0) {
      if (!scrollToLoaded(base::take(_pendingShowAt))) scrollToBottom();
    } else {
      scrollToBottom();
      _readTimer.callOnce(500);
    }
  }
  _applyingPage = false;
  gtk_widget_set_visible(_loading, _topicOlderWaiting || _topicNewerWaiting);
  if (g_getenv("CONTRAIL_DEBUG")) g_message("Topic slice: %d messages, skipped %d/%d", int(_topicIds.size()),
      _topicSkippedBefore.value_or(-1), _topicSkippedAfter.value_or(-1));
  scheduleViewportUpdate();
}

bool ChatView::loadedAtTop() const {
  return _topic ? (_topicSkippedBefore == 0) : _history->loadedAtTop();
}

bool ChatView::loadedAtBottom() const {
  return _topic ? (_topicSkippedAfter == 0) : _history->loadedAtBottom();
}

// The messages of the chat, oldest first.
std::vector<not_null<HistoryItem *>> ChatView::items() const {
  auto result = std::vector<not_null<HistoryItem *>>();
  if (_topic) {
    result.reserve(_topicIds.size());
    for (const auto &id : _topicIds) {
      if (const auto item = _history->owner().message(id)) {
        result.push_back(item);
      }
    }
  } else {
    for (const auto &block : _history->blocks) {
      for (const auto &view : block->messages) {
        result.push_back(view->data());
      }
    }
  }
  return result;
}

void ChatView::sync() {
  std::set<const HistoryItem *> present;
  auto lastRegular = MsgId();
  _sponsoredAfter.clear();
  for (const auto &item : items()) {
    if (item->isSponsored()) {
      _sponsoredAfter.emplace(item, lastRegular);
    } else if (item->isRegular()) {
      lastRegular = item->id;
    }
    present.insert(item);
    if (!_rows.contains(item)) {
      addRow(item);
    }
  }
  for (auto i = _rows.begin(); i != _rows.end();) {
    if (!present.contains(i->first)) {
      _sponsoredViewed.erase(i->first);
      _waitingMedia.erase(i->second.get());
      i = _rows.erase(i);
    } else {
      ++i;
    }
  }
  // Also recomputes the headers.
  gtk_list_box_invalidate_sort(GTK_LIST_BOX(_list));
  scheduleViewportUpdate();
  if (_stickToBottom && _window->isActive()) {
    _readTimer.callOnce(300);
  }
}

void ChatView::addRow(not_null<HistoryItem *> item) {
  auto row = std::make_unique<MessageWidget>(item, this);
  gtk_list_box_append(GTK_LIST_BOX(_list), row->row());
  if (!row->refreshMedia()) {
    _waitingMedia.insert(row.get());
  }
  _rows.emplace(item, std::move(row));
}

void ChatView::removeRow(const HistoryItem *item) {
  _sponsoredAfter.erase(item);
  _sponsoredViewed.erase(item);
  if (const auto i = _rows.find(item); i != _rows.end()) {
    _waitingMedia.erase(i->second.get());
    _rows.erase(i);
    gtk_list_box_invalidate_headers(GTK_LIST_BOX(_list));
  }
}

MessageWidget *ChatView::rowFor(const HistoryItem *item) const {
  const auto i = _rows.find(item);
  return (i != _rows.end()) ? i->second.get() : nullptr;
}

void ChatView::refreshItem(const HistoryItem *item) {
  if (auto *row = rowFor(item)) {
    row->refresh();
    if (!row->refreshMedia()) {
      _waitingMedia.insert(row);
    }
    gtk_list_box_row_changed(GTK_LIST_BOX_ROW(row->row()));
  }
}

void ChatView::scrollToBottom() {
  _stickToBottom = true;
  _scrollTarget = FullMsgId();
  scheduleViewportUpdate();
}

void ChatView::scrollToRow(MessageWidget *row, bool highlight) {
  _stickToBottom = false;
  _scrollTarget = row->item()->fullId();
  _scrollTargetHighlight = highlight;
  scheduleViewportUpdate();
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

bool ChatView::scrollIdle() const {
  return scrollIdleIgnoringShift() && _pendingShift == 0.;
}

bool ChatView::scrollIdleIgnoringShift() const {
  return !_touchActive && !ListMoving();
}

void ChatView::maintainTopReserve() {
  if (!_initialLoaded || _applyingPage || _olderRequest || _topicOlderWaiting) return;
  const auto wanted = loadedAtTop() ? 0 : (_topReserve < kTopReserveLow) ? kTopReserve : _topReserve;
  if (wanted == _topReserve) return;
  if (!scrollIdle()) {
    _reserveTimer.callOnce(300);
    return;
  }
  setTopReserve(wanted, wanted - _topReserve);
}

void ChatView::scheduleViewportUpdate() {
  if (_viewportSource) return;
  auto *weak = new base::weak_ptr<ChatView>(this);
  // Wait for GTK's next frame to allocate new rows before restoring scroll coordinates.
  _viewportSource = g_timeout_add_full(G_PRIORITY_DEFAULT, 20, +[](gpointer data) -> gboolean {
    auto *weak = static_cast<base::weak_ptr<ChatView> *>(data);
    if (const auto self = weak->get()) {
      self->_viewportSource = 0;
      RunCoreEvent([=] { self->updateViewport(); });
    }
    return G_SOURCE_REMOVE;
  }, weak, +[](gpointer data) { delete static_cast<base::weak_ptr<ChatView> *>(data); });
}

void ChatView::updateViewport() {
  if (_applyingPage || !_initialLoaded) return;
  if (!gtk_widget_get_mapped(_root)) return;
  if (gtk_adjustment_get_page_size(_adjustment) <= 0
      || std::any_of(_rows.begin(), _rows.end(), [](const auto &pair) {
           return gtk_widget_get_height(pair.second->row()) <= 0;
         })) {
    scheduleViewportUpdate();
    return;
  }
  _adjustingScroll = true;
  if (_scrollTarget) {
    const auto item = _history->owner().message(_scrollTarget);
    if (const auto row = item ? rowFor(item) : nullptr) {
      if (ScrollWidgetToY(row->row(), _scroll, _adjustment, gtk_adjustment_get_page_size(_adjustment) / 3)) {
        if (_scrollTargetHighlight) row->highlight();
        _scrollTarget = FullMsgId();
      }
    } else {
      _scrollTarget = FullMsgId();
    }
  } else if (_stickToBottom) {
    gtk_adjustment_set_value(_adjustment, gtk_adjustment_get_upper(_adjustment) - gtk_adjustment_get_page_size(_adjustment));
  }
  _adjustingScroll = false;
  maintainTopReserve();
  const auto upper = gtk_adjustment_get_upper(_adjustment);
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  const auto value = gtk_adjustment_get_value(_adjustment);
  const auto bottom = value + page >= upper - 40 && loadedAtBottom();
  gtk_widget_set_visible(_toBottom, !bottom && upper > page);
  preloadVisibleEdges();
  if (bottom) {
    showSponsored();
  }
  reportSponsoredViews();
  if (_window->isActive() && !_readTimer.isActive()) _readTimer.callOnce(300);
}

void ChatView::showSponsored() {
  auto &sponsored = _history->session().sponsoredMessages();
  if (_topic || !_initialLoaded || !_history->loadedAtBottom() || !sponsored.canHaveFor(_history)
      || sponsored.isTopBarFor(_history)) {
    return;
  }
  using State = Data::SponsoredMessages::State;
  using Result = Data::SponsoredMessages::AppendResult;
  // The core asks the server at most once in a few minutes and keeps the answer.
  sponsored.request(_history, crl::guard(this, [=] { showSponsored(); }));
  _sponsoredMediaLoading = false;
  auto added = false;
  switch (sponsored.state(_history)) {
    case State::AppendToEnd: {
      const auto result = sponsored.append(_history);
      _sponsoredMediaLoading = (result == Result::MediaLoading);
      added = (result == Result::Appended);
    } break;
    case State::InjectToMiddle: {
      // Contrail shows it after the newest message, where the reader ends up, like AppendToEnd.
      auto last = MsgId();
      for (const auto &block : _history->blocks) {
        for (const auto &view : block->messages) {
          if (view->data()->isRegular()) last = view->data()->id;
        }
      }
      if (last) {
        sponsored.inject(_history, last, 0, 400);
        added = true;
      }
    } break;
    default: break;
  }
  if (added) {
    sync();
  }
}

void ChatView::reportSponsoredViews() {
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  for (const auto &[item, after] : _sponsoredAfter) {
    const auto row = rowFor(item);
    if (!row || _sponsoredViewed.contains(item)) continue;
    double y = 0;
    const auto height = gtk_widget_get_height(row->row());
    // Seen once the whole message is on the screen (or fills it, if it is taller).
    if (height > 0 && WidgetViewportY(row->row(), _scroll, y)
        && ((y >= 0 && y + height <= page) || (y <= 0 && y + height >= page))) {
      _sponsoredViewed.insert(item);
      _history->session().sponsoredMessages().view(item->fullId());
    }
  }
}

void ChatView::preloadVisibleEdges() {
  if (_applyingPage || _olderRequest || _newerRequest || !_initialLoaded || _loadFailed || _scrollTarget) return;
  const auto value = gtk_adjustment_get_value(_adjustment);
  const auto upper = gtk_adjustment_get_upper(_adjustment);
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  if (value - _topReserve < kPreloadHeight && !loadedAtTop()) loadOlder();
  else if (value + page > upper - kPreloadHeight && !loadedAtBottom()) loadNewer();
}

void ChatView::showAt(MsgId id) {
  if (scrollToLoaded(id)) {
    return;
  }
  reload(id);
}

void ChatView::reload(MsgId showAt) {
  ++_loadEpoch;
  _initialLoaded = false;
  _loadFailed = false;
  _emptyReloadTried = false;
  _scrollTarget = FullMsgId();
  _pendingShift = 0;
  setTopReserve(0, 0);
  _history->owner().histories().cancelRequest(base::take(_olderRequest));
  _history->owner().histories().cancelRequest(base::take(_newerRequest));
  _topicViewer.destroy();
  _topicIds.clear();
  _topicSkippedBefore = _topicSkippedAfter = std::nullopt;
  _topicOlderWaiting = _topicNewerWaiting = false;
  _waitingMedia.clear();
  _rows.clear();
  _pendingShowAt = showAt;
  _stickToBottom = !showAt;
  loadInitial();
}

bool ChatView::scrollToLoaded(MsgId id) {
  if (const auto item = _history->owner().message(_history->peer, id)) {
    if (auto *row = rowFor(item)) {
      scrollToRow(row, id != _unreadAfter + 1);
      return true;
    }
  }
  // The first unread message: ids have gaps, take the first loaded one after the read ones.
  if (_unreadAfter && id == _unreadAfter + 1) {
    MessageWidget *first = nullptr;
    for (const auto &[item, row] : _rows) {
      if (item->id > _unreadAfter && item->isRegular() && (!first || item->id < first->item()->id)) {
        first = row.get();
      }
    }
    if (first) {
      scrollToRow(first, false);
      return true;
    }
  }
  return false;
}

void ChatView::debugScroll(bool top) {
  if (top) {
    // The first loaded message, as a user scrolling up reaches it.
    _scrollTarget = FullMsgId();
    _stickToBottom = false;
    NoteListMotion();
    gtk_adjustment_set_value(_adjustment, _topReserve);
    scheduleViewportUpdate();
  } else {
    scrollToBottom();
  }
}

void ChatView::debugCheckDraft() {
  if (_editing || _recording) return;
  for (const auto &[item, row] : _rows) {
    if (!item->allowsEdit(base::unixtime::now())) continue;
    const auto original = composerText();
    const auto reply = _replyTo;
    const auto sample = u"Contrail draft regression"_q;
    setComposerText(sample);
    edit(row->item());
    cancelReplyOrEdit();
    const auto restored = composerText() == sample && _replyTo == reply;
    setComposerText(original);
    g_message("Draft regression: %s", restored ? "passed" : "FAILED");
    return;
  }
  g_message("Draft regression: skipped (no editable message loaded)");
}

void ChatView::scrolled() {
  if (_adjustingScroll || _applyingPage || !_initialLoaded) return;
  const auto value = gtk_adjustment_get_value(_adjustment);
  const auto upper = gtk_adjustment_get_upper(_adjustment);
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  const auto atBottom = value + page >= upper - 40 && loadedAtBottom();
  // GTK also changes the adjustment while allocating newly inserted rows. Those changes do not
  // represent a user drag and must not drop the bottom/anchor/target requested before layout.
  // Only input events opt out of bottom following. Layout/clamping changes also emit this signal
  // and must never turn a newly opened channel's bottom-following flag off.
  if (!_viewportSource && !_scrollTarget && atBottom) _stickToBottom = true;
  gtk_widget_set_visible(_toBottom, !atBottom && upper > page);
  if (!_viewportSource) scheduleViewportUpdate();
  if (_window->isActive() && !_readTimer.isActive()) _readTimer.callOnce(300);
}

void ChatView::debugHistory(const QString &action) {
  if (action == u"older"_q) loadOlder();
  else if (action == u"newer"_q) loadNewer();
  else if (action == u"end"_q) reload(0);
  else if (action.startsWith(u"find:"_q)) {
    // "history-state find:TEXT": the loaded message with this text at the top third of the screen.
    for (const auto &[item, row] : _rows) {
      if (item->originalText().text.contains(action.mid(5))) {
        scrollToRow(row.get(), false);
        g_message("Found message %d: %d characters, %d entities", int(item->id.bare),
                  int(item->originalText().text.size()), int(item->originalText().entities.size()));
        break;
      }
    }
  }
  g_message("History state: rows=%zu min=%d max=%d top=%d bottom=%d initial=%d requests=%d/%d failed=%d scroll=%.0f/%.0f page=%.0f",
      _rows.size(), int(_history->minMsgId().bare), int(_history->maxMsgId().bare), int(loadedAtTop()),
      int(loadedAtBottom()), int(_initialLoaded), _olderRequest, _newerRequest, int(_loadFailed),
      gtk_adjustment_get_value(_adjustment), gtk_adjustment_get_upper(_adjustment), gtk_adjustment_get_page_size(_adjustment));
  const auto zero = std::count_if(_rows.begin(), _rows.end(), [](const auto &pair) {
    return gtk_widget_get_height(pair.second->row()) <= 0;
  });
  g_message("History layout: zero-rows=%d target=%d reserve=%d source=%u mapped=%d sticky=%d list-height=%d kind=%s composer=%s",
      int(zero), int(_scrollTarget.msg.bare), _topReserve, _viewportSource,
      int(gtk_widget_get_mapped(_root)), int(_stickToBottom), gtk_widget_get_height(_list),
      _topic ? "topic" : _history->peer->isUser() ? "user" : _history->peer->isBroadcast() ? "channel" : "group",
      gtk_stack_get_visible_child_name(GTK_STACK(_composerStack)));
}

void ChatView::markAsRead() {
  if (!_window->isActive() || _window->currentThread() != _thread || !_initialLoaded || _scrollTarget
      || !gtk_widget_get_mapped(_root)) {
    return;
  }
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  if (page <= 0) return;
  const auto atBottom = gtk_adjustment_get_value(_adjustment) + page >= gtk_adjustment_get_upper(_adjustment) - 40
      && loadedAtBottom();
  if (_stickToBottom || atBottom) {
    if (_topic) {
      if (_topic->chatListBadgesState().unread) {
        _topic->readTillEnd();
      }
      Core::App().notifications().clearFromTopic(_topic);
      return;
    }
    if (_history->unreadCount() > 0 || _history->unreadMark() || _history->chatListBadgesState().unread) {
      _history->owner().histories().readInbox(_history);
    }
    Core::App().notifications().clearFromHistory(_history);
    return;
  }
  // Higher up, read till the lowest unread message seen whole, as HistoryInner does. Opening a chat
  // puts its first unread message on the screen, and the list may have nowhere further to scroll.
  HistoryItem *till = nullptr;
  for (const auto &[item, row] : _rows) {
    if (item->out() || !item->isRegular() || !item->unread(_thread) || (till && item->id < till->id)) continue;
    double y = 0;
    const auto height = gtk_widget_get_height(row->row());
    if (height > 0 && WidgetViewportY(row->row(), _scroll, y) && y + height >= 0 && y + height <= page) {
      till = row->item();
    }
  }
  if (!till) {
  } else if (_topic) {
    _topic->replies()->readTill(till);
  } else {
    _history->owner().histories().readInboxTill(till);
  }
}

// --- composer ----------------------------------------------------------------

void ChatView::buildComposer() {
  _composer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

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
  auto *cancel = MakeIconButton("window-close-symbolic", U8(tr::lng_cancel(tr::now)).c_str(), "flat");
  gtk_widget_add_css_class(cancel, "circular");
  _signals.clicked(cancel, [this] { cancelReplyOrEdit(); });
  gtk_box_append(GTK_BOX(_replyBar), cancel);
  gtk_widget_set_visible(_replyBar, FALSE);
  gtk_box_append(GTK_BOX(_composer), _replyBar);

  // The message field.
  auto *field = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(field, "composer");
  auto *attach = MakeIconButton("mail-attachment-symbolic", U8(tr::lng_attach(tr::now)).c_str(), "flat");
  gtk_widget_add_css_class(attach, "circular");
  gtk_widget_set_valign(attach, GTK_ALIGN_END);
  _signals.clicked(attach, [this] { chooseFiles(); });
  gtk_box_append(GTK_BOX(field), attach);

  _input = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(_input), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(_input), FALSE);
  gtk_text_view_set_input_hints(GTK_TEXT_VIEW(_input), GtkInputHints(GTK_INPUT_HINT_SPELLCHECK | GTK_INPUT_HINT_WORD_COMPLETION | GTK_INPUT_HINT_EMOJI | GTK_INPUT_HINT_UPPERCASE_SENTENCES));
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
  const auto placeholderText = _history->peer->isBroadcast() ? tr::lng_broadcast_ph(tr::now) : tr::lng_message_ph(tr::now);
  auto *placeholder = gtk_label_new(U8(placeholderText).c_str());
  // GtkTextView gives overlay children their minimum size, which an ellipsized label has none of.
  gtk_label_set_width_chars(GTK_LABEL(placeholder), std::min(int(placeholderText.size()), 24));
  gtk_widget_add_css_class(placeholder, "dim-label");
  gtk_widget_set_can_target(placeholder, FALSE);
  gtk_label_set_xalign(GTK_LABEL(placeholder), 0);
  gtk_label_set_ellipsize(GTK_LABEL(placeholder), PANGO_ELLIPSIZE_END);
  gtk_text_view_add_overlay(GTK_TEXT_VIEW(_input), placeholder, 0, 0);
  auto *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(_input));
  _signals.connect<void(GtkTextBuffer *)>(buffer, "changed", [this, placeholder](GtkTextBuffer *buffer) {
    const auto empty = gtk_text_buffer_get_char_count(buffer) == 0;
    gtk_widget_set_visible(placeholder, empty);
    gtk_widget_set_sensitive(_sendButton, !empty && !_editPending);
    gtk_stack_set_visible_child_name(GTK_STACK(_sendStack), (empty && !_editing) ? "mic" : "send");
    if (!empty && !_settingComposerText) {
      _history->session().sendProgressManager().update(_history, _thread->topicRootId(), Api::SendProgressType::Typing);
    }
  });

  _sendButton = MakeIconButton("contrail-send-symbolic", U8(tr::lng_send_button(tr::now)).c_str(), "suggested-action");
  gtk_widget_add_css_class(_sendButton, "circular");
  gtk_widget_set_valign(_sendButton, GTK_ALIGN_END);
  _signals.clicked(_sendButton, [this] { send(); });
  // With an empty field the button records a voice message while it is held (setupRecordGesture).
  _mic = MakeIconButton("audio-input-microphone-symbolic", U8(tr::lng_shortcuts_record_voice_message(tr::now)).c_str(), "flat");
  gtk_widget_add_css_class(_mic, "circular");
  // Only the keyboard gets here (the gesture takes presses): record without holding.
  _signals.clicked(_mic, [this] {
    startRecording();
    lockRecording();
  });
  _sendStack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(_sendStack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_add_named(GTK_STACK(_sendStack), _mic, "mic");
  gtk_stack_add_named(GTK_STACK(_sendStack), _sendButton, "send");
  gtk_widget_set_valign(_sendStack, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(field), _sendStack);

  // Enter sends, Shift+Enter starts a new line.
  auto *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  _signals.connect<gboolean(GtkEventControllerKey *, guint, guint, GdkModifierType)>(
      keys, "key-pressed", [this](GtkEventControllerKey *, guint key, guint, GdkModifierType state) -> gboolean {
        if ((key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) && !(state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK))) {
          send();
          return TRUE;
        } else if (key == GDK_KEY_Up && composerText().isEmpty() && !_editing) {
          // Up in an empty field edits the last sent message.
          const auto list = items();
          for (auto i = list.rbegin(); i != list.rend(); ++i) {
            if ((*i)->out() && (*i)->allowsEdit(base::unixtime::now())) {
              edit(*i);
              return TRUE;
            }
          }
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
    const auto channel = _history->peer->asChannel();
    if (channel && !channel->amIn()) {
      _history->session().api().joinChannel(channel);
    } else {
      auto value = Data::MuteValue();
      if (_thread->muted()) {
        value.unmute = true;
      } else {
        value.forever = true;
      }
      _history->owner().notifySettings().update(_thread, value);
    }
  });
  gtk_stack_add_named(GTK_STACK(_composerStack), _joinButton, "join");

  // Recording a voice message: while holding, "slide to cancel" and the microphone under the
  // finger; once locked, the delete and send buttons.
  auto *record = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(record, "composer");
  gtk_widget_add_css_class(record, "record-bar");
  _recordCancel = MakeIconButton("user-trash-symbolic", U8(tr::lng_record_lock_delete(tr::now)).c_str(), "flat");
  gtk_widget_add_css_class(_recordCancel, "circular");
  gtk_widget_set_valign(_recordCancel, GTK_ALIGN_CENTER);
  _signals.clicked(_recordCancel, [this] { finishRecording(false); });
  gtk_box_append(GTK_BOX(record), _recordCancel);
  _recordDot = gtk_label_new("\u25CF");
  gtk_widget_add_css_class(_recordDot, "recording-dot");
  gtk_widget_set_margin_start(_recordDot, 6);
  gtk_box_append(GTK_BOX(record), _recordDot);
  _recordTime = gtk_label_new("0:00");
  gtk_widget_add_css_class(_recordTime, "numeric");
  gtk_label_set_xalign(GTK_LABEL(_recordTime), 0);
  gtk_label_set_width_chars(GTK_LABEL(_recordTime), 6);
  gtk_box_append(GTK_BOX(record), _recordTime);
  _recordHint = gtk_label_new(U8(u"\u2039  "_q + tr::lng_record_cancel_recording(tr::now)).c_str());
  gtk_widget_add_css_class(_recordHint, "dim-label");
  gtk_widget_set_hexpand(_recordHint, TRUE);
  gtk_label_set_xalign(GTK_LABEL(_recordHint), 1);
  gtk_widget_set_margin_end(_recordHint, kRecordHintMargin);
  gtk_label_set_ellipsize(GTK_LABEL(_recordHint), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(record), _recordHint);
  _recordSend = MakeIconButton("contrail-send-symbolic", U8(tr::lng_send_button(tr::now)).c_str(), "suggested-action");
  gtk_widget_add_css_class(_recordSend, "circular");
  gtk_widget_set_valign(_recordSend, GTK_ALIGN_CENTER);
  _signals.clicked(_recordSend, [this] { finishRecording(true); });
  gtk_box_append(GTK_BOX(record), _recordSend);
  _recordMicIndicator = gtk_image_new_from_icon_name("audio-input-microphone-symbolic");
  gtk_widget_add_css_class(_recordMicIndicator, "record-mic");
  gtk_widget_set_valign(_recordMicIndicator, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(record), _recordMicIndicator);
  gtk_stack_add_named(GTK_STACK(_composerStack), record, "record");
  gtk_box_append(GTK_BOX(_composer), _composerStack);
  adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(_root), _composer);
  setupRecordGesture();

  // Restore the draft (the core turns one typed on another device into the local one).
  if (const auto draft = _history->localDraft(_thread->topicRootId(), _thread->monoforumPeerId())) {
    _loadedDraft = draft->textWithTags.text;
    _loadedReplyTo = draft->reply.messageId;
    setComposerText(draft->textWithTags.text);
    if (const auto replyId = draft->reply.messageId) {
      if (const auto item = _history->owner().message(replyId)) {
        replyTo(item);
      }
    }
  }
}

void ChatView::refreshComposerState() {
  const auto peer = _history->peer;
  const auto channel = peer->asChannel();
  if (channel && !channel->amIn()) {
    gtk_button_set_label(GTK_BUTTON(_joinButton), U8(channel->isBroadcast() ? tr::lng_profile_join_channel(tr::now)
                                                                            : tr::lng_profile_join_group(tr::now))
                                                      .c_str());
    gtk_stack_set_visible_child_name(GTK_STACK(_composerStack), "join");
  } else if (!Data::CanSendTexts(_thread)) {
    gtk_button_set_label(GTK_BUTTON(_joinButton),
                         U8(_thread->muted() ? tr::lng_channel_unmute(tr::now) : tr::lng_channel_mute(tr::now)).c_str());
    gtk_stack_set_visible_child_name(GTK_STACK(_composerStack), "join");
  } else {
    gtk_stack_set_visible_child_name(GTK_STACK(_composerStack), "field");
  }
}

QString ChatView::composerText() const {
  auto *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(_input));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  auto *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  auto result = Q(text);
  g_free(text);
  return result;
}

void ChatView::setComposerText(const QString &text) {
  auto *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(_input));
  const auto utf8 = text.toUtf8();
  _settingComposerText = true;
  gtk_text_buffer_set_text(buffer, utf8.constData(), utf8.size());
  _settingComposerText = false;
}

void ChatView::showReplyBar(const QString &title, const QString &text, const char *icon) {
  gtk_image_set_from_icon_name(GTK_IMAGE(_replyIcon), icon);
  gtk_label_set_text(GTK_LABEL(_replyTitle), U8(title).c_str());
  gtk_label_set_text(GTK_LABEL(_replyText), U8(QString(text).replace('\n', ' ')).c_str());
  gtk_widget_set_visible(_replyBar, TRUE);
  gtk_widget_grab_focus(_input);
}

void ChatView::replyTo(not_null<HistoryItem *> item) {
  if (item->isSponsored()) return;
  if (_editing) {
    cancelReplyOrEdit();
  }
  _replyTo = item->fullId();
  const auto from = item->displayFrom() ? item->displayFrom() : item->from().get();
  showReplyBar(from->name(), item->toPreview({.hideSender = true, .generateImages = false}).text.text,
               "mail-reply-sender-symbolic");
}

void ChatView::edit(not_null<HistoryItem *> item) {
  if (_editPending) return;
  if (!_editing) {
    _beforeEditText = composerText();
    _beforeEditReplyTo = _replyTo;
  }
  _replyTo = FullMsgId();
  _editing = item->fullId();
  ++_editGeneration;
  const auto &original = item->originalText();
  setComposerText(original.text);
  showReplyBar(tr::lng_edit_message(tr::now), original.text, "document-edit-symbolic");
  gtk_widget_set_sensitive(_sendButton, TRUE);
}

void ChatView::cancelReplyOrEdit() {
  const auto wasEditing = bool(_editing);
  _replyTo = FullMsgId();
  _editing = FullMsgId();
  _editPending = false;
  ++_editGeneration;
  gtk_text_view_set_editable(GTK_TEXT_VIEW(_input), TRUE);
  gtk_widget_set_visible(_replyBar, FALSE);
  if (wasEditing) {
    setComposerText(base::take(_beforeEditText));
    if (const auto reply = base::take(_beforeEditReplyTo)) {
      if (const auto item = _history->owner().message(reply)) replyTo(item);
    }
  }
}

bool ChatView::handleEscape() {
  if (_recording) {
    finishRecording(false);
    return true;
  } else if (_replyTo || _editing) {
    cancelReplyOrEdit();
    return true;
  }
  return false;
}

void ChatView::send() {
  if (_editPending || _recording || !Data::CanSendTexts(_thread)) return;
  const auto text = composerText().trimmed();
  const auto session = &_history->session();
  if (_editing) {
    const auto item = session->data().message(_editing);
    if (item && !text.isEmpty()) {
      auto sending = TextWithEntities{text};
      TextUtilities::PrepareForSending(sending, TextParseLinks | TextParseMentions | TextParseHashtags | TextParseBotCommands);
      const auto weak = base::make_weak(this);
      const auto editing = _editing;
      const auto generation = _editGeneration;
      _editPending = true;
      gtk_widget_set_sensitive(_sendButton, FALSE);
      gtk_text_view_set_editable(GTK_TEXT_VIEW(_input), FALSE);
      Api::EditTextMessage(
          item, sending, Data::WebPageDraft(), Api::SendOptions(),
          [=](mtpRequestId) {
            if (const auto strong = weak.get(); strong && strong->_editing == editing
                && strong->_editGeneration == generation) {
              strong->cancelReplyOrEdit();
            }
          },
          [=](const QString &error, mtpRequestId) {
            if (const auto strong = weak.get(); strong && strong->_editing == editing
                && strong->_editGeneration == generation) {
              strong->_editPending = false;
              gtk_text_view_set_editable(GTK_TEXT_VIEW(strong->_input), TRUE);
              if (error == u"MESSAGE_NOT_MODIFIED"_q) {
                strong->cancelReplyOrEdit();
              } else {
                gtk_widget_set_sensitive(strong->_sendButton, TRUE);
                ShowToast(error);
              }
            }
          },
          false);
    }
    return;
  }
  if (text.isEmpty()) {
    return;
  }
  auto action = Api::SendAction(_thread);
  if (_replyTo) {
    action.replyTo.messageId = _replyTo;
  }
  auto message = Api::MessageToSend(action);
  message.textWithTags = TextWithTags{text, {}};
  session->api().sendMessage(std::move(message));
  setComposerText(QString());
  cancelReplyOrEdit();
  _history->clearLocalDraft(_thread->topicRootId(), _thread->monoforumPeerId());
  _history->clearCloudDraft(_thread->topicRootId(), _thread->monoforumPeerId());
  _loadedDraft.clear();
  _loadedReplyTo = FullMsgId();
  scrollToBottom();
}

void ChatView::sendText(const QString &text) {
  if (!Data::CanSendTexts(_thread) || text.trimmed().isEmpty()) return;
  auto message = Api::MessageToSend(Api::SendAction(_thread));
  message.textWithTags = TextWithTags{text, {}};
  _history->session().api().sendMessage(std::move(message));
  scrollToBottom();
}

void ChatView::chooseFiles() {
  auto *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, U8(tr::lng_attach(tr::now)).c_str());
  auto *weak = new base::weak_ptr<ChatView>(this);
  gtk_file_dialog_open_multiple(
      dialog, GTK_WINDOW(_window->widget()), nullptr,
      +[](GObject *source, GAsyncResult *result, gpointer data) {
        auto *weak = static_cast<base::weak_ptr<ChatView> *>(data);
        auto *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source), result, nullptr);
        if (files) {
          QStringList paths;
          const auto count = g_list_model_get_n_items(files);
          for (guint i = 0; i != count; ++i) {
            auto *file = G_FILE(g_list_model_get_item(files, i));
            if (auto *path = g_file_get_path(file)) {
              paths.push_back(Q(path));
              g_free(path);
            }
            g_object_unref(file);
          }
          g_object_unref(files);
          if (const auto strong = weak->get(); strong && !paths.isEmpty()) {
            strong->sendFiles(paths);
          }
        }
        delete weak;
      },
      weak);
  g_object_unref(dialog);
}

void ChatView::sendFiles(const QStringList &paths) {
  if (paths.isEmpty() || !Data::CanSendTexts(_thread)) return;
  const auto session = &_history->session();
  auto list = Storage::PrepareMediaList(paths, 400, session->premium());
  if (list.error != Ui::PreparedList::Error::None) {
    ShowToast(list.errorData.isEmpty() ? tr::lng_send_image_empty(tr::now, lt_name, paths.front()) : list.errorData);
    return;
  }
  // Pictures go as photos, everything else as files, each one separately.
  auto action = Api::SendAction(_thread);
  if (_replyTo) {
    action.replyTo.messageId = _replyTo;
  }
  for (auto &file : list.files) {
    auto single = Ui::PreparedList();
    const auto isPhoto = (file.type == Ui::PreparedFile::Type::Photo);
    single.files.push_back(std::move(file));
    session->api().sendFiles(std::move(single), isPhoto ? SendMediaType::Photo : SendMediaType::File, nullptr, action);
  }
  cancelReplyOrEdit();
  scrollToBottom();
}

// The gesture is on the whole composer, which stays on the screen when the microphone button is
// replaced with the recording bar under the finger.
void ChatView::setupRecordGesture() {
  auto *drag = gtk_gesture_drag_new();
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(drag), GTK_PHASE_CAPTURE);
  _signals.connect<void(GtkGestureDrag *, double, double)>(drag, "drag-begin", [this](GtkGestureDrag *gesture, double x, double y) {
    graphene_rect_t bounds;
    const auto point = GRAPHENE_POINT_INIT(float(x), float(y));
    const auto onMic = !_recording && gtk_widget_get_mapped(_mic) && gtk_widget_get_sensitive(_mic)
                       && gtk_widget_compute_bounds(_mic, _composer, &bounds)
                       && graphene_rect_contains_point(&bounds, &point);
    if (!onMic) {
      gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
      return;
    }
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    startRecording();
    _recordHolding = _recording;
    showRecordState();
  });
  _signals.connect<void(GtkGestureDrag *, double, double)>(drag, "drag-update", [this](GtkGestureDrag *, double dx, double dy) {
    if (_recordHolding) dragRecording(dx, dy);
  });
  _signals.connect<void(GtkGestureDrag *, double, double)>(drag, "drag-end", [this](GtkGestureDrag *, double, double) {
    if (!std::exchange(_recordHolding, false)) return;
    if (crl::now() - _recordStarted < kRecordMinHold) {
      // A tap: Telegram explains that the button is to be held.
      finishRecording(false);
      ShowToast(tr::lng_record_hold_tip(tr::now));
    } else {
      finishRecording(true);
    }
  });
  _signals.connect<void(GtkGesture *, GdkEventSequence *)>(drag, "cancel", [this](GtkGesture *, GdkEventSequence *) {
    if (std::exchange(_recordHolding, false)) finishRecording(false);
  });
  gtk_widget_add_controller(_composer, GTK_EVENT_CONTROLLER(drag));
}

void ChatView::startRecording() {
  if (_editing || !Data::CanSendTexts(_thread)) return;
  const auto capture = ::Media::Capture::instance();
  if (_recording || !capture->available()) {
    if (!capture->available()) {
      ShowToast(tr::lng_record_audio_problem(tr::now));
    }
    return;
  }
  _recording = true;
  _recordLocked = false;
  _recordStarted = crl::now();
  Media::Player::instance()->pause(AudioMsgId::Type::Voice);
  gtk_label_set_text(GTK_LABEL(_recordTime), U8(Ui::FormatDurationText(0) + QLocale().decimalPoint() + '0').c_str());
  gtk_stack_set_visible_child_name(GTK_STACK(_composerStack), "record");
  showRecordState();
  TriggerFeedback("button-pressed");
  // start() ends the previous stream of updates: subscribe after it.
  capture->start();
  capture->updated() | rpl::on_next_error([=](const ::Media::Capture::Update &update) {
    const auto ms = update.samples * crl::time(1000) / ::Media::Player::kDefaultFrequency;
    gtk_label_set_text(GTK_LABEL(_recordTime),
                       U8(Ui::FormatDurationText(ms / 1000) + QLocale().decimalPoint() + QString::number(ms % 1000 / 100)).c_str());
    // The dot breathes with the voice.
    gtk_widget_set_opacity(_recordDot, 0.35 + 0.65 * std::min(1., update.level / 6000.));
  }, [=](::Media::Capture::Error) {
    finishRecording(false);
    ShowToast(tr::lng_record_audio_problem(tr::now));
  }, _recordLifetime);
  _history->session().sendProgressManager().update(_history, _thread->topicRootId(), Api::SendProgressType::RecordVoice);
}

void ChatView::lockRecording() {
  if (!_recording) return;
  _recordHolding = false;
  _recordLocked = true;
  TriggerFeedback("button-pressed");
  showRecordState();
}

void ChatView::showRecordState() {
  const auto holding = _recording && !_recordLocked;
  gtk_widget_set_visible(_recordCancel, _recordLocked);
  gtk_widget_set_visible(_recordSend, _recordLocked);
  gtk_widget_set_visible(_recordMicIndicator, holding);
  gtk_widget_set_opacity(_recordHint, _recordLocked ? 0. : 1.);
  gtk_widget_set_margin_end(_recordHint, kRecordHintMargin);
  gtk_widget_set_visible(_lockHint, holding);
  gtk_widget_set_margin_bottom(_lockHint, 16);
  gtk_widget_set_opacity(_recordDot, 1.);
  if (holding) gtk_widget_set_visible(_toBottom, FALSE);
}

void ChatView::dragRecording(double dx, double dy) {
  if (dx < -kRecordCancelDistance) {
    _recordHolding = false;
    finishRecording(false);
    TriggerFeedback("button-pressed");
    return;
  } else if (dy < -kRecordLockDistance) {
    lockRecording();
    return;
  }
  // "Slide to cancel" follows the finger and fades; the lock rises with it.
  const auto left = std::clamp(-dx, 0., kRecordCancelDistance);
  gtk_widget_set_margin_end(_recordHint, kRecordHintMargin + int(left));
  gtk_widget_set_opacity(_recordHint, 1. - 0.75 * left / kRecordCancelDistance);
  const auto up = std::clamp(-dy, 0., kRecordLockDistance);
  gtk_widget_set_margin_bottom(_lockHint, 16 + int(up / 2));
}

void ChatView::finishRecording(bool send) {
  if (!_recording) {
    return;
  }
  _recording = false;
  _recordHolding = false;
  _recordLocked = false;
  _recordLifetime.destroy();
  showRecordState();
  refreshComposerState();
  scheduleViewportUpdate();
  const auto history = _history;
  auto action = Api::SendAction(_thread);
  if (_replyTo) {
    action.replyTo.messageId = _replyTo;
  }
  if (!send) {
    ::Media::Capture::instance()->stop();
    history->session().sendProgressManager().cancel(history, _thread->topicRootId(), Api::SendProgressType::RecordVoice);
    return;
  }
  cancelReplyOrEdit();
  ::Media::Capture::instance()->stop(crl::guard(this, [=](::Media::Capture::Result &&result) {
    if (result.bytes.isEmpty() || result.duration < 1000) {
      return;
    }
    history->session().api().sendVoiceMessage(result.bytes, result.waveform, result.duration, false, action);
    scrollToBottom();
  }));
}

// --- message actions ---------------------------------------------------------

void ChatView::showMessageMenu(not_null<HistoryItem *> item, GtkWidget *widget, double x, double y) {
  if (item->isSponsored()) return;
  auto *menu = g_menu_new();
  const auto now = base::unixtime::now();
  if (!item->isService() && Data::CanSendTexts(_thread)) {
    g_menu_append(menu, U8(tr::lng_context_reply_msg(tr::now)).c_str(), "msg.reply");
  }
  if (!item->originalText().text.isEmpty()) {
    g_menu_append(menu, U8(tr::lng_context_copy_text(tr::now)).c_str(), "msg.copy");
  }
  if (item->allowsEdit(now)) {
    g_menu_append(menu, U8(tr::lng_context_edit_msg(tr::now)).c_str(), "msg.edit");
  }
  const auto document = item->media() ? item->media()->document() : nullptr;
  const auto photo = item->media() ? item->media()->photo() : nullptr;
  if ((document && !document->sticker()) || photo) {
    g_menu_append(menu, U8(tr::lng_context_save_file(tr::now)).c_str(), "msg.save");
  }
  if (item->canDelete()) {
    g_menu_append(menu, U8(tr::lng_context_delete_msg(tr::now)).c_str(), "msg.delete");
  }

  auto *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  g_object_unref(menu);
  gtk_widget_set_parent(popover, widget);
  GdkRectangle rect = {int(x), int(y), 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  DestroyPopoverOnClose(popover);

  auto *group = g_simple_action_group_new();
  const auto add = [&](const char *name, std::function<void()> fn) {
    auto *action = g_simple_action_new(name, nullptr);
    Connect<void(GSimpleAction *, GVariant *)>(action, "activate", [fn](GSimpleAction *, GVariant *) { fn(); });
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    g_object_unref(action);
  };
  const auto id = item->fullId();
  const auto owner = &_history->owner();
  const auto weak = base::make_weak(this);
  const auto withItem = [=](auto fn) {
    return [=] {
      if (const auto strong = weak.get()) {
        if (const auto item = owner->message(id)) {
          fn(strong, item);
        }
      }
    };
  };
  add("reply", withItem([](ChatView *self, not_null<HistoryItem *> item) { self->replyTo(item); }));
  add("copy", withItem([](ChatView *, not_null<HistoryItem *> item) {
    CopyToClipboard(item->originalText().text);
    ShowToast(tr::lng_text_copied(tr::now));
  }));
  add("edit", withItem([](ChatView *self, not_null<HistoryItem *> item) { self->edit(item); }));
  add("save", withItem([](ChatView *self, not_null<HistoryItem *> item) {
    const auto media = item->media();
    if (!media) {
      return;
    }
    if (const auto document = media->document()) {
      if (const auto path = document->filepath(true); !path.isEmpty()) {
        const auto target = UniqueDownloadPath(QFileInfo(path).fileName());
        QFile::copy(path, target);
        ShowToast(tr::lng_mediaview_saved_to(tr::now, lt_downloads, tr::lng_downloads_section(tr::now)));
      } else {
        document->save(item->fullId(), UniqueDownloadPath(document->filename()));
        ShowToast(tr::lng_downloads_section(tr::now));
      }
    } else if (const auto photo = media->photo()) {
      const auto view = photo->createMediaView();
      if (const auto image = view->image(Data::PhotoSize::Large)) {
        auto dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        QDir().mkpath(dir);
        const auto path = dir + u"/photo_"_q + QDateTime::currentDateTime().toString(u"yyyy-MM-dd_HH-mm-ss"_q) + u".jpg"_q;
        image->original().save(path, "JPG");
        ShowToast(QFileInfo(path).fileName());
      }
    }
  }));
  add("delete", withItem([](ChatView *self, not_null<HistoryItem *> item) { self->confirmDelete(item); }));
  gtk_widget_insert_action_group(popover, "msg", G_ACTION_GROUP(group));
  g_object_unref(group);
  gtk_popover_popup(GTK_POPOVER(popover));
}

void ChatView::confirmDelete(not_null<HistoryItem *> item) {
  auto *dialog = adw_alert_dialog_new(U8(tr::lng_selected_delete_sure_this(tr::now)).c_str(), nullptr);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", U8(tr::lng_cancel(tr::now)).c_str(), "delete",
                                 U8(tr::lng_box_delete(tr::now)).c_str(), nullptr);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  GtkWidget *revoke = nullptr;
  if (item->canDeleteForEveryone(base::unixtime::now())) {
    revoke = gtk_check_button_new_with_label(U8(tr::lng_delete_for_everyone_check(tr::now)).c_str());
    gtk_check_button_set_active(GTK_CHECK_BUTTON(revoke), TRUE);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), revoke);
  } else if (_history->peer->isChannel()) {
    // Channels and supergroups: deletion is always for everyone.
  }
  const auto id = item->fullId();
  const auto session = &_history->session();
  Connect<void(AdwAlertDialog *, const char *)>(dialog, "response", [=](AdwAlertDialog *, const char *response) {
    if (g_strcmp0(response, "delete") != 0) {
      return;
    }
    const auto all = revoke ? gtk_check_button_get_active(GTK_CHECK_BUTTON(revoke)) : false;
    session->data().histories().deleteMessages({id}, all);
    session->data().sendHistoryChangeNotifications();
  });
  adw_dialog_present(ADW_DIALOG(dialog), _window->widget());
}

void ChatView::openPeer(not_null<PeerData *> peer) {
  _window->openPeer(peer);
}

void ChatView::openPhoto(not_null<PhotoData *> photo, not_null<HistoryItem *> item) {
  _window->openPhoto(photo, item->fullId());
}

void ChatView::openDocument(not_null<DocumentData *> document, not_null<HistoryItem *> item) {
  if (const auto path = document->filepath(true); !path.isEmpty()) {
    LaunchFile(path);
  } else if (document->loading()) {
    document->cancel();
    _openWhenLoaded.erase(document);
  } else {
    document->save(item->fullId(), UniqueDownloadPath(document->filename()));
    // Videos open as soon as they are downloaded, files are opened with a second tap.
    if (document->isVideoFile() || document->isAnimation() || document->isVideoMessage() || document->isImage()) {
      _openWhenLoaded.insert(document);
    }
  }
}

}  // namespace Contrail
