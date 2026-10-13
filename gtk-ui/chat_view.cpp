#include "chat_view.hpp"

#include "main_window.hpp"
#include "message.hpp"
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
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_histories.h"
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
constexpr auto kPreloadHeight = 600.;

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

ChatView::ChatView(not_null<History *> history, not_null<MainWindow *> window, MsgId showAt)
    : _history(history), _window(window), _pendingShowAt(showAt), _readTimer([=] { markAsRead(); }),
      _statusTimer([=] { refreshHeader(); }) {
  _root = adw_toolbar_view_new();
  g_object_ref_sink(_root);
  adw_toolbar_view_set_bottom_bar_style(ADW_TOOLBAR_VIEW(_root), ADW_TOOLBAR_RAISED);
  buildHeader();
  buildList();
  buildComposer();
  refreshHeader();
  refreshComposerState();
  _statusTimer.callEach(30 * 1000);

  const auto session = &_history->session();
  auto &data = session->data();
  data.newItemAdded() | rpl::filter([=](not_null<HistoryItem *> item) { return item->history() == _history; })
      | rpl::on_next([=](not_null<HistoryItem *> item) {
          // Seen right away while the chat is in front (HistoryWidget::newItemAdded): no notification.
          if (item->showNotification() && _stickToBottom && _window->isActive()
              && _window->currentHistory() == _history) {
            _history->owner().histories().readInboxOnNewMessage(item);
            Core::App().notifications().clearFromHistory(_history);
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
  data.sendActionManager().animationUpdated()
      | rpl::filter([=](const Data::SendActionManager::AnimationUpdate &update) {
          return update.textUpdated && update.thread == _history.get();
        })
      | rpl::on_next([=] { refreshHeader(); }, _lifetime);
  session->downloaderTaskFinished() | rpl::on_next([=] {
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
  _activeHandler = Connect<void(GObject *, GParamSpec *)>(_window->widget(), "notify::is-active", [this](GObject *, GParamSpec *) {
    if (_window->isActive()) {
      _readTimer.callOnce(300);
    }
  });

  // Opening a chat with unread messages starts at the first of them, under an "Unread messages" bar.
  if (!_pendingShowAt && _history->unreadCount() > 0 && _history->inboxReadTillId() > 0) {
    _unreadAfter = _history->inboxReadTillId();
    _pendingShowAt = _unreadAfter + 1;
  }
  g_object_set_data(G_OBJECT(_list), "contrail-unread-after", GINT_TO_POINTER(int(_unreadAfter.bare)));

  loadInitial();
}

ChatView::~ChatView() {
  if (_recording) {
    _recordLifetime.destroy();
    ::Media::Capture::instance()->stop();
  }
  _lifetime.destroy();
  g_signal_handler_disconnect(_window->widget(), _activeHandler);
  if (_olderRequest) {
    _history->owner().histories().cancelRequest(_olderRequest);
  }
  if (_newerRequest) {
    _history->owner().histories().cancelRequest(_newerRequest);
  }
  // The draft stays with the chat, as in Telegram Desktop; saved only when it was changed here.
  const auto text = composerText();
  if (!_editing && text != _loadedDraft) {
    if (text.trimmed().isEmpty()) {
      _history->clearLocalDraft(MsgId(), PeerId());
    } else {
      _history->setLocalDraft(std::make_unique<Data::Draft>(
          TextWithTags{text, {}}, FullReplyTo{_replyTo}, SuggestOptions(), MessageCursor(), Data::WebPageDraft()));
    }
    _history->session().api().saveDraftToCloudDelayed(_history);
  }
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
  OnClicked(titleButton, [this] { _window->openProfile(_history->peer); });
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), titleButton);

  auto *avatar = MakeAvatar(_history->peer, 32, true);
  auto *avatarButton = gtk_button_new();
  gtk_button_set_child(GTK_BUTTON(avatarButton), avatar);
  gtk_widget_add_css_class(avatarButton, "flat");
  gtk_widget_add_css_class(avatarButton, "circular");
  OnClicked(avatarButton, [this] { _window->openProfile(_history->peer); });
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), avatarButton);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(_root), header);
}

