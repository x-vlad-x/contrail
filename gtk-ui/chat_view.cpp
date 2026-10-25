#include "chat_view.hpp"

#include "attach.hpp"
#include "chat_live_locations.hpp"
#include "forward.hpp"
#include "live_map.hpp"
#include "live_location.hpp"
#include "location_ui.hpp"
#include "main_window.hpp"
#include "message.hpp"
#include "reactions.hpp"
#include "scroll_helpers.hpp"
#include "send_queue.hpp"
#include "text.hpp"
#include "userpics.hpp"
#include "voice_recorder.hpp"

#include "api/api_common.h"
#include "api/api_editing.h"
#include "api/api_sending.h"
#include "apiwrap.h"
#include "base/unixtime.h"
#include "chat_helpers/message_field.h"
#include "core/application.h"
#include "core/core_settings.h"
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
#include "data/data_groups.h"
#include "data/data_histories.h"
#include "data/data_message_reactions.h"
#include "data/data_replies_list.h"
#include "data/data_peer_values.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_premium_limits.h"
#include "data/data_send_action.h"
#include "history/view/history_view_send_action.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/notify/data_notify_settings.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "history/view/history_view_item_preview.h"
#include "history/view/history_view_pinned_bar.h"
#include "history/view/history_view_pinned_tracker.h"
#include "history/history_item_components.h"
#include "history/history_item_helpers.h"
#include "data/data_shared_media.h"
#include "main/main_session_settings.h"
#include "storage/storage_shared_media.h"
#include "ui/chat/message_bar.h"
#include "ui/dynamic_image.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/audio/media_audio_capture.h"
#include "media/audio/media_audio_capture_common.h"
#include "media/player/media_player_instance.h"
#include "ui/text/format_values.h"
#include "storage/file_upload.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/chat/attach/attach_send_files_way.h"
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
constexpr auto kTopReserveHigh = 300000;  // Rows removed above the screen leave their height to the reserve.
// The window of messages with rows: this many around a message the chat opens at, rows within this distance
// from the screen kept when the list stands still (well beyond where the next rows are made or loaded, so
// the two don't take turns), and removed only when there are more rows than this.
constexpr auto kWindowMessages = 200;
constexpr auto kKeepHeight = 2.5 * kPreloadHeight;
constexpr auto kMaxRows = 300;
// Voice messages are recorded while the microphone button is held, as in the phone apps.
constexpr auto kRecordCancelDistance = 120.;  // Slide left to cancel.
constexpr auto kRecordLockDistance = 90.;     // Slide up to keep recording without holding.
constexpr auto kRecordMinHold = crl::time(400);
constexpr auto kRecordHintMargin = 16;  // "Slide to cancel" right of the timer, left of the microphone.

// Telegram doesn't take Ogg Opus files (what voice messages are made of) in an album of music: the whole
// album would fail on the server (MEDIA_EMPTY) and its messages stay "sending". They go on their own, the rest
// of the music stays together, in the order chosen.
bool IsOggAudio(const Ui::PreparedFile &file) {
  const auto mime = file.information ? file.information->filemime : QString();
  const auto suffix = QFileInfo(file.path).suffix().toLower();
  return mime == u"audio/ogg"_q || mime == u"audio/opus"_q || mime == u"audio/x-opus+ogg"_q || suffix == u"ogg"_q
         || suffix == u"opus"_q || suffix == u"oga"_q;
}

std::vector<Ui::PreparedGroup> SeparateOggFromMusic(std::vector<Ui::PreparedGroup> &&groups) {
  auto result = std::vector<Ui::PreparedGroup>();
  for (auto &group : groups) {
    if (group.type != Ui::AlbumType::Music
        || std::none_of(group.list.files.begin(), group.list.files.end(), IsOggAudio)) {
      result.push_back(std::move(group));
      continue;
    }
    auto run = Ui::PreparedList();
    const auto flush = [&] {
      if (run.files.empty()) return;
      const auto type = (run.files.size() > 1) ? Ui::AlbumType::Music : Ui::AlbumType::None;
      result.push_back(Ui::PreparedGroup{.list = std::move(run), .type = type});
      run = Ui::PreparedList();
    };
    for (auto &file : group.list.files) {
      if (IsOggAudio(file)) {
        flush();
        auto single = Ui::PreparedList();
        single.files.push_back(std::move(file));
        result.push_back(Ui::PreparedGroup{.list = std::move(single), .type = Ui::AlbumType::None});
      } else {
        run.files.push_back(std::move(file));
      }
    }
    flush();
  }
  return result;
}

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

// A bar under the header, as the phone apps show the pinned message and the live locations: a tap on it does one
// thing, the buttons at its end others. In a revealer, to slide in and out.
GtkWidget *MakeTopBar(const char *cssClass, GtkWidget *prefix, GtkWidget **title, GtkWidget **text,
                      std::vector<GtkWidget *> buttons, Fn<void()> tapped) {
  auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(box, "chat-top-bar");
  gtk_widget_add_css_class(box, cssClass);
  if (prefix) {
    gtk_widget_set_valign(prefix, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), prefix);
  }
  auto *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(column, "top-bar-text");
  gtk_widget_set_hexpand(column, TRUE);
  gtk_widget_set_valign(column, GTK_ALIGN_CENTER);
  *title = gtk_label_new(nullptr);
  gtk_widget_add_css_class(*title, "top-bar-title");
  gtk_label_set_xalign(GTK_LABEL(*title), 0);
  gtk_label_set_ellipsize(GTK_LABEL(*title), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(column), *title);
  *text = gtk_label_new(nullptr);
  gtk_label_set_xalign(GTK_LABEL(*text), 0);
  gtk_label_set_ellipsize(GTK_LABEL(*text), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode(GTK_LABEL(*text), TRUE);
  gtk_box_append(GTK_BOX(column), *text);
  gtk_box_append(GTK_BOX(box), column);
  for (auto *button : buttons) {
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), button);
  }
  // The buttons claim their own taps.
  auto *click = gtk_gesture_click_new();
  Connect<void(GtkGestureClick *, int, double, double)>(click, "released",
                                                        [tapped](GtkGestureClick *, int, double, double) { tapped(); });
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));
  auto *revealer = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
  gtk_revealer_set_child(GTK_REVEALER(revealer), box);
  return revealer;
}

QString ForwardSenderName(not_null<HistoryItem *> item) {
  if (const auto sender = item->originalSender()) return sender->name();
  if (const auto hidden = item->originalHiddenSenderInfo()) return hidden->name;
  return item->from()->name();
}

}  // namespace

ChatView::ChatView(not_null<Data::Thread *> thread, not_null<MainWindow *> window, MsgId showAt,
                   std::optional<double> showAtY)
    : _thread(thread), _history(thread->owningHistory()), _topic(thread->asTopic()), _window(window),
      _scrollTargetY(showAt ? showAtY : std::nullopt), _pendingShowAt(showAt), _readTimer([=] { markAsRead(); }),
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
  data.itemDataChanges() | rpl::on_next([=](not_null<HistoryItem *> item) { refreshItemData(item); }, _lifetime);
  // The core asks to repaint a message when its reactions are due to be asked for again (Reactions::poll).
  data.itemRepaintRequest() | rpl::on_next([=](not_null<const HistoryItem *> item) {
    if (rowFor(item)) pollReactions();
  }, _lifetime);
  Live().changes() | rpl::on_next([=] { refreshLiveBar(); }, _lifetime);
  if (_liveLocations) {
    _liveLocations->changes() | rpl::on_next([=] { refreshLiveBar(); }, _lifetime);
  }
  // Messages to forward were put here (from another chat, or this one).
  session->changes().entryUpdates(_thread, Data::EntryUpdate::Flag::ForwardDraft)
      | rpl::on_next([=] { refreshForwardBar(); }, _lifetime);
  // A replied message came from the server (or turned out deleted): the reply shows it.
  data.itemResizeRequest() | rpl::on_next([=](not_null<const HistoryItem *> item) {
    if (const auto row = rowFor(item); row && row->replyPending()) refreshItem(item);
  }, _lifetime);
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
  // Each message of an album shows its own upload.
  auto &uploader = session->uploader();
  rpl::merge(uploader.photoProgress(), uploader.documentProgress(), uploader.photoFailed(), uploader.documentFailed())
      | rpl::on_next([=](const FullMsgId &id) { refreshTransfer(id); }, _lifetime);
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

  // Recording went on while the window was closed: ready to be sent or deleted.
  if (Recorder().active() && Recorder().thread() == _thread.get()) {
    _recording = true;
    _recordLocked = true;
    _recordStarted = Recorder().started();
    attachRecording();
  }

  // A group's or a channel's reactions (which ones, whether any) come with its full info, as other things the
  // chat needs; the core asks for it at most once in a while.
  _history->peer->updateFull();

  setupPinned();
  refreshForwardBar();
  loadInitial();
}

ChatView::~ChatView() {
  invalidate_weak_ptrs(this);
  // Files prepared for this chat never go to another one.
  _pasteReader = nullptr;
  if (const auto attach = _attach.get()) attach->close();
  // Like HistoryWidget: the sponsored messages shown here leave the history with the chat.
  _history->session().sponsoredMessages().clearItems(_history);
  if (_viewportSource) g_source_remove(_viewportSource);
  _readTimer.cancel();
  _statusTimer.cancel();
  _reserveTimer.cancel();
  _signals.clear();
  _pinnedTracker = nullptr;
  _liveLocations = nullptr;
  // Removing rows must not query messages that are being destroyed.
  gtk_list_box_set_header_func(GTK_LIST_BOX(_list), nullptr, nullptr, nullptr);
  gtk_list_box_set_sort_func(GTK_LIST_BOX(_list), nullptr, nullptr, nullptr);
  // A voice message being recorded goes on without the chat (the window was closed): see VoiceRecorder.
  _recordLifetime.destroy();
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
  _itemRows.clear();
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

  buildTopBars();
}

void ChatView::buildTopBars() {
  // Whoever shares a live location in this chat (they don't go to channels).
  if (!_history->peer->isBroadcast()) {
    _liveLocations = std::make_unique<ChatLiveLocations>(_history);
  }
  auto *liveIcon = gtk_image_new_from_icon_name("contrail-live-location-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(liveIcon), 16);
  gtk_widget_add_css_class(liveIcon, "location-row-icon");
  gtk_widget_add_css_class(liveIcon, "live-location");
  _liveStop = MakeIconButton("window-close-symbolic", U8(tr::lng_group_call_screen_share_stop(tr::now)).c_str(), "flat");
  gtk_widget_add_css_class(_liveStop, "circular");
  _signals.clicked(_liveStop, [this] {
    if (const auto id = Live().sharingIn(_history->peer)) Live().stop(id);
  });
  GtkWidget *liveTitle = nullptr;
  _liveBar = MakeTopBar("live-bar", liveIcon, &liveTitle, &_liveText, {_liveStop}, crl::guard(this, [=] {
    ShowLiveMap(_window, _thread);
  }));
  gtk_label_set_text(GTK_LABEL(liveTitle), U8(tr::lng_live_location(tr::now)).c_str());
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(_root), _liveBar);
  refreshLiveBar();

  // A GtkImage keeps its size whatever the picture: a GtkPicture's height follows its width, which the bar's
  // layout must not depend on (see MakeHeightForWidthBin).
  _pinnedPreview = gtk_image_new();
  gtk_image_set_pixel_size(GTK_IMAGE(_pinnedPreview), 36);
  gtk_widget_add_css_class(_pinnedPreview, "pinned-preview");
  gtk_widget_set_overflow(_pinnedPreview, GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_visible(_pinnedPreview, FALSE);
  _pinnedList = MakeIconButton("view-list-bullet-symbolic",
                               U8(Translated("All Pinned Messages", "Все закреплённые сообщения",
                                             "Усі закріплені повідомлення")).c_str(),
                               "flat");
  gtk_widget_add_css_class(_pinnedList, "circular");
  _signals.clicked(_pinnedList, [this] { showPinnedList(); });
  _pinnedClose = MakeIconButton("window-close-symbolic", U8(tr::lng_pinned_hide_all_hide(tr::now)).c_str(), "flat");
  gtk_widget_add_css_class(_pinnedClose, "circular");
  _signals.clicked(_pinnedClose, [this] {
    const auto item = _history->owner().message(_pinnedTracker ? _pinnedTracker->currentMessageId().message : FullMsgId());
    if (item && item->canPin()) {
      confirmUnpin(item);
    } else {
      confirmHidePinned();
    }
  });
  _pinnedBar = MakeTopBar("pinned-bar", _pinnedPreview, &_pinnedTitle, &_pinnedText, {_pinnedList, _pinnedClose},
                          crl::guard(this, [=] { pinnedBarClicked(); }));
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(_root), _pinnedBar);
}

void ChatView::refreshLiveBar() {
  const auto sharing = bool(Live().sharingIn(_history->peer));
  const auto empty = std::vector<not_null<HistoryItem *>>();
  const auto &active = _liveLocations ? _liveLocations->active() : empty;
  const auto mine = std::any_of(active.begin(), active.end(), [](not_null<HistoryItem *> item) { return IsMyLiveLocation(item); });
  auto names = LiveLocationNames(active);
  if (sharing && !mine) {
    // Being sent: the message hasn't come back yet.
    names = active.empty() ? tr::lng_from_you(tr::now)
                           : Translated("%1 and you", "%1 и вы", "%1 і ви").arg(LiveLocationNames(active));
  }
  gtk_label_set_text(GTK_LABEL(_liveText), U8(names).c_str());
  gtk_widget_set_visible(_liveStop, sharing);
  gtk_revealer_set_reveal_child(GTK_REVEALER(_liveBar), sharing || !active.empty());
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

// --- pinned messages ---------------------------------------------------------

void ChatView::setupPinned() {
  const auto session = &_history->session();
  _pinnedTracker = std::make_unique<HistoryView::PinnedTracker>(_thread);
  HistoryView::PinnedBarContent(session, _pinnedTracker->shownMessageId(), [] {})
      | rpl::on_next([=](Ui::MessageBarContent &&content) {
          _pinnedTitleText = content.title;
          _pinnedTextText = content.text.text;
          _pinnedCount = content.count;
          _pinnedImage = content.preview ? content.preview->image(36 * gtk_widget_get_scale_factor(_pinnedBar)) : QImage();
          refreshPinnedBar();
        }, _lifetime);
  // A message pinned or unpinned somewhere: the bar may come back after it was hidden.
  session->changes().entryUpdates(_thread, Data::EntryUpdate::Flag::HasPinnedMessages)
      | rpl::on_next([=] { refreshPinnedBar(); }, _lifetime);
  updatePinnedViewer();
}

bool ChatView::pinnedHiddenByUser() const {
  const auto peer = _history->peer;
  if (peer->canPinMessages()) return false;
  const auto top = Data::ResolveTopPinnedId(peer, _thread->topicRootId(), _thread->monoforumPeerId());
  const auto hidden = _history->session().settings().hiddenPinnedMessageId(peer->id, _thread->topicRootId(),
                                                                           _thread->monoforumPeerId());
  return top && hidden == top.msg;
}

void ChatView::refreshPinnedBar() {
  const auto shown = (!_pinnedTitleText.isEmpty() || !_pinnedTextText.isEmpty()) && !pinnedHiddenByUser();
  gtk_revealer_set_reveal_child(GTK_REVEALER(_pinnedBar), shown);
  if (!shown) return;
  gtk_label_set_text(GTK_LABEL(_pinnedTitle), U8(_pinnedTitleText).c_str());
  gtk_label_set_text(GTK_LABEL(_pinnedText), U8(QString(_pinnedTextText).replace('\n', ' ')).c_str());
  if (auto *texture = ToTexture(_pinnedImage)) {
    gtk_image_set_from_paintable(GTK_IMAGE(_pinnedPreview), GDK_PAINTABLE(texture));
    g_object_unref(texture);
    gtk_widget_set_visible(_pinnedPreview, TRUE);
  } else {
    gtk_image_clear(GTK_IMAGE(_pinnedPreview));
    gtk_widget_set_visible(_pinnedPreview, FALSE);
  }
  // Several pinned: the list of them; one: unpin it (or hide the bar, without the right to unpin).
  gtk_widget_set_visible(_pinnedList, _pinnedCount > 1);
  gtk_widget_set_visible(_pinnedClose, _pinnedCount <= 1);
}

// The bar shows the last message pinned above the bottom of the screen (HistoryWidget::updatePinnedViewer).
void ChatView::updatePinnedViewer() {
  if (!_pinnedTracker) return;
  auto lessThan = MsgId(ServerMaxMsgId - 1);
  const auto value = gtk_adjustment_get_value(_adjustment);
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  const auto atBottom = _stickToBottom
                        || (value + page >= gtk_adjustment_get_upper(_adjustment) - 40 && shownAtBottom());
  if (_initialLoaded && !atBottom) {
    auto bottom = MsgId();
    for (const auto row : visibleRows()) {
      for (const auto &item : row->items()) {
        if (!item->isRegular() || item->id <= bottom) continue;
        auto *widget = row->widgetFor(item);
        double y = 0;
        const auto height = gtk_widget_get_height(widget);
        if (height > 0 && WidgetViewportY(widget, _scroll, y) && y + height > 0 && y < page) bottom = item->id;
      }
    }
    if (bottom) lessThan = bottom + 1;
  }
  if (_pinnedClicked && !_scrollTarget && lessThan <= _pinnedClicked.msg) {
    // Scrolled above the message shown with the bar.
    _pinnedClicked = FullMsgId();
  }
  if (_pinnedClicked && !_minPinned) {
    _minPinned = Data::ResolveMinPinnedId(_history->peer, _thread->topicRootId(), _thread->monoforumPeerId());
  }
  if (_pinnedClicked && _minPinned && *_minPinned >= _pinnedClicked) {
    // The oldest one was shown: round to the newest again.
    _pinnedTracker->trackAround(ServerMaxMsgId - 1);
  } else {
    _pinnedTracker->trackAround(_pinnedClicked ? std::min(lessThan, _pinnedClicked.msg) : lessThan);
  }
}

void ChatView::pinnedBarClicked() {
  if (!_pinnedTracker) return;
  const auto id = _pinnedTracker->currentMessageId().message;
  if (!id || id.peer != _history->peer->id) return;
  _pinnedClicked = id;
  if (const auto item = _history->owner().message(id)) {
    // A pinned message of an album: the album is shown, the bar goes on before all of it.
    if (const auto group = _history->owner().groups().find(item)) _pinnedClicked = group->items.front()->fullId();
  }
  _minPinned = std::nullopt;
  showAt(id.msg);
  updatePinnedViewer();
}

void ChatView::confirmPin(not_null<HistoryItem *> item) {
  const auto peer = _history->peer;
  const auto top = Data::ResolveTopPinnedId(peer, _thread->topicRootId(), _thread->monoforumPeerId());
  const auto old = top && item->id < top.msg;
  const auto group = peer->isChat() || peer->isMegagroup();
  auto *dialog = adw_alert_dialog_new(U8(old ? tr::lng_pinned_pin_old_sure(tr::now)
                                         : group ? tr::lng_pinned_pin_sure_group(tr::now)
                                                 : tr::lng_pinned_pin_sure(tr::now)).c_str(),
                                      nullptr);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", U8(tr::lng_cancel(tr::now)).c_str(), "pin",
                                 U8(tr::lng_pinned_pin(tr::now)).c_str(), nullptr);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "pin", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "pin");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  // As PinMessageBox: for the other person too in a chat with a user, the members notified in a group.
  GtkWidget *forPeer = nullptr;
  GtkWidget *notify = nullptr;
  if (peer->isUser() && !peer->isSelf()) {
    forPeer = gtk_check_button_new_with_label(U8(tr::lng_pinned_also_for_other(tr::now, lt_user, peer->shortName())).c_str());
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), forPeer);
  } else if (!old && group && !peer->isMonoforum()) {
    notify = gtk_check_button_new_with_label(U8(tr::lng_pinned_notify(tr::now)).c_str());
    gtk_check_button_set_active(GTK_CHECK_BUTTON(notify), TRUE);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), notify);
  }
  const auto id = item->id;
  const auto session = &_history->session();
  Connect<void(AdwAlertDialog *, const char *)>(dialog, "response", [=](AdwAlertDialog *, const char *response) {
    if (g_strcmp0(response, "pin") != 0) return;
    using Flag = MTPmessages_UpdatePinnedMessage::Flag;
    auto flags = MTPmessages_UpdatePinnedMessage::Flags(0);
    if (notify && !gtk_check_button_get_active(GTK_CHECK_BUTTON(notify))) flags |= Flag::f_silent;
    if (forPeer && !gtk_check_button_get_active(GTK_CHECK_BUTTON(forPeer))) flags |= Flag::f_pm_oneside;
    session->api()
        .request(MTPmessages_UpdatePinnedMessage(MTP_flags(flags), peer->input(), MTP_int(id)))
        .done([=](const MTPUpdates &result) { session->api().applyUpdates(result); })
        .fail([=](const MTP::Error &error) { ShowToast(error.type()); })
        .send();
  });
  adw_dialog_present(ADW_DIALOG(dialog), _window->widget());
}