void ChatView::refreshHeader() {
  const auto peer = _history->peer;
  auto name = peer->isSelf() ? tr::lng_saved_messages(tr::now) : peer->name();
  QString status;
  if (const auto user = peer->asUser()) {
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
  if (const auto action = _history->sendActionPainter()->actionText(); !action.isEmpty()) {
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
      +[](GtkListBoxRow *a, GtkListBoxRow *b, gpointer) -> int {
        auto *ma = static_cast<MessageWidget *>(g_object_get_data(G_OBJECT(a), "contrail-message"));
        auto *mb = static_cast<MessageWidget *>(g_object_get_data(G_OBJECT(b), "contrail-message"));
        if (!ma || !mb) {
          return 0;
        }
        const auto ia = ma->item(), ib = mb->item();
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
      nullptr, nullptr);
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
        if (newDay || unreadBar) {
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
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 900);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 700);
  adw_clamp_set_child(ADW_CLAMP(clamp), _list);

  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  _loading = adw_spinner_new();
  gtk_widget_set_size_request(_loading, 24, 24);
  gtk_widget_set_margin_top(_loading, 8);
  gtk_widget_set_margin_bottom(_loading, 8);
  gtk_widget_set_visible(_loading, FALSE);
  gtk_box_append(GTK_BOX(box), _loading);
  gtk_box_append(GTK_BOX(box), clamp);
  gtk_widget_set_valign(box, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom(box, 6);

  _scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(_scroll), box);
  gtk_widget_set_vexpand(_scroll, TRUE);
  gtk_widget_add_css_class(_scroll, "chat-background");
  _adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(_scroll));
  Connect<void(GtkAdjustment *)>(_adjustment, "value-changed", [this](GtkAdjustment *) { scrolled(); });
  Connect<void(GObject *, GParamSpec *)>(_adjustment, "notify::upper", [this](GObject *, GParamSpec *) {
    const auto upper = gtk_adjustment_get_upper(_adjustment);
    const auto page = gtk_adjustment_get_page_size(_adjustment);
    if (_anchorUpper >= 0) {
      // Older messages were added on top: keep the same messages in view.
      gtk_adjustment_set_value(_adjustment, _anchorValue + (upper - _anchorUpper));
      _anchorUpper = -1;
    } else if (_stickToBottom) {
      gtk_adjustment_set_value(_adjustment, upper - page);
    }
  });
  Connect<void(GObject *, GParamSpec *)>(_adjustment, "notify::page-size", [this](GObject *, GParamSpec *) {
    if (_stickToBottom) {
      gtk_adjustment_set_value(_adjustment, gtk_adjustment_get_upper(_adjustment) - gtk_adjustment_get_page_size(_adjustment));
    }
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
  OnClicked(_toBottom, [this] {
    if (!_history->loadedAtBottom()) {
      // Jumped somewhere into the past: reload the end of the chat.
      _rows.clear();
      _waitingMedia.clear();
      _history->getReadyFor(ShowAtTheEndMsgId);
      _initialLoaded = false;
      loadInitial();
    } else {
      scrollToBottom();
    }
  });
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), _toBottom);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(_root), overlay);
}