void ChatView::confirmUnpin(not_null<HistoryItem *> item) {
  auto *dialog = adw_alert_dialog_new(U8(tr::lng_pinned_unpin_sure(tr::now)).c_str(), nullptr);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", U8(tr::lng_cancel(tr::now)).c_str(), "unpin",
                                 U8(tr::lng_pinned_unpin(tr::now)).c_str(), nullptr);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "unpin", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  const auto peer = item->history()->peer;
  const auto id = item->id;
  const auto session = &_history->session();
  Connect<void(AdwAlertDialog *, const char *)>(dialog, "response", [=](AdwAlertDialog *, const char *response) {
    if (g_strcmp0(response, "unpin") != 0) return;
    session->api()
        .request(MTPmessages_UpdatePinnedMessage(MTP_flags(MTPmessages_UpdatePinnedMessage::Flag::f_unpin),
                                                 peer->input(), MTP_int(id)))
        .done([=](const MTPUpdates &result) { session->api().applyUpdates(result); })
        .send();
  });
  adw_dialog_present(ADW_DIALOG(dialog), _window->widget());
}

// UnpinAllMessages in window_peer_menu.cpp.
void ChatView::confirmUnpinAll() {
  auto *dialog = adw_alert_dialog_new(U8(tr::lng_pinned_unpin_all_sure(tr::now)).c_str(), nullptr);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", U8(tr::lng_cancel(tr::now)).c_str(), "unpin",
                                 U8(tr::lng_pinned_unpin(tr::now)).c_str(), nullptr);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "unpin", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  const auto history = _history;
  const auto topicRootId = _thread->topicRootId();
  const auto monoforumPeerId = _thread->monoforumPeerId();
  Connect<void(AdwAlertDialog *, const char *)>(dialog, "response", [=](AdwAlertDialog *, const char *response) {
    if (g_strcmp0(response, "unpin") != 0) return;
    const auto api = &history->session().api();
    const auto send = [=](auto self) -> void {
      using Flag = MTPmessages_UnpinAllMessages::Flag;
      api->request(MTPmessages_UnpinAllMessages(MTP_flags(topicRootId ? Flag::f_top_msg_id : Flag()),
                                                history->peer->input(), MTP_int(topicRootId.bare), MTPInputPeer()))
          .done([=](const MTPmessages_AffectedHistory &result) {
            if (api->applyAffectedHistory(history->peer, result) > 0) {
              self(self);
            } else {
              history->unpinMessagesFor(topicRootId, monoforumPeerId);
            }
          })
          .send();
    };
    send(send);
  });
  adw_dialog_present(ADW_DIALOG(dialog), _window->widget());
}

// Without the right to unpin: the bar goes away until something new is pinned (HidePinnedBar).
void ChatView::confirmHidePinned() {
  auto *dialog = adw_alert_dialog_new(nullptr, U8(tr::lng_pinned_hide_all_sure(tr::now)).c_str());
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", U8(tr::lng_cancel(tr::now)).c_str(), "hide",
                                 U8(tr::lng_pinned_hide_all_hide(tr::now)).c_str(), nullptr);
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  const auto weak = base::make_weak(this);
  Connect<void(AdwAlertDialog *, const char *)>(dialog, "response", [=](AdwAlertDialog *, const char *response) {
    const auto strong = weak.get();
    if (!strong || g_strcmp0(response, "hide") != 0) return;
    const auto peer = strong->_history->peer;
    const auto topicRootId = strong->_thread->topicRootId();
    const auto monoforumPeerId = strong->_thread->monoforumPeerId();
    if (const auto top = Data::ResolveTopPinnedId(peer, topicRootId, monoforumPeerId)) {
      auto &session = peer->session();
      session.settings().setHiddenPinnedMessageId(peer->id, topicRootId, monoforumPeerId, top.msg);
      session.saveSettingsDelayed();
      strong->refreshPinnedBar();
    }
  });
  adw_dialog_present(ADW_DIALOG(dialog), _window->widget());
}

// All the pinned messages of the chat, newest first; a tap shows one.
void ChatView::showPinnedList() {
  const auto peer = _history->peer;
  const auto session = &_history->session();
  auto *dialog = adw_dialog_new();
  adw_dialog_set_content_width(ADW_DIALOG(dialog), 480);
  adw_dialog_set_content_height(ADW_DIALOG(dialog), 900);
  auto *view = adw_toolbar_view_new();
  auto *header = adw_header_bar_new();
  auto *title = adw_window_title_new(U8(tr::lng_pinned_message(tr::now)).c_str(), nullptr);
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
  auto *list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(list, "boxed-list");
  gtk_widget_set_valign(list, GTK_ALIGN_START);
  SetMarginAll(list, 12);
  auto *clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(clamp), list);
  auto *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
  gtk_widget_set_vexpand(scroll, TRUE);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroll);
  // Unpin them all, or (without the right to) hide the bar.
  auto *action = gtk_button_new_with_label(
      U8(peer->canPinMessages() ? tr::lng_pinned_unpin_all(tr::now, lt_count, std::max(_pinnedCount, 1))
                                : tr::lng_pinned_hide_all(tr::now)).c_str());
  gtk_widget_add_css_class(action, "pill");
  gtk_widget_add_css_class(action, peer->canPinMessages() ? "destructive-action" : "flat");
  gtk_widget_set_halign(action, GTK_ALIGN_CENTER);
  SetMarginAll(action, 12);
  adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(view), action);
  adw_dialog_set_child(ADW_DIALOG(dialog), view);

  const auto weak = base::make_weak(this);
  Connect<void(GtkButton *)>(action, "clicked", [=](GtkButton *) {
    adw_dialog_close(ADW_DIALOG(dialog));
    if (const auto strong = weak.get()) {
      if (strong->_history->peer->canPinMessages()) {
        strong->confirmUnpinAll();
      } else {
        strong->confirmHidePinned();
      }
    }
  });
  Connect<void(GtkListBox *, GtkListBoxRow *)>(list, "row-activated", [=](GtkListBox *, GtkListBoxRow *row) {
    const auto id = MsgId(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "contrail-msg")));
    adw_dialog_close(ADW_DIALOG(dialog));
    if (const auto strong = weak.get(); strong && id) {
      strong->_pinnedClicked = FullMsgId(strong->_history->peer->id, id);
      strong->_minPinned = std::nullopt;
      strong->showAt(id);
      strong->updatePinnedViewer();
    }
  });
  // The core's list of pinned messages (shared media of the Pinned kind), as the bar's tracker loads it.
  auto lifetime = std::make_unique<rpl::lifetime>();
  SharedMediaMergedViewer(
      session,
      SharedMediaMergedKey(SparseIdsMergedSlice::Key(peer->id, _thread->topicRootId(), _thread->monoforumPeerId(),
                                                     PeerId(0), ServerMaxMsgId - 1),
                           Storage::SharedMediaType::Pinned),
      200, 200)
      | rpl::on_next([=](const SparseIdsMergedSlice &slice) {
          if (const auto count = slice.fullCount()) {
            adw_window_title_set_title(ADW_WINDOW_TITLE(title),
                                       U8(tr::lng_pinned_messages_title(tr::now, lt_count, *count)).c_str());
          }
          gtk_list_box_remove_all(GTK_LIST_BOX(list));
          for (auto i = int(slice.size()); i != 0; --i) {
            const auto item = session->data().message(slice[i - 1]);
            if (!item) continue;
            auto *row = adw_action_row_new();
            const auto from = item->displayFrom() ? item->displayFrom() : item->from().get();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                          Escape(from->name() + u" · "_q + FormatListDate(item->date())).c_str());
            const auto preview = item->toPreview({.hideSender = true, .generateImages = false}).text.text;
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), Escape(QString(preview).replace('\n', ' ')).c_str());
            adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 2);
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
            g_object_set_data(G_OBJECT(row), "contrail-msg", GINT_TO_POINTER(int(item->id.bare)));
            gtk_list_box_append(GTK_LIST_BOX(list), row);
          }
        }, *lifetime);
  AttachData(dialog, "contrail-pinned-list", std::move(lifetime));
  adw_dialog_present(ADW_DIALOG(dialog), _window->widget());
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
        // Before the row with the first unread message (an album's last message may be it).
        const auto unreadBar = unreadAfter && !item->out() && m->lastItem()->id > unreadAfter && item->isRegular()
                               && (!p || p->lastItem()->id <= unreadAfter);
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
                                && p->item()->from() == item->from() && item->date() - p->lastItem()->date() < 15 * 60;
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
          _scrollTargetY = std::nullopt;
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
      windowAround(_pendingShowAt);
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
    // The core keeps the messages loaded before: rows only for the newest of them.
    windowAround(0);
    sync();
    setTopReserve(shownAtTop() ? 0 : kTopReserve, 0);
    scrollToBottom();
    _readTimer.callOnce(500);
    if (_itemRows.size() < 20) {
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
              if (first) self->windowAround(std::max(self->_pendingShowAt, MsgId(0)));
              self->sync();
              if (keepPosition) {
                if (const auto added = self->listHeight() - heightBefore; added > 0) {
                  const auto fromReserve = std::min(added, self->_topReserve);
                  // Without enough room above, the rest moves the content and the position with it.
                  self->setTopReserve(self->_topReserve - fromReserve, added - fromReserve);
                }
              }
              if (first) {
                self->setTopReserve(self->shownAtTop() ? 0 : kTopReserve, 0);
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
                  self->_itemRows.size(), int(history->loadedAtTop()), int(history->loadedAtBottom()));
              if (!list || (first && self->_itemRows.empty() && total > 0)) {
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
  if (first) windowAround(std::max(_pendingShowAt, MsgId(0)));
  sync();
  if (keepPosition) {
    if (const auto added = listHeight() - heightBefore; added > 0) {
      const auto fromReserve = std::min(added, _topReserve);
      setTopReserve(_topReserve - fromReserve, added - fromReserve);
    }
  }
  if (first) {
    setTopReserve(shownAtTop() ? 0 : kTopReserve, 0);
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

// The loaded messages of the chat, oldest first.
std::vector<not_null<HistoryItem *>> ChatView::loadedItems() const {
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

std::vector<not_null<HistoryItem *>> ChatView::items() const {
  auto all = loadedItems();
  if (!_windowMin && !_windowMax) return all;
  auto result = std::vector<not_null<HistoryItem *>>();
  result.reserve(std::min(all.size(), size_t(kWindowMessages * 2)));
  auto started = !_windowMin;
  for (const auto &item : all) {
    if (item->isRegular()) {
      if (_windowMax && item->id > _windowMax) break;
      if (_windowMin && item->id < _windowMin) continue;
      started = true;
    } else if (!started || _windowMax) {
      // Sponsored messages and the ones being sent go with the messages around them.
      continue;
    }
    result.push_back(item);
  }
  return result;
}

namespace {

// The nearest server message's id from `index` on, going by `step`; 0 past the end.
MsgId RegularIdFrom(const std::vector<not_null<HistoryItem *>> &all, int index, int step) {
  for (; index >= 0 && index < int(all.size()); index += step) {
    if (all[index]->isRegular()) return all[index]->id;
  }
  return MsgId();
}

int IndexOf(const std::vector<not_null<HistoryItem *>> &all, MsgId id) {
  const auto i = std::find_if(all.begin(), all.end(), [&](not_null<HistoryItem *> item) {
    return item->isRegular() && item->id >= id;
  });
  return int(i - all.begin());
}

}  // namespace

void ChatView::windowAround(MsgId id) {
  _windowMin = _windowMax = MsgId();
  const auto all = loadedItems();
  const auto count = int(all.size());
  if (count <= kWindowMessages) return;
  const auto index = id ? std::min(IndexOf(all, id), count - 1) : count - 1;
  const auto from = id ? std::max(index - kWindowMessages / 2, 0) : count - kWindowMessages;
  const auto till = id ? std::min(index + kWindowMessages / 2, count - 1) : count - 1;
  if (from > 0) _windowMin = RegularIdFrom(all, from, 1);
  if (till < count - 1) _windowMax = RegularIdFrom(all, till, -1);
}

// Jumping to a loaded message without a row (or to the end, from rows far back): the window moves there whole.
void ChatView::showWindowAround(MsgId id) {
  windowAround(id);
  sync();
  setTopReserve(shownAtTop() ? 0 : kTopReserve, 0);
}

// The next rows above, from the loaded messages: they take the room reserved above, as an older page does.
bool ChatView::extendWindowUp() {
  if (!_windowMin) return false;
  const auto all = loadedItems();
  const auto index = IndexOf(all, _windowMin);
  const auto heightBefore = listHeight();
  _windowMin = (index - kMessagesPerPage > 0) ? RegularIdFrom(all, index - kMessagesPerPage, -1) : MsgId();
  _applyingPage = true;
  sync();
  if (const auto added = listHeight() - heightBefore; added > 0) {
    const auto fromReserve = std::min(added, _topReserve);
    setTopReserve(_topReserve - fromReserve, added - fromReserve);
  }
  _applyingPage = false;
  return true;
}

bool ChatView::extendWindowDown() {
  if (!_windowMax) return false;
  const auto all = loadedItems();
  const auto index = IndexOf(all, _windowMax);
  const auto next = index + kMessagesPerPage;
  _windowMax = (next < int(all.size()) - 1) ? RegularIdFrom(all, next, 1) : MsgId();
  sync();
  return true;
}

// The rows far from the screen go while the list stands still. Above it they leave their height to the room
// reserved there, so nothing on the screen moves.
void ChatView::trimRows(bool force, double keep) {
  if (keep <= 0.) keep = kKeepHeight;
  if ((!force && int(_rows.size()) <= kMaxRows) || _scrollTarget || !_initialLoaded) return;
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  const graphene_point_t zero = GRAPHENE_POINT_INIT_ZERO;
  graphene_point_t top;
  if (page <= 0 || !gtk_widget_compute_point(_scroll, _list, &zero, &top)) return;
  const auto message = [](GtkListBoxRow *row) {
    return row ? static_cast<MessageWidget *>(g_object_get_data(G_OBJECT(row), "contrail-message")) : nullptr;
  };
  const auto list = GTK_LIST_BOX(_list);
  const auto keptBelow = message(gtk_list_box_get_row_at_y(list, int(top.y + page + keep)));
  if (keptBelow && !_stickToBottom
      && gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(keptBelow->row())) < int(_rows.size()) - 1) {
    auto newest = MsgId();
    for (const auto &item : keptBelow->items()) {
      if (item->isRegular()) newest = std::max(newest, item->id);
    }
    if (newest) {
      _windowMax = newest;
      sync();
    }
  }
  const auto aboveY = int(top.y - keep);
  const auto keptAbove = (aboveY > 0) ? message(gtk_list_box_get_row_at_y(list, aboveY)) : nullptr;
  if (keptAbove && gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(keptAbove->row())) > 0) {
    auto oldest = MsgId();
    for (const auto &item : keptAbove->items()) {
      if (item->isRegular() && (!oldest || item->id < oldest)) oldest = item->id;
    }
    if (oldest) {
      const auto heightBefore = listHeight();
      _windowMin = oldest;
      sync();
      if (const auto removed = heightBefore - listHeight(); removed > 0) setTopReserve(_topReserve + removed, 0);
    }
  }
  if (g_getenv("CONTRAIL_DEBUG")) {
    g_message("History window: rows=%zu min=%d max=%d reserve=%d", _rows.size(), int(_windowMin.bare),
              int(_windowMax.bare), _topReserve);
  }
}

// The rows follow the messages: one per message, one per album. A row keeps its widgets when its album
// grows (the rest of it comes with the next page) or shrinks (a message deleted), and when the messages
// of an album being sent get their ids from the server.
void ChatView::sync() {
  auto units = std::vector<std::vector<not_null<HistoryItem *>>>();
  auto unitOfGroup = std::unordered_map<uint64, size_t>();
  auto lastRegular = MsgId();
  _sponsoredAfter.clear();
  const auto &groups = _history->owner().groups();
  for (const auto &item : items()) {
    if (item->isSponsored()) {
      _sponsoredAfter.emplace(item, lastRegular);
    } else if (item->isRegular()) {
      lastRegular = item->id;
    }
    if (const auto groupId = groups.find(item) ? item->groupId().raw() : uint64()) {
      const auto [i, added] = unitOfGroup.emplace(groupId, units.size());
      if (!added) {
        units[i->second].push_back(item);
        continue;
      }
    }
    units.push_back({item});
  }
  auto used = std::set<MessageWidget *>();
  for (auto &unit : units) {
    MessageWidget *row = nullptr;
    for (const auto &item : unit) {
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
  for (auto i = _rows.begin(); i != _rows.end();) {
    if (!used.contains(i->first)) {
      forgetRow(i->first);
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

MessageWidget *ChatView::addRow(std::vector<not_null<HistoryItem *>> items) {
  auto row = std::make_unique<MessageWidget>(std::move(items), this);
  const auto raw = row.get();
  gtk_list_box_append(GTK_LIST_BOX(_list), row->row());
  for (const auto &item : raw->items()) {
    _itemRows[item] = raw;
  }
  if (!raw->refreshMedia()) {
    _waitingMedia.insert(raw);
  }
  _rows.emplace(raw, std::move(row));
  return raw;
}

void ChatView::setRowItems(MessageWidget *row, std::vector<not_null<HistoryItem *>> items) {
  if (row->items() == items) return;
  for (const auto &item : row->items()) {
    if (const auto i = _itemRows.find(item); i != _itemRows.end() && i->second == row) {
      _itemRows.erase(i);
    }
  }
  row->setItems(std::move(items));
  for (const auto &item : row->items()) {
    _itemRows[item] = row;
  }
  if (!row->refreshMedia()) {
    _waitingMedia.insert(row);
  }
  gtk_list_box_row_changed(GTK_LIST_BOX_ROW(row->row()));
}

void ChatView::forgetRow(MessageWidget *row) {
  for (const auto &item : row->items()) {
    _sponsoredViewed.erase(item);
    if (const auto i = _itemRows.find(item); i != _itemRows.end() && i->second == row) {
      _itemRows.erase(i);
    }
  }
  _waitingMedia.erase(row);
}

// The message is about to be destroyed: no row may hold it any longer.
void ChatView::removeRow(const HistoryItem *item) {
  _sponsoredAfter.erase(item);
  _sponsoredViewed.erase(item);
  const auto i = _itemRows.find(item);
  if (i == _itemRows.end()) return;
  const auto row = i->second;
  _itemRows.erase(i);
  auto rest = row->items();
  rest.erase(std::remove(rest.begin(), rest.end(), item), rest.end());
  if (!rest.empty()) {
    setRowItems(row, std::move(rest));
  } else {
    forgetRow(row);
    _rows.erase(row);
  }
  gtk_list_box_invalidate_headers(GTK_LIST_BOX(_list));
}

MessageWidget *ChatView::rowFor(const HistoryItem *item) const {
  const auto i = _itemRows.find(item);
  return (i != _itemRows.end()) ? i->second : nullptr;
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

void ChatView::refreshItemData(const HistoryItem *item) {
  if (auto *row = rowFor(item); row && row->refreshReactions()) {
    gtk_list_box_row_changed(GTK_LIST_BOX_ROW(row->row()));
    return;
  }
  refreshItem(item);
}

void ChatView::refreshMessage(FullMsgId id) {
  if (const auto item = _history->owner().message(id)) refreshItem(item);
}

void ChatView::refreshTransfer(FullMsgId id) {
  if (const auto item = _history->owner().message(id)) {
    if (auto *row = rowFor(item); row && !row->refreshMedia()) {
      _waitingMedia.insert(row);
    }
  }
}

void ChatView::scrollToBottom() {
  // The rows end before the newest messages: they go to the end.
  if (_windowMax) showWindowAround(MsgId());
  _stickToBottom = true;
  _scrollTarget = FullMsgId();
  scheduleViewportUpdate();
}

void ChatView::scrollToItem(not_null<const HistoryItem *> item, bool highlight) {
  _stickToBottom = false;
  _scrollTarget = item->fullId();
  // Coming back to where the chat was: the same place on the screen, nothing lit up.
  _scrollTargetHighlight = highlight && !_scrollTargetY;
  scheduleViewportUpdate();
}

std::pair<MsgId, double> ChatView::scrollState() const {
  if (!_initialLoaded || _stickToBottom) return {};
  const auto value = gtk_adjustment_get_value(_adjustment);
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  if (value + page >= gtk_adjustment_get_upper(_adjustment) - 40 && shownAtBottom()) return {};
  // The topmost message seen, by its id: it is loaded again when the chat is built again.
  auto result = std::pair<MsgId, double>{MsgId(), 0.};
  auto best = std::numeric_limits<double>::max();
  for (const auto &[item, row] : _itemRows) {
    if (!item->isRegular()) continue;
    auto *widget = row->widgetFor(item);
    double y = 0;
    const auto height = gtk_widget_get_height(widget);
    if (height > 0 && WidgetViewportY(widget, _scroll, y) && y + height > 0 && y < page && y < best) {
      best = y;
      result = {item->id, y};
    }
  }
  return result;
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
  const auto wanted = [&] {
    return shownAtTop() ? 0
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
      // A message of an album: its cell, not the album's top.
      const auto y = _scrollTargetY.value_or(gtk_adjustment_get_page_size(_adjustment) / 3);
      if (ScrollWidgetToY(row->widgetFor(item), _scroll, _adjustment, y)) {
        if (_scrollTargetHighlight) row->highlight(item);
        _scrollTarget = FullMsgId();
        _scrollTargetY = std::nullopt;
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
  const auto bottom = value + page >= upper - 40 && shownAtBottom();
  gtk_widget_set_visible(_toBottom, !bottom && upper > page);
  preloadVisibleEdges();
  if (bottom) {
    showSponsored();
  }
  reportSponsoredViews();
  pollReactions();
  updatePinnedViewer();
  if (_window->isActive() && !_readTimer.isActive()) _readTimer.callOnce(300);
}

std::vector<MessageWidget *> ChatView::visibleRows() const {
  auto result = std::vector<MessageWidget *>();
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  const graphene_point_t zero = GRAPHENE_POINT_INIT_ZERO;
  graphene_point_t top;
  if (page <= 0 || !gtk_widget_compute_point(_scroll, _list, &zero, &top)) return result;
  // Above the first message (the room for older pages): the list starts lower on the screen.
  auto *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(_list), std::max(int(top.y), 0));
  if (!row && top.y < 0) row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(_list), 0);
  for (; row; row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(_list), gtk_list_box_row_get_index(row) + 1)) {
    graphene_rect_t bounds;
    if (!gtk_widget_compute_bounds(GTK_WIDGET(row), _list, &bounds)) break;
    if (bounds.origin.y >= top.y + page) break;
    if (auto *message = static_cast<MessageWidget *>(g_object_get_data(G_OBJECT(row), "contrail-message"))) {
      result.push_back(message);
    }
  }
  return result;
}

// The reactions of the messages on the screen are asked for again from time to time, as Telegram Desktop does
// for the ones it paints (in groups and channels: in chats with a user the server sends them).
void ChatView::pollReactions() {
  if (!_window->isActive() || !gtk_widget_get_mapped(_root) || _history->peer->isUser()) return;
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  const auto now = crl::now();
  auto &reactions = _history->owner().reactions();
  for (const auto row : visibleRows()) {
    double y = 0;
    const auto height = gtk_widget_get_height(row->row());
    if (height > 0 && WidgetViewportY(row->row(), _scroll, y) && y + height > 0 && y < page) {
      for (const auto &item : row->items()) {
        if (item->isRegular()) reactions.poll(item, now);
      }
    }
  }
}

// New reactions to the user's messages are read when the messages are on the screen.
void ChatView::readReactions() {
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  auto read = base::flat_set<not_null<HistoryItem *>>();
  for (const auto row : visibleRows()) {
    for (const auto &item : row->items()) {
      if (!item->hasUnreadReaction()) continue;
      double y = 0;
      auto *widget = row->widgetFor(item);
      const auto height = gtk_widget_get_height(widget);
      if (height > 0 && WidgetViewportY(widget, _scroll, y) && y + height > 0 && y < page) read.emplace(item);
    }
  }
  if (!read.empty()) _history->session().api().markContentsRead(read);
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
  if (value - _topReserve < kPreloadHeight) {
    if (!extendWindowUp() && !loadedAtTop()) loadOlder();
  } else if (value + page > upper - kPreloadHeight) {
    if (!extendWindowDown() && !loadedAtBottom()) loadNewer();
  }
}

void ChatView::showAt(MsgId id) {
  _scrollTargetY = std::nullopt;
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
  _itemRows.clear();
  _rows.clear();
  _windowMin = _windowMax = MsgId();
  _pendingShowAt = showAt;
  _stickToBottom = !showAt;
  loadInitial();
}

bool ChatView::scrollToLoaded(MsgId id) {
  if (const auto item = _history->owner().message(_history->peer, id)) {
    if (!rowFor(item) && (_windowMin || _windowMax)) {
      // Loaded, out of the window of rows.
      const auto all = loadedItems();
      if (std::find(all.begin(), all.end(), item) != all.end()) showWindowAround(id);
    }
    if (rowFor(item)) {
      scrollToItem(item, id != _unreadAfter + 1);
      return true;
    }
  }
  // The first unread message: ids have gaps, take the first loaded one after the read ones.
  if (_unreadAfter && id == _unreadAfter + 1) {
    const HistoryItem *first = nullptr;
    for (const auto &[item, row] : _itemRows) {
      if (item->id > _unreadAfter && item->isRegular() && (!first || item->id < first->id)) {
        first = item;
      }
    }
    if (first) {
      scrollToItem(first, false);
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
  for (const auto &[raw, row] : _rows) {
    const auto item = row->item();
    if (!item->allowsEdit(base::unixtime::now())) continue;
    const auto original = composerText();
    const auto reply = _replyTo;
    const auto sample = u"Contrail draft regression"_q;
    setComposerText(sample);
    edit(item);
    cancelReplyOrEdit();
    const auto restored = composerText() == sample && _replyTo == reply;
    setComposerText(original);
    g_message("Draft regression: %s", restored ? "passed" : "FAILED");
    return;
  }
  g_message("Draft regression: skipped (no editable message loaded)");
}

void ChatView::scrolled() {
  const auto value = gtk_adjustment_get_value(_adjustment);
  const auto down = value > _lastScrollValue;
  _lastScrollValue = value;
  if (_adjustingScroll || _applyingPage || !_initialLoaded) return;
  if (down && _pinnedClicked && !_scrollTarget) {
    // Scrolled down by hand: the bar follows the screen again.
    _pinnedClicked = FullMsgId();
    _minPinned = std::nullopt;
  }
  const auto upper = gtk_adjustment_get_upper(_adjustment);
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  const auto atBottom = value + page >= upper - 40 && shownAtBottom();
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
  else if (action.startsWith(u"id:"_q)) showAt(MsgId(action.mid(3).toLongLong()));
  else if (action.startsWith(u"delete:"_q) && _history->peer->isSelf()) {
    // Test messages are sent to Saved Messages only, and deleted there.
    _history->owner().histories().deleteMessages({FullMsgId(_history->peer->id, MsgId(action.mid(7).toLongLong()))}, true);
    _history->owner().sendHistoryChangeNotifications();
  } else if (action.startsWith(u"play:"_q)) {
    const auto item = _history->owner().message(_history->peer, MsgId(action.mid(5).toLongLong()));
    if (const auto document = (item && item->media()) ? item->media()->document() : nullptr) {
      Media::Player::instance()->playPause(AudioMsgId(document, item->fullId()));
    }
  } else if (action.startsWith(u"speed:"_q)) {
    Core::App().settings().setVoicePlaybackSpeed(action.mid(6).toDouble());
  } else if (action.startsWith(u"info:"_q)) {
    if (const auto item = _history->owner().message(_history->peer, MsgId(action.mid(5).toLongLong()))) {
      const auto document = item->media() ? item->media()->document() : nullptr;
      g_message("Message %lld: date=%s group=%llu photo=%d document=%s voice=%d text=%d", (long long)item->id.bare,
                U8(base::unixtime::parse(item->date()).toString(u"yyyy-MM-dd HH:mm:ss"_q)).c_str(),
                (unsigned long long)item->groupId().raw(), int(item->media() && item->media()->photo()),
                document ? U8(document->filename()).c_str() : "-", int(document && document->isVoiceMessage()),
                int(item->originalText().text.size()));
    } else {
      g_message("Message %s: not loaded", U8(action.mid(5)).c_str());
    }
  } else if (action == u"player"_q) {
    const auto state = Media::Player::instance()->getState(AudioMsgId::Type::Voice);
    g_message("Player state: state=%d position=%lld length=%lld frequency=%d", int(state.state),
              (long long)state.position, (long long)state.length, state.frequency);
  }
  else if (action.startsWith(u"photo:"_q)) {
    const auto item = _history->owner().message(_history->peer, MsgId(action.mid(6).toLongLong()));
    if (const auto photo = (item && item->media()) ? item->media()->photo() : nullptr) openPhoto(photo, item);
  }
  else if (action.startsWith(u"find:"_q)) {
    // "history-state find:TEXT": the loaded message with this text at the top third of the screen.
    for (const auto &[item, row] : _itemRows) {
      if (item->originalText().text.contains(action.mid(5))) {
        scrollToItem(item, false);
        g_message("Found message %d: %d characters, %d entities", int(item->id.bare),
                  int(item->originalText().text.size()), int(item->originalText().entities.size()));
        break;
      }
    }
  }
  else if (action.startsWith(u"react:"_q)) {
    // "react:ID:EMOJI" toggles a reaction: only in Saved Messages and in the offline demo.
    const auto parts = action.mid(6).split(':');
    const auto demo = QString::fromUtf8(g_getenv("CONTRAIL_APP_ID") ? g_getenv("CONTRAIL_APP_ID") : "").endsWith(u"Demo"_q);
    const auto item = _history->owner().message(_history->peer, MsgId(parts.value(0).toLongLong()));
    if (item && parts.size() > 1 && (_history->peer->isSelf() || demo)) {
      ToggleReaction(item, Data::ReactionId{parts[1]});
    }
  } else if (action.startsWith(u"reactions:"_q)) {
    if (const auto item = _history->owner().message(_history->peer, MsgId(action.mid(10).toLongLong()))) {
      auto list = QStringList();
      for (const auto &reaction : item->reactionsWithLocal()) {
        list.push_back(Data::ReactionEntityData(reaction.id) + u"="_q + QString::number(reaction.count) + (reaction.my ? u"*"_q : QString()));
      }
      g_message("Reactions of %lld: [%s] can=%d tags=%d unread=%d", (long long)item->id.bare, U8(list.join(' ')).c_str(),
                int(item->canReact()), int(item->reactionsAreTags()), int(item->hasUnreadReaction()));
    }
  } else if (action == u"custom-reactions"_q) {
    // The loaded messages with custom emoji (Telegram Premium) among their reactions.
    for (const auto &[item, row] : _itemRows) {
      for (const auto &reaction : item->reactionsWithLocal()) {
        if (reaction.id.custom()) {
          g_message("Custom reaction on %lld: %llu x%d", (long long)item->id.bare,
                    (unsigned long long)reaction.id.custom(), reaction.count);
        }
      }
    }
  } else if (action.startsWith(u"menu:"_q)) {
    const auto item = _history->owner().message(_history->peer, MsgId(action.mid(5).toLongLong()));
    if (const auto row = item ? rowFor(item) : nullptr) showMessageMenu(item, row->widgetFor(item), 40, 20);
  } else if (action == u"location"_q) {
    chooseLocation();
  } else if (action.startsWith(u"map:"_q)) {
    if (const auto item = _history->owner().message(_history->peer, MsgId(action.mid(4).toLongLong()))) openLocation(item);
  } else if (action.startsWith(u"fix:"_q)) {
    // "fix:LAT,LON,ACCURACY": a position as if from the location service (while something watches it).
    const auto parts = action.mid(4).split(',');
    Location().debugFix(parts.value(0).toDouble(), parts.value(1).toDouble(), parts.value(2, u"10"_q).toDouble());
  } else if (action == u"live"_q) {
    g_message("Live locations: %s; location: %s", U8(Live().debugState()).c_str(), U8(Location().debugState()).c_str());
  } else if (action == u"live-update"_q) {
    Live().debugUpdate();
  } else if (action.startsWith(u"live-start:"_q) && _history->peer->isSelf()) {
    // A live location in Saved Messages (the phone apps don't offer it there), from the last position.
    if (const auto fix = Location().recent(600)) {
      Live().start(_thread, TimeId(action.mid(11).toInt()), *fix, FullMsgId());
    } else {
      g_message("Live locations: no recent position");
    }
  } else if (action == u"trim"_q) {
    trimRows(true);
  } else if (action == u"live-map"_q) {
    ShowLiveMap(_window, _thread);
  } else if (action == u"live-bar"_q) {
    g_message("Live bar: revealed=%d text=%s", int(gtk_revealer_get_reveal_child(GTK_REVEALER(_liveBar))),
              gtk_label_get_text(GTK_LABEL(_liveText)));
  } else if (action == u"pinned"_q) {
    g_message("Pinned bar: revealed=%d title=%s text=%s count=%d current=%d clicked=%d",
              int(gtk_revealer_get_reveal_child(GTK_REVEALER(_pinnedBar))), gtk_label_get_text(GTK_LABEL(_pinnedTitle)),
              gtk_label_get_text(GTK_LABEL(_pinnedText)), _pinnedCount,
              _pinnedTracker ? int(_pinnedTracker->currentMessageId().message.msg.bare) : 0, int(_pinnedClicked.msg.bare));
  } else if (action == u"pinned-click"_q) {
    pinnedBarClicked();
  } else if (action == u"pinned-list"_q) {
    showPinnedList();
  } else if ((action.startsWith(u"pin:"_q) || action.startsWith(u"unpin:"_q)) && _history->peer->isSelf()) {
    // Without the confirmation, in Saved Messages only.
    const auto unpin = action.startsWith(u"unpin:"_q);
    const auto id = MsgId(action.mid(unpin ? 6 : 4).toLongLong());
    const auto session = &_history->session();
    using Flag = MTPmessages_UpdatePinnedMessage::Flag;
    session->api()
        .request(MTPmessages_UpdatePinnedMessage(MTP_flags(unpin ? Flag::f_unpin : Flag(0)), _history->peer->input(),
                                                 MTP_int(id)))
        .done([=](const MTPUpdates &result) { session->api().applyUpdates(result); })
        .send();
  } else if (action.startsWith(u"forward:"_q) && _history->peer->isSelf()) {
    // The message waiting here to be forwarded (to Saved Messages, from Saved Messages).
    const auto id = FullMsgId(_history->peer->id, MsgId(action.mid(8).toLongLong()));
    _history->setForwardDraft(_thread->topicRootId(), _thread->monoforumPeerId(), {.ids = {id}});
  } else if (action.startsWith(u"forward-picker:"_q)) {
    // Only shows the chats to forward to: nothing goes anywhere without a tap.
    if (const auto item = _history->owner().message(_history->peer, MsgId(action.mid(15).toLongLong()))) forward(item);
  } else if (action == u"forward-state"_q) {
    g_message("Forward: shown=%d draft=%d bar=%s / %s", int(_forwardShown), int(forwarding()),
              gtk_label_get_text(GTK_LABEL(_replyTitle)), gtk_label_get_text(GTK_LABEL(_replyText)));
  } else if (action == u"forward-menu"_q) {
    showForwardMenu();
  }
  g_message("History state: rows=%zu window=%d..%d min=%d max=%d top=%d bottom=%d initial=%d requests=%d/%d failed=%d scroll=%.0f/%.0f page=%.0f",
      _itemRows.size(), int(_windowMin.bare), int(_windowMax.bare), int(_history->minMsgId().bare), int(_history->maxMsgId().bare), int(loadedAtTop()),
      int(loadedAtBottom()), int(_initialLoaded), _olderRequest, _newerRequest, int(_loadFailed),
      gtk_adjustment_get_value(_adjustment), gtk_adjustment_get_upper(_adjustment), gtk_adjustment_get_page_size(_adjustment));
  for (const auto &[raw, row] : _rows) {
    if (row->items().size() > 1) {
      auto ids = QStringList();
      for (const auto &item : row->items()) {
        auto *cell = row->widgetFor(item);
        graphene_rect_t bounds;
        if (cell != row->row() && gtk_widget_compute_bounds(cell, row->row(), &bounds)) {
          ids.push_back(QString::number(item->id.bare) + u"@%1,%2 %3x%4"_q.arg(int(bounds.origin.x)).arg(int(bounds.origin.y))
                            .arg(int(bounds.size.width)).arg(int(bounds.size.height)));
        } else {
          ids.push_back(QString::number(item->id.bare));
        }
      }
      g_message("History album: %s", U8(ids.join(',')).c_str());
    }
  }
  const auto zero = std::count_if(_rows.begin(), _rows.end(), [](const auto &pair) {
    return gtk_widget_get_height(pair.second->row()) <= 0;
  });
  g_message("History layout: zero-rows=%d target=%d reserve=%d source=%u mapped=%d sticky=%d list-height=%d kind=%s composer=%s",
      int(zero), int(_scrollTarget.msg.bare), _topReserve, _viewportSource,
      int(gtk_widget_get_mapped(_root)), int(_stickToBottom), gtk_widget_get_height(_list),
      _topic ? "topic" : _history->peer->isUser() ? "user" : _history->peer->isBroadcast() ? "channel" : "group",
      gtk_stack_get_visible_child_name(GTK_STACK(_composerStack)));
}

// "paste" (as Ctrl+V does), "files PATH|PATH" (as the file chooser), anything else goes to the
// attach dialog (AttachDialog::debugAction).
void ChatView::debugAttach(const QString &action) {
  if (action == u"paste"_q) {
    g_signal_emit_by_name(_input, "paste-clipboard");
  } else if (action.startsWith(u"files "_q)) {
    openAttach(action.mid(6).split('|'));
  } else if (action == u"record-start"_q) {
    // As the keyboard does on the microphone button: recording without holding it.
    startRecording();
    lockRecording();
  } else if (action == u"record-send"_q || action == u"record-cancel"_q) {
    finishRecording(action == u"record-send"_q);
  } else if (action == u"clear-field"_q) {
    setComposerText(QString());
    cancelReplyOrEdit();
  } else if (const auto attach = _attach.get()) {
    attach->debugAction(action);
  } else {
    g_message("Attach state: no dialog");
  }
}

void ChatView::markAsRead() {
  if (!_window->isActive() || _window->currentThread() != _thread || !_initialLoaded || _scrollTarget
      || !gtk_widget_get_mapped(_root)) {
    return;
  }
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  if (page <= 0) return;
  readReactions();
  const auto atBottom = gtk_adjustment_get_value(_adjustment) + page >= gtk_adjustment_get_upper(_adjustment) - 40
      && shownAtBottom();
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
  for (const auto row : visibleRows()) {
    for (const auto &item : row->items()) {
      if (item->out() || !item->isRegular() || !item->unread(_thread) || (till && item->id < till->id)) continue;
      double y = 0;
      auto *widget = row->widgetFor(item);
      const auto height = gtk_widget_get_height(widget);
      if (height > 0 && WidgetViewportY(widget, _scroll, y) && y + height >= 0 && y + height <= page) {
        till = item;
      }
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
  _signals.clicked(cancel, [this] {
    if (_replyTo || _editing) {
      cancelReplyOrEdit();
    } else {
      cancelForward();
    }
  });
  gtk_box_append(GTK_BOX(_replyBar), cancel);
  // Messages to forward: a tap on them shows what can be done with them.
  auto *replyClick = gtk_gesture_click_new();
  _signals.connect<void(GtkGestureClick *, int, double, double)>(replyClick, "released",
      [this](GtkGestureClick *, int, double, double) {
        if (_forwardShown && !_replyTo && !_editing) showForwardMenu();
      });
  gtk_widget_add_controller(replyColumn, GTK_EVENT_CONTROLLER(replyClick));
  gtk_widget_set_visible(_replyBar, FALSE);
  gtk_box_append(GTK_BOX(_composer), _replyBar);

  // The message field.
  auto *field = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(field, "composer");
  // A file or a location, as the phone apps' attach menu.
  auto *attachMenu = g_menu_new();
  g_menu_append(attachMenu, U8(tr::lng_attach_file(tr::now)).c_str(), "attach.file");
  g_menu_append(attachMenu, U8(tr::lng_maps_point(tr::now)).c_str(), "attach.location");
  auto *attach = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(attach), "mail-attachment-symbolic");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(attach), G_MENU_MODEL(attachMenu));
  g_object_unref(attachMenu);
  gtk_widget_set_tooltip_text(attach, U8(tr::lng_attach(tr::now)).c_str());
  gtk_widget_add_css_class(attach, "flat");
  gtk_widget_add_css_class(attach, "circular");
  gtk_widget_set_valign(attach, GTK_ALIGN_END);
  auto *attachActions = g_simple_action_group_new();
  const auto addAttachAction = [&](const char *name, Fn<void()> callback) {
    auto *action = g_simple_action_new(name, nullptr);
    _signals.connect<void(GSimpleAction *, GVariant *)>(action, "activate", [callback](GSimpleAction *, GVariant *) { callback(); });
    g_action_map_add_action(G_ACTION_MAP(attachActions), G_ACTION(action));
    g_object_unref(action);
  };
  addAttachAction("file", [this] { chooseFiles(); });
  addAttachAction("location", [this] { chooseLocation(); });
  gtk_widget_insert_action_group(field, "attach", G_ACTION_GROUP(attachActions));
  g_object_unref(attachActions);
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
    refreshSendButton();
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
          const auto list = loadedItems();
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
  // Ctrl+V, Shift+Insert and "Paste" in the field's menu: files and pictures go to the attach dialog,
  // text stays with the field.
  _signals.connect<void(GtkTextView *)>(_input, "paste-clipboard", [this](GtkTextView *view) {
    if (ClassifyPaste(gdk_clipboard_get_formats(gtk_widget_get_clipboard(_input))) != PasteKind::Text) {
      g_signal_stop_emission_by_name(view, "paste-clipboard");
      paste();
    }
  });

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
  if (_recording) {
    // A voice message being recorded keeps its bar until it is sent or deleted.
    if (Data::CanSendTexts(_thread)) {
      gtk_stack_set_visible_child_name(GTK_STACK(_composerStack), "record");
      return;
    }
    finishRecording(false);
  }
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

void ChatView::refreshSendButton() {
  const auto empty = gtk_text_buffer_get_char_count(gtk_text_view_get_buffer(GTK_TEXT_VIEW(_input))) == 0;
  // Messages to forward go without a comment too.
  const auto forward = !_editing && forwarding();
  gtk_widget_set_sensitive(_sendButton, (!empty || forward) && !_editPending);
  gtk_stack_set_visible_child_name(GTK_STACK(_sendStack), (empty && !_editing && !forward) ? "mic" : "send");
}

bool ChatView::forwarding() const {
  return !_history->forwardDraft(_thread->topicRootId(), _thread->monoforumPeerId()).ids.empty();
}

void ChatView::refreshForwardBar() {
  const auto draft = _history->resolveForwardDraft(_thread->topicRootId(), _thread->monoforumPeerId());
  if (draft.items.empty() || _editing) {
    // Shown again when the edit is over.
    if (std::exchange(_forwardShown, false) && !_replyTo && !_editing) gtk_widget_set_visible(_replyBar, FALSE);
    refreshSendButton();
    return;
  }
  // One bar: the messages to forward take the place of a reply.
  _replyTo = FullMsgId();
  const auto count = int(draft.items.size());
  auto senders = QStringList();
  for (const auto &item : draft.items) {
    const auto name = ForwardSenderName(item);
    if (!senders.contains(name)) senders.push_back(name);
  }
  QString text;
  if (draft.options != Data::ForwardOptions::PreserveInfo) {
    text = tr::lng_forward_sender_names_removed(tr::now);
  } else if (senders.size() > 2) {
    text = tr::lng_forwarding_from(tr::now, lt_count, senders.size() - 1, lt_user, senders.front());
  } else if (senders.size() == 2) {
    text = tr::lng_forwarding_from_two(tr::now, lt_user, senders[0], lt_second_user, senders[1]);
  } else if (!senders.isEmpty()) {
    text = senders.front();
  }
  if (count == 1) {
    const auto preview = draft.items.front()->toPreview({.hideSender = true, .generateImages = false}).text.text;
    if (!preview.isEmpty()) text = text.isEmpty() ? preview : (text + u": "_q + preview);
  }
  gtk_image_set_from_icon_name(GTK_IMAGE(_replyIcon), "mail-forward-symbolic");
  gtk_label_set_text(GTK_LABEL(_replyTitle), U8(count == 1 ? tr::lng_forward_title(tr::now)
                                                         : tr::lng_forward_many_title(tr::now, lt_count, count)).c_str());
  gtk_label_set_text(GTK_LABEL(_replyText), U8(text.replace('\n', ' ')).c_str());
  gtk_widget_set_visible(_replyBar, TRUE);
  _forwardShown = true;
  refreshSendButton();
}

void ChatView::cancelForward() {
  _history->setForwardDraft(_thread->topicRootId(), _thread->monoforumPeerId(), {});
}

// With or without the senders' names, or to another chat, as the phone apps offer.
void ChatView::showForwardMenu() {
  const auto draft = _history->forwardDraft(_thread->topicRootId(), _thread->monoforumPeerId());
  if (draft.ids.empty()) return;
  const auto many = draft.ids.size() > 1;
  const auto names = (draft.options == Data::ForwardOptions::PreserveInfo);
  auto *menu = g_menu_new();
  g_menu_append(menu,
                U8(names ? (many ? tr::lng_forward_action_hide_senders(tr::now) : tr::lng_forward_action_hide_sender(tr::now))
                         : (many ? tr::lng_forward_action_show_senders(tr::now) : tr::lng_forward_action_show_sender(tr::now)))
                    .c_str(),
                "fwd.names");
  g_menu_append(menu, U8(tr::lng_forward_change_recipient(tr::now)).c_str(), "fwd.recipient");
  auto *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  g_object_unref(menu);
  gtk_widget_set_parent(popover, _replyBar);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_TOP);
  DestroyPopoverOnClose(popover);
  auto *group = g_simple_action_group_new();
  const auto weak = base::make_weak(this);
  const auto add = [&](const char *name, Fn<void(ChatView *)> fn) {
    auto *action = g_simple_action_new(name, nullptr);
    Connect<void(GSimpleAction *, GVariant *)>(action, "activate", [=](GSimpleAction *, GVariant *) {
      if (const auto strong = weak.get()) fn(strong);
    });
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    g_object_unref(action);
  };
  add("names", [](ChatView *self) {
    auto draft = self->_history->forwardDraft(self->_thread->topicRootId(), self->_thread->monoforumPeerId());
    draft.options = (draft.options == Data::ForwardOptions::PreserveInfo) ? Data::ForwardOptions::NoSenderNames
                                                                          : Data::ForwardOptions::PreserveInfo;
    self->_history->setForwardDraft(self->_thread->topicRootId(), self->_thread->monoforumPeerId(), std::move(draft));
  });
  add("recipient", [](ChatView *self) {
    auto ids = self->_history->forwardDraft(self->_thread->topicRootId(), self->_thread->monoforumPeerId()).ids;
    const auto window = self->_window;
    self->cancelForward();
    ShowForwardPicker(window, std::move(ids));
  });
  gtk_widget_insert_action_group(popover, "fwd", G_ACTION_GROUP(group));
  g_object_unref(group);
  gtk_popover_popup(GTK_POPOVER(popover));
}

void ChatView::forward(not_null<HistoryItem *> item) {
  // An album goes whole.
  auto items = HistoryItemsList{item};
  if (const auto group = _history->owner().groups().find(item)) items = group->items;
  ShowForwardPicker(_window, _history->owner().itemsToIds(items));
}

void ChatView::replyTo(not_null<HistoryItem *> item) {
  if (item->isSponsored()) return;
  if (_editing) {
    cancelReplyOrEdit();
  }
  if (forwarding()) cancelForward();
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
  _forwardShown = false;
  if (wasEditing) {
    setComposerText(base::take(_beforeEditText));
    if (const auto reply = base::take(_beforeEditReplyTo)) {
      if (const auto item = _history->owner().message(reply)) replyTo(item);
    }
  }
  if (!_replyTo) refreshForwardBar();
  refreshSendButton();
}

void ChatView::releaseMemory() {
  // A little beyond where the next rows are made, so they aren't made again right away.
  trimRows(true, kPreloadHeight * 1.25);
}

bool ChatView::handleEscape() {
  if (_recording) {
    finishRecording(false);
    return true;
  } else if (_replyTo || _editing) {
    cancelReplyOrEdit();
    return true;
  } else if (forwarding()) {
    cancelForward();
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
  auto action = Api::SendAction(_thread);
  if (text.isEmpty()) {
    // Only the messages to forward.
    if (forwarding()) {
      session->api().finishForwarding(action);
      scrollToBottom();
    }
    return;
  }
  if (_replyTo) {
    action.replyTo.messageId = _replyTo;
  }
  // The comment goes first, then the messages to forward (ApiWrap::sendMessage takes them along).
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
        if (auto *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source), result, nullptr)) {
          GSList *list = nullptr;
          const auto count = g_list_model_get_n_items(files);
          for (guint i = count; i != 0; --i) {
            list = g_slist_prepend(list, g_list_model_get_item(files, i - 1));
          }
          const auto split = SplitPastedFiles(list);
          g_slist_free_full(list, g_object_unref);
          g_object_unref(files);
          // The chat may have been closed while the dialog was open: then nothing is sent.
          if (const auto strong = weak->get(); strong && !split.local.isEmpty()) {
            RunCoreEvent([&] { strong->openAttach(split.local); });
          }
        }
        delete weak;
      },
      weak);
  g_object_unref(dialog);
}

void ChatView::chooseLocation() {
  if (_editing || _recording || !Data::CanSendTexts(_thread)) return;
  const auto weak = base::make_weak(this);
  ShowLocationPicker(_window, _thread, _replyTo, [=] {
    if (const auto strong = weak.get()) {
      strong->cancelReplyOrEdit();
      strong->scrollToBottom();
    }
  });
}

void ChatView::paste() {
  auto *clipboard = gtk_widget_get_clipboard(_input);
  const auto kind = ClassifyPaste(gdk_clipboard_get_formats(clipboard));
  // A new paste replaces one still being read; the reader dies with this chat, and its answer with it.
  _pasteReader = std::make_unique<ClipboardReader>(clipboard, kind, [this](ClipboardReader::Result &&result) {
    _pasteReader = nullptr;
    if (!result.files.remote.isEmpty()) {
      // Links to remote files are text.
      pasteText(result.files.remote.join('\n'));
    }
    if (!result.files.local.isEmpty()) {
      openAttach(result.files.local);
    } else if (!result.image.isNull()) {
      openAttach({}, std::move(result.image), result.text);
    } else if (result.files.remote.isEmpty() && !result.text.isEmpty()) {
      pasteText(result.text);
    }
  });
}

void ChatView::pasteText(const QString &text) {
  if (text.isEmpty() || !gtk_text_view_get_editable(GTK_TEXT_VIEW(_input))) return;
  auto *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(_input));
  const auto utf8 = text.toUtf8();
  gtk_text_buffer_delete_selection(buffer, TRUE, TRUE);
  gtk_text_buffer_insert_interactive_at_cursor(buffer, utf8.constData(), utf8.size(), TRUE);
  gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(_input), gtk_text_buffer_get_insert(buffer));
}

void ChatView::openAttach(const QStringList &paths, QImage image, const QString &textOnCancel) {
  if (_editing || _recording) return;
  if (!Data::CanSendAnyOf(_thread, Data::FilesSendRestrictions())) {
    const auto error = Data::AnyFileRestrictionError(_history->peer);
    ShowToast(error ? error.text : tr::lng_forward_send_files_cant(tr::now));
    pasteText(textOnCancel);
    return;
  }
  if (const auto open = _attach.get()) open->close();
  // Where the files go is fixed now: the chat, the topic and the reply.
  auto action = Api::SendAction(_thread);
  if (_replyTo) {
    action.replyTo.messageId = _replyTo;
  }
  const auto weak = base::make_weak(this);
  _attach = AttachDialog::Show({
      .parent = _window->widget(),
      .paths = paths,
      .image = std::move(image),
      .caption = composerText().trimmed(),
      .premium = _history->session().premium(),
      .send = [=](Ui::PreparedList &list, const Ui::SendFilesWay &way, const QString &caption) {
        const auto strong = weak.get();
        return strong && strong->sendAttachments(list, way, caption, action);
      },
      .cancelled = [=] {
        // As in Telegram Desktop: the text pasted with a picture goes to the field instead.
        if (const auto strong = weak.get(); strong && Ui::InsertTextOnImageCancel(textOnCancel)) {
          strong->pasteText(textOnCancel);
        }
      },
  });
}

// SendFilesBox::send() and HistoryWidget::sendingFilesConfirmed(): the files go in albums of up to ten
// (Ui::DivideByGroups), the caption with the last album, each album through ApiWrap::sendFiles.
bool ChatView::sendAttachments(Ui::PreparedList &list, const Ui::SendFilesWay &way, const QString &caption,
                               const Api::SendAction &action) {
  const auto peer = _history->peer;
  const auto session = &_history->session();
  const auto compress = way.sendImagesAsPhotos();
  if (const auto error = Data::FileRestrictionError(peer, list, compress)) {
    ShowToast(error.text);
    return false;
  } else if (const auto left = peer->slowmodeSecondsLeft()) {
    ShowToast(tr::lng_slowmode_enabled(tr::now, lt_left, Ui::FormatDurationWordsSlowmode(left)));
    return false;
  }
  const auto limit = Data::PremiumLimits(session).captionLengthCurrent();
  if (const auto over = int(caption.size()) - limit; over > 0) {
    ShowToast(tr::lng_caption_limit_reached(tr::now, lt_count, over));
    return false;
  }
  const auto slowmode = peer->slowmodeApplied();
  auto groups = SeparateOggFromMusic(Ui::DivideByGroups(std::move(list), way, slowmode));
  if (slowmode && groups.size() > 1) {
    // Back as it was, for the dialog.
    for (auto &group : groups) {
      for (auto &file : group.list.files) list.files.push_back(std::move(file));
    }
    ShowToast(tr::lng_slowmode_no_many(tr::now));
    return false;
  }
  if (!caption.isEmpty() && !groups.empty()) {
    auto &group = groups.back();
    auto &files = group.list.files;
    auto &captioned = (group.type == Ui::AlbumType::PhotoVideo) ? files.front() : files.back();
    if (!captioned.isSticker() || compress) {
      captioned.caption = TextWithTags{caption, {}};
    }
  }
  // The caption is the message field's text: the server clears the chat's draft with it (clearDraft).
  SendFileGroups(session, std::move(groups), compress ? SendMediaType::Photo : SendMediaType::File, action);
  // The caption came from the message field.
  if (!_editing) {
    setComposerText(QString());
    cancelReplyOrEdit();
    _history->clearLocalDraft(_thread->topicRootId(), _thread->monoforumPeerId());
    _history->clearCloudDraft(_thread->topicRootId(), _thread->monoforumPeerId());
    _loadedDraft.clear();
    _loadedReplyTo = FullMsgId();
  }
  scrollToBottom();
  return true;
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
  if (_editing || _recording || !Data::CanSendTexts(_thread)) return;
  if (!Recorder().start(_thread, _replyTo)) {
    if (!::Media::Capture::instance()->available()) {
      ShowToast(tr::lng_record_audio_problem(tr::now));
    }
    return;
  }
  _recording = true;
  _recordLocked = false;
  _recordStarted = Recorder().started();
  TriggerFeedback("button-pressed");
  attachRecording();
}

// The recording bar follows the recorder, which may have been started before this chat was built
// (the window was closed while recording).
void ChatView::attachRecording() {
  _recordLifetime.destroy();
  gtk_stack_set_visible_child_name(GTK_STACK(_composerStack), "record");
  showRecordState();
  Recorder().state() | rpl::on_next([=](const VoiceRecorder::State &state) {
    const auto ms = state.duration;
    gtk_label_set_text(GTK_LABEL(_recordTime),
                       U8(Ui::FormatDurationText(ms / 1000) + QLocale().decimalPoint() + QString::number(ms % 1000 / 100)).c_str());
    // The dot breathes with the voice.
    gtk_widget_set_opacity(_recordDot, 0.35 + 0.65 * std::min(1., state.level / 6000.));
  }, _recordLifetime);
  Recorder().failed() | rpl::on_next([=] {
    finishRecording(false);
    ShowToast(tr::lng_record_audio_problem(tr::now));
  }, _recordLifetime);
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
  if (send) {
    cancelReplyOrEdit();
    scrollToBottom();
  }
  Recorder().finish(send);
}

// --- message actions ---------------------------------------------------------

void ChatView::showMessageMenu(not_null<HistoryItem *> item, GtkWidget *widget, double x, double y) {
  if (item->isSponsored()) return;
  auto *menu = g_menu_new();
  const auto now = base::unixtime::now();
  // The reactions on top, as in the phone apps.
  auto *strip = item->isService() ? nullptr : MakeReactionsStrip(item, _window, nullptr);
  if (strip) {
    auto *section = g_menu_new();
    auto *custom = g_menu_item_new(nullptr, nullptr);
    g_menu_item_set_attribute(custom, "custom", "s", "reactions");
    g_menu_append_item(section, custom);
    g_object_unref(custom);
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
    g_object_unref(section);
  }
  if (!item->isService() && Data::CanSendTexts(_thread)) {
    g_menu_append(menu, U8(tr::lng_context_reply_msg(tr::now)).c_str(), "msg.reply");
  }
  if (!item->originalText().text.isEmpty()) {
    g_menu_append(menu, U8(tr::lng_context_copy_text(tr::now)).c_str(), "msg.copy");
  }
  if (!item->isService() && item->allowsForward()) {
    g_menu_append(menu, U8(tr::lng_context_forward_msg(tr::now)).c_str(), "msg.forward");
  }
  if (item->canPin()) {
    if (item->isPinned()) {
      g_menu_append(menu, U8(tr::lng_context_unpin_msg(tr::now)).c_str(), "msg.unpin");
    } else {
      g_menu_append(menu, U8(tr::lng_context_pin_msg(tr::now)).c_str(), "msg.pin");
    }
  }
  if (item->allowsEdit(now)) {
    g_menu_append(menu, U8(tr::lng_context_edit_msg(tr::now)).c_str(), "msg.edit");
  }
  const auto document = item->media() ? item->media()->document() : nullptr;
  const auto photo = item->media() ? item->media()->photo() : nullptr;
  if ((document && !document->sticker()) || photo) {
    g_menu_append(menu, U8(tr::lng_context_save_file(tr::now)).c_str(), "msg.save");
  }
  if (Live().sharing(item->fullId())) {
    g_menu_append(menu, U8(tr::lng_group_call_screen_share_stop(tr::now)).c_str(), "msg.stop-live");
  }
  if (CanShowWhoReacted(item)) {
    g_menu_append(menu, U8(tr::lng_context_seen_reacted(tr::now, lt_count, ReactionsCount(item))).c_str(), "msg.reacted");
  }
  if (item->canDelete()) {
    g_menu_append(menu, U8(tr::lng_context_delete_msg(tr::now)).c_str(), "msg.delete");
  }

  auto *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  g_object_unref(menu);
  if (strip) {
    // Choosing a reaction closes the menu (the popover is destroyed after it is closed).
    auto *frame = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(frame), strip);
    gtk_popover_menu_add_child(GTK_POPOVER_MENU(popover), frame, "reactions");
    for (auto *child = gtk_widget_get_first_child(strip); child; child = gtk_widget_get_next_sibling(child)) {
      if (GTK_IS_BUTTON(child)) {
        Connect<void(GtkButton *)>(child, "clicked", [popover](GtkButton *) { gtk_popover_popdown(GTK_POPOVER(popover)); });
      }
    }
  }
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
  add("forward", withItem([](ChatView *self, not_null<HistoryItem *> item) { self->forward(item); }));
  add("pin", withItem([](ChatView *self, not_null<HistoryItem *> item) { self->confirmPin(item); }));
  add("unpin", withItem([](ChatView *self, not_null<HistoryItem *> item) { self->confirmUnpin(item); }));
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
  add("stop-live", withItem([](ChatView *, not_null<HistoryItem *> item) { Live().stop(item->fullId()); }));
  add("reacted", withItem([](ChatView *self, not_null<HistoryItem *> item) { ShowWhoReacted(self->_window, item); }));
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

void ChatView::openLocation(not_null<HistoryItem *> item) {
  // A live location going on: everyone sharing one in this chat, on one map.
  if (LiveLocationActive(item)) {
    ShowLiveMap(_window, _thread);
  } else {
    ShowLocation(_window, item);
  }
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