void ChatView::loadInitial() {
  if (_pendingShowAt > 0) {
    if (_history->owner().message(_history->peer, _pendingShowAt)) {
      sync();
      _initialLoaded = true;
      if (!scrollToLoaded(base::take(_pendingShowAt))) {
        scrollToBottom();
      }
      return;
    }
    _history->getReadyFor(_pendingShowAt);
    requestHistory(_pendingShowAt, -kMessagesPerPage / 2, kMessagesPerPage, true, true);
    return;
  }
  if (!_history->isEmpty() && _history->loadedAtBottom()) {
    _initialLoaded = true;
    sync();
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
  if (_olderRequest || !_initialLoaded || _history->loadedAtTop()) {
    return;
  }
  const auto minId = _history->minMsgId();
  if (!minId) {
    return;
  }
  requestHistory(minId, 0, kMessagesPerPage, true, false);
}

void ChatView::loadNewer() {
  if (_newerRequest || !_initialLoaded || _history->loadedAtBottom()) {
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
  gtk_widget_set_visible(_loading, TRUE);
  auto &request = older ? _olderRequest : _newerRequest;
  request = history->owner().histories().sendRequest(
      history, Data::Histories::RequestType::History, [=](Fn<void()> finish) {
        return history->session()
            .api()
            .request(MTPmessages_GetHistory(history->peer->input(), MTP_int(offsetId), MTP_int(0), MTP_int(addOffset),
                                            MTP_int(limit), MTP_int(0), MTP_int(0), MTP_long(0)))
            .done([=](const MTPmessages_Messages &result) {
              (older ? _olderRequest : _newerRequest) = 0;
              gtk_widget_set_visible(_loading, FALSE);
              const QVector<MTPMessage> *list = nullptr;
              result.match(
                  [&](const MTPDmessages_messagesNotModified &) {},
                  [&](const auto &data) {
                    history->owner().processUsers(data.vusers());
                    history->owner().processChats(data.vchats());
                    list = &data.vmessages().v;
                  });
              result.match([&](const MTPDmessages_channelMessages &data) {
                if (const auto channel = history->peer->asChannel()) {
                  channel->ptsReceived(data.vpts().v);
                }
              }, [](const auto &) {});
              if (list) {
                if (older || initial) {
                  if (!initial) {
                    _anchorUpper = gtk_adjustment_get_upper(_adjustment);
                    _anchorValue = gtk_adjustment_get_value(_adjustment);
                  }
                  history->addOlderSlice(*list);
                  if (initial && offsetId > 0) {
                    history->addNewerSlice({});
                  }
                } else {
                  history->addNewerSlice(*list);
                }
              }
              finish();
              const auto first = !_initialLoaded;
              _initialLoaded = true;
              sync();
              if (first) {
                if (_pendingShowAt > 0) {
                  if (!scrollToLoaded(base::take(_pendingShowAt))) {
                    scrollToBottom();
                  }
                } else {
                  scrollToBottom();
                  _readTimer.callOnce(500);
                }
              }
            })
            .fail([=] {
              (older ? _olderRequest : _newerRequest) = 0;
              gtk_widget_set_visible(_loading, FALSE);
              _anchorUpper = -1;
              finish();
            })
            .send();
      });
}

void ChatView::sync() {
  std::set<const HistoryItem *> present;
  for (const auto &block : _history->blocks) {
    for (const auto &view : block->messages) {
      const auto item = view->data();
      present.insert(item);
      if (!_rows.contains(item)) {
        addRow(item);
      }
    }
  }
  for (auto i = _rows.begin(); i != _rows.end();) {
    if (!present.contains(i->first)) {
      _waitingMedia.erase(i->second.get());
      i = _rows.erase(i);
    } else {
      ++i;
    }
  }
  gtk_list_box_invalidate_sort(GTK_LIST_BOX(_list));
  gtk_list_box_invalidate_headers(GTK_LIST_BOX(_list));
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
  gtk_adjustment_set_value(_adjustment, gtk_adjustment_get_upper(_adjustment) - gtk_adjustment_get_page_size(_adjustment));
}

void ChatView::scrollToRow(MessageWidget *row, bool highlight) {
  _stickToBottom = false;
  // Wait for the layout, then put the message in the middle of the screen.
  auto *weak = new base::weak_ptr<ChatView>(this);
  auto *rowWidget = row->row();
  g_object_ref(rowWidget);
  g_timeout_add_once(
      50,
      +[](gpointer data) {
        auto *pair = static_cast<std::pair<base::weak_ptr<ChatView> *, GtkWidget *> *>(data);
        if (const auto strong = pair->first->get()) {
          graphene_point_t point;
          const graphene_point_t zero = GRAPHENE_POINT_INIT_ZERO;
          if (gtk_widget_compute_point(pair->second, gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(strong->_scroll)),
                                       &zero, &point)) {
            const auto page = gtk_adjustment_get_page_size(strong->_adjustment);
            gtk_adjustment_set_value(strong->_adjustment, point.y - page / 3);
          }
        }
        g_object_unref(pair->second);
        delete pair->first;
        delete pair;
      },
      new std::pair<base::weak_ptr<ChatView> *, GtkWidget *>(weak, rowWidget));
  if (highlight) {
    row->highlight();
  }
}

void ChatView::showAt(MsgId id) {
  if (scrollToLoaded(id)) {
    return;
  }
  // Not loaded: load the messages around it.
  _rows.clear();
  _waitingMedia.clear();
  _pendingShowAt = id;
  _initialLoaded = false;
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
    _stickToBottom = false;
    gtk_adjustment_set_value(_adjustment, 0);
  } else {
    scrollToBottom();
  }
}

void ChatView::scrolled() {
  const auto value = gtk_adjustment_get_value(_adjustment);
  const auto upper = gtk_adjustment_get_upper(_adjustment);
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  const auto atBottom = (value + page >= upper - 40) && _history->loadedAtBottom();
  if (_anchorUpper < 0) {
    _stickToBottom = atBottom;
  }
  gtk_widget_set_visible(_toBottom, !atBottom && upper > page);
  if (value < kPreloadHeight) {
    loadOlder();
  }
  if (value + page > upper - kPreloadHeight) {
    loadNewer();
  }
  if (atBottom) {
    _readTimer.callOnce(300);
  }
}

void ChatView::markAsRead() {
  if (!_window->isActive() || _window->currentHistory() != _history || !_stickToBottom) {
    return;
  }
  if (_history->unreadCount() > 0 || _history->unreadMark() || _history->chatListBadgesState().unread) {
    _history->owner().histories().readInbox(_history);
  }
  Core::App().notifications().clearFromHistory(_history);
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
  OnClicked(cancel, [this] { cancelReplyOrEdit(); });
  gtk_box_append(GTK_BOX(_replyBar), cancel);
  gtk_widget_set_visible(_replyBar, FALSE);
  gtk_box_append(GTK_BOX(_composer), _replyBar);

  // The message field.
  auto *field = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(field, "composer");
  auto *attach = MakeIconButton("mail-attachment-symbolic", U8(tr::lng_attach(tr::now)).c_str(), "flat");
  gtk_widget_add_css_class(attach, "circular");
  gtk_widget_set_valign(attach, GTK_ALIGN_END);
  OnClicked(attach, [this] { chooseFiles(); });
  gtk_box_append(GTK_BOX(field), attach);

  _input = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(_input), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(_input), FALSE);
  gtk_text_view_set_input_hints(GTK_TEXT_VIEW(_input), GtkInputHints(GTK_INPUT_HINT_SPELLCHECK | GTK_INPUT_HINT_WORD_COMPLETION | GTK_INPUT_HINT_EMOJI | GTK_INPUT_HINT_UPPERCASE_SENTENCES));
  gtk_widget_add_css_class(_input, "message-input");
  auto *inputScroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(inputScroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(inputScroll), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(inputScroll), 160);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(inputScroll), _input);
  gtk_widget_set_hexpand(inputScroll, TRUE);
  gtk_widget_set_valign(inputScroll, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(inputScroll, "message-input-frame");
  gtk_box_append(GTK_BOX(field), inputScroll);

  // Placeholder: a label over the empty field.
  auto *placeholder = gtk_label_new(U8(_history->peer->isBroadcast() ? tr::lng_broadcast_ph(tr::now) : tr::lng_message_ph(tr::now)).c_str());
  gtk_widget_add_css_class(placeholder, "dim-label");
  gtk_widget_set_can_target(placeholder, FALSE);
  gtk_label_set_xalign(GTK_LABEL(placeholder), 0);
  gtk_text_view_add_overlay(GTK_TEXT_VIEW(_input), placeholder, 0, 0);
  auto *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(_input));
  Connect<void(GtkTextBuffer *)>(buffer, "changed", [this, placeholder](GtkTextBuffer *buffer) {
    const auto empty = gtk_text_buffer_get_char_count(buffer) == 0;
    gtk_widget_set_visible(placeholder, empty);
    gtk_widget_set_sensitive(_sendButton, !empty || _editing);
    gtk_stack_set_visible_child_name(GTK_STACK(_sendStack), (empty && !_editing) ? "mic" : "send");
    if (!empty) {
      _history->session().sendProgressManager().update(_history, Api::SendProgressType::Typing);
    }
  });

  _sendButton = MakeIconButton("contrail-send-symbolic", U8(tr::lng_send_button(tr::now)).c_str(), "suggested-action");
  gtk_widget_add_css_class(_sendButton, "circular");
  gtk_widget_set_valign(_sendButton, GTK_ALIGN_END);
  OnClicked(_sendButton, [this] { send(); });
  // With an empty field the button records a voice message.
  auto *mic = MakeIconButton("audio-input-microphone-symbolic", U8(tr::lng_shortcuts_record_voice_message(tr::now)).c_str(), "flat");
  gtk_widget_add_css_class(mic, "circular");
  OnClicked(mic, [this] { startRecording(); });
  _sendStack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(_sendStack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_add_named(GTK_STACK(_sendStack), mic, "mic");
  gtk_stack_add_named(GTK_STACK(_sendStack), _sendButton, "send");
  gtk_widget_set_valign(_sendStack, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(field), _sendStack);

  // Enter sends, Shift+Enter starts a new line.
  auto *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  Connect<gboolean(GtkEventControllerKey *, guint, guint, GdkModifierType)>(
      keys, "key-pressed", [this](GtkEventControllerKey *, guint key, guint, GdkModifierType state) -> gboolean {
        if ((key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) && !(state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK))) {
          send();
          return TRUE;
        } else if (key == GDK_KEY_Up && composerText().isEmpty() && !_editing) {
          // Up in an empty field edits the last sent message.
          for (auto i = _history->blocks.rbegin(); i != _history->blocks.rend(); ++i) {
            for (auto j = (*i)->messages.rbegin(); j != (*i)->messages.rend(); ++j) {
              const auto item = (*j)->data();
              if (item->out() && item->allowsEdit(base::unixtime::now())) {
                edit(item);
                return TRUE;
              }
            }
          }
        }
        return FALSE;
      });
  gtk_widget_add_controller(_input, keys);

  _composerStack = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(_composerStack), field, "field");
  _joinButton = gtk_button_new_with_label("");
  gtk_widget_add_css_class(_joinButton, "flat");
  gtk_widget_add_css_class(_joinButton, "join-button");
  OnClicked(_joinButton, [this] {
    const auto channel = _history->peer->asChannel();
    if (channel && !channel->amIn()) {
      _history->session().api().joinChannel(channel);
    } else {
      auto value = Data::MuteValue();
      if (_history->muted()) {
        value.unmute = true;
      } else {
        value.forever = true;
      }
      _history->owner().notifySettings().update(_history, value);
    }
  });
  gtk_stack_add_named(GTK_STACK(_composerStack), _joinButton, "join");

  // Recording a voice message.
  auto *record = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(record, "composer");
  auto *cancelRecord = MakeIconButton("user-trash-symbolic", U8(tr::lng_cancel(tr::now)).c_str(), "flat");
  gtk_widget_add_css_class(cancelRecord, "circular");
  OnClicked(cancelRecord, [this] { finishRecording(false); });
  gtk_box_append(GTK_BOX(record), cancelRecord);
  auto *dot = gtk_label_new("\u25CF");
  gtk_widget_add_css_class(dot, "recording-dot");
  gtk_box_append(GTK_BOX(record), dot);
  _recordTime = gtk_label_new("0:00");
  gtk_widget_add_css_class(_recordTime, "numeric");
  gtk_widget_set_hexpand(_recordTime, TRUE);
  gtk_label_set_xalign(GTK_LABEL(_recordTime), 0);
  gtk_box_append(GTK_BOX(record), _recordTime);
  auto *sendRecord = MakeIconButton("contrail-send-symbolic", U8(tr::lng_send_button(tr::now)).c_str(), "suggested-action");
  gtk_widget_add_css_class(sendRecord, "circular");
  OnClicked(sendRecord, [this] { finishRecording(true); });
  gtk_box_append(GTK_BOX(record), sendRecord);
  gtk_stack_add_named(GTK_STACK(_composerStack), record, "record");
  gtk_box_append(GTK_BOX(_composer), _composerStack);
  adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(_root), _composer);

  // Restore the draft (the core turns one typed on another device into the local one).
  if (const auto draft = _history->localDraft(MsgId(), PeerId())) {
    _loadedDraft = draft->textWithTags.text;
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
  if (channel && !channel->amIn() && !channel->isForum()) {
    gtk_button_set_label(GTK_BUTTON(_joinButton), U8(channel->isBroadcast() ? tr::lng_profile_join_channel(tr::now)
                                                                            : tr::lng_profile_join_group(tr::now))
                                                      .c_str());
    gtk_stack_set_visible_child_name(GTK_STACK(_composerStack), "join");
  } else if (!Data::CanSendTexts(_history)) {
    gtk_button_set_label(GTK_BUTTON(_joinButton),
                         U8(_history->muted() ? tr::lng_channel_unmute(tr::now) : tr::lng_channel_mute(tr::now)).c_str());
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
  gtk_text_buffer_set_text(buffer, utf8.constData(), utf8.size());
}

void ChatView::showReplyBar(const QString &title, const QString &text, const char *icon) {
  gtk_image_set_from_icon_name(GTK_IMAGE(_replyIcon), icon);
  gtk_label_set_text(GTK_LABEL(_replyTitle), U8(title).c_str());
  gtk_label_set_text(GTK_LABEL(_replyText), U8(QString(text).replace('\n', ' ')).c_str());
  gtk_widget_set_visible(_replyBar, TRUE);
  gtk_widget_grab_focus(_input);
}

void ChatView::replyTo(not_null<HistoryItem *> item) {
  if (_editing) {
    cancelReplyOrEdit();
  }
  _replyTo = item->fullId();
  const auto from = item->displayFrom() ? item->displayFrom() : item->from().get();
  showReplyBar(from->name(), item->toPreview({.hideSender = true, .generateImages = false}).text.text,
               "mail-reply-sender-symbolic");
}

void ChatView::edit(not_null<HistoryItem *> item) {
  _replyTo = FullMsgId();
  _editing = item->fullId();
  const auto &original = item->originalText();
  setComposerText(original.text);
  showReplyBar(tr::lng_edit_message(tr::now), original.text, "document-edit-symbolic");
  gtk_widget_set_sensitive(_sendButton, TRUE);
}

void ChatView::cancelReplyOrEdit() {
  if (_editing) {
    setComposerText(QString());
  }
  _replyTo = FullMsgId();
  _editing = FullMsgId();
  gtk_widget_set_visible(_replyBar, FALSE);
}

bool ChatView::handleEscape() {
  if (_replyTo || _editing) {
    cancelReplyOrEdit();
    return true;
  }
  return false;
}

void ChatView::send() {
  const auto text = composerText().trimmed();
  const auto session = &_history->session();
  if (_editing) {
    const auto item = session->data().message(_editing);
    if (item && !text.isEmpty()) {
      auto sending = TextWithEntities{text};
      TextUtilities::PrepareForSending(sending, TextParseLinks | TextParseMentions | TextParseHashtags | TextParseBotCommands);
      const auto weak = base::make_weak(this);
      Api::EditTextMessage(
          item, sending, Data::WebPageDraft(), Api::SendOptions(),
          [](mtpRequestId) {},
          [](const QString &error, mtpRequestId) {
            if (error != u"MESSAGE_NOT_MODIFIED"_q) {
              ShowToast(error);
            }
          },
          false);
    }
    cancelReplyOrEdit();
    setComposerText(QString());
    return;
  }
  if (text.isEmpty()) {
    return;
  }
  auto action = Api::SendAction(_history);
  if (_replyTo) {
    action.replyTo = FullReplyTo{.messageId = _replyTo};
  }
  auto message = Api::MessageToSend(action);
  message.textWithTags = TextWithTags{text, {}};
  session->api().sendMessage(std::move(message));
  setComposerText(QString());
  cancelReplyOrEdit();
  _history->clearLocalDraft(MsgId(), PeerId());
  _history->clearCloudDraft(MsgId(), PeerId());
  scrollToBottom();
}

void ChatView::sendText(const QString &text) {
  auto message = Api::MessageToSend(Api::SendAction(_history));
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
  const auto session = &_history->session();
  auto list = Storage::PrepareMediaList(paths, 400, session->premium());
  if (list.error != Ui::PreparedList::Error::None) {
    ShowToast(list.errorData.isEmpty() ? tr::lng_send_image_empty(tr::now, lt_name, paths.front()) : list.errorData);
    return;
  }
  // Pictures go as photos, everything else as files, each one separately.
  auto action = Api::SendAction(_history);
  if (_replyTo) {
    action.replyTo = FullReplyTo{.messageId = _replyTo};
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

void ChatView::startRecording() {
  const auto capture = ::Media::Capture::instance();
  if (_recording || !capture->available()) {
    if (!capture->available()) {
      ShowToast(tr::lng_record_audio_problem(tr::now));
    }
    return;
  }
  _recording = true;
  Media::Player::instance()->pause(AudioMsgId::Type::Voice);
  gtk_label_set_text(GTK_LABEL(_recordTime), "0:00");
  gtk_stack_set_visible_child_name(GTK_STACK(_composerStack), "record");
  capture->updated() | rpl::on_next_error([=](const ::Media::Capture::Update &update) {
    const auto ms = update.samples * crl::time(1000) / ::Media::Player::kDefaultFrequency;
    gtk_label_set_text(GTK_LABEL(_recordTime), U8(Ui::FormatDurationText(ms / 1000)).c_str());
  }, [=](::Media::Capture::Error) {
    finishRecording(false);
    ShowToast(tr::lng_record_audio_problem(tr::now));
  }, _recordLifetime);
  _history->session().sendProgressManager().update(_history, Api::SendProgressType::RecordVoice);
  capture->start();
}

void ChatView::finishRecording(bool send) {
  if (!_recording) {
    return;
  }
  _recording = false;
  _recordLifetime.destroy();
  refreshComposerState();
  const auto history = _history;
  auto action = Api::SendAction(_history);
  if (_replyTo) {
    action.replyTo = FullReplyTo{.messageId = _replyTo};
  }
  if (!send) {
    ::Media::Capture::instance()->stop();
    history->session().sendProgressManager().cancel(history, Api::SendProgressType::RecordVoice);
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
  auto *menu = g_menu_new();
  const auto now = base::unixtime::now();
  if (!item->isService() && Data::CanSendTexts(_history)) {
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
  Connect<void(GtkPopover *)>(popover, "closed", [](GtkPopover *p) {
    g_idle_add_once(+[](gpointer w) { gtk_widget_unparent(GTK_WIDGET(w)); }, p);
  });

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
