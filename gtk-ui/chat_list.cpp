#include "chat_list.hpp"

#include "demo.hpp"

#include "main_window.hpp"
#include "text.hpp"
#include "userpics.hpp"

#include "apiwrap.h"
#include "core/application.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_folder.h"
#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "data/data_histories.h"
#include "data/data_peer.h"
#include "data/data_send_action.h"
#include "history/view/history_view_send_action.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/notify/data_notify_settings.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_item_preview.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/mtproto_dc_options.h"
#include "mtproto/mtp_instance.h"

namespace Contrail {
namespace {

constexpr auto kRowKey = "contrail-entry";

QString EntryName(not_null<Dialogs::Entry *> entry) {
  if (const auto history = entry->asHistory()) {
    if (history->peer->isSelf()) {
      return tr::lng_saved_messages(tr::now);
    } else if (history->peer->isRepliesChat()) {
      return tr::lng_replies_messages(tr::now);
    }
    return history->peer->name();
  } else if (entry->asFolder()) {
    return tr::lng_archived_name(tr::now);
  }
  return entry->chatListName();
}

TextWithEntities EntryPreview(not_null<Dialogs::Entry *> entry) {
  if (const auto folder = entry->asFolder()) {
    // The archive: names of the chats on top of it.
    QStringList names;
    for (const auto &row : folder->chatsList()->indexed()->all()) {
      if (const auto history = row->history()) {
        names.push_back(history->peer->name());
        if (names.size() >= 4) {
          break;
        }
      }
    }
    return {names.join(u", "_q)};
  }
  const auto thread = entry->asThread();
  if (!thread) {
    return {};
  }
  const auto history = thread->owningHistory();
  if (const auto draft = history->localDraft(thread->topicRootId(), thread->monoforumPeerId())) {
    if (!draft->textWithTags.text.isEmpty()) {
      auto result = Ui::Text::Colorized(tr::lng_from_draft(tr::now) + u": "_q);
      result.append(draft->textWithTags.text);
      return result;
    }
  }
  const auto item = entry->chatListMessage();
  if (!item) {
    return {};
  }
  auto preview = item->toPreview({.hideSender = false, .generateImages = false});
  if (item->out() && !item->isService() && !history->peer->isSelf() && !history->peer->isBroadcast()
      && !preview.text.text.startsWith(tr::lng_from_you(tr::now))) {
    auto result = Ui::Text::Colorized(tr::lng_from_you(tr::now) + u": "_q);
    result.append(std::move(preview.text));
    return result;
  }
  return std::move(preview.text);
}

// One line: newlines in a preview would break the row.
TextWithEntities OneLine(TextWithEntities text) {
  for (auto &ch : text.text) {
    if (ch == '\n' || ch == '\r') {
      ch = ' ';
    }
  }
  return text;
}

}  // namespace

ChatList::ChatList(not_null<Main::Session *> session, Data::Folder *folder, not_null<MainWindow *> window,
                   Data::Forum *forum)
    : _session(session), _folder(folder), _window(window), _forum(forum), _searchTimer([=] { searchGlobal(_query); }) {
  buildPage();
  refreshRows();

  auto &data = _session->data();
  if (_forum) {
    _forum->chatsListChanges() | rpl::on_next([=] { scheduleRefresh(); }, _lifetime);
    _forum->chatsListLoadedEvents() | rpl::on_next([=] { scheduleRefresh(); }, _lifetime);
    using TopicFlag = Data::TopicUpdate::Flag;
    _session->changes().topicUpdates(TopicFlag::Title | TopicFlag::Notifications | TopicFlag::Closed
                                     | TopicFlag::UnreadView | TopicFlag::UnreadMentions | TopicFlag::CloudDraft)
        | rpl::on_next([=](const Data::TopicUpdate &update) { updateEntry(update.topic); }, _lifetime);
  } else {
    data.chatsListChanges() | rpl::filter([=](Data::Folder *f) { return f == _folder; }) | rpl::on_next([=] {
      scheduleRefresh();
    }, _lifetime);
    data.chatsListLoadedEvents() | rpl::filter([=](Data::Folder *f) { return f == _folder; }) | rpl::on_next([=] {
      scheduleRefresh();
    }, _lifetime);
  }
  _session->changes().entryUpdates(Data::EntryUpdate::Flag::Repaint | Data::EntryUpdate::Flag::LocalDraftSet)
      | rpl::on_next([=](const Data::EntryUpdate &update) { updateEntry(update.entry); }, _lifetime);
  _session->changes().entryUpdates(Data::EntryUpdate::Flag::Destroyed) | rpl::on_next([=](const Data::EntryUpdate &update) {
    // A deleted topic goes at once: its row must not outlive it until the next refresh.
    removeEntry(update.entry);
    scheduleRefresh();
  }, _lifetime);
  using HistoryFlag = Data::HistoryUpdate::Flag;
  _session->changes().historyUpdates(HistoryFlag::IsPinned | HistoryFlag::UnreadView | HistoryFlag::Folder
                                     | HistoryFlag::UnreadMentions | HistoryFlag::OutboxRead | HistoryFlag::CloudDraft)
      | rpl::on_next([=](const Data::HistoryUpdate &update) {
          updateEntry(update.history);
          if (update.flags & (HistoryFlag::IsPinned | HistoryFlag::Folder)) {
            scheduleRefresh();
          }
        }, _lifetime);
  using PeerFlag = Data::PeerUpdate::Flag;
  _session->changes().peerUpdates(PeerFlag::Name | PeerFlag::Notifications | PeerFlag::OnlineStatus)
      | rpl::on_next([=](const Data::PeerUpdate &update) {
          if (const auto history = _session->data().historyLoaded(update.peer)) {
            updateEntry(history);
          }
        }, _lifetime);
  data.newItemAdded() | rpl::on_next([=](not_null<HistoryItem *> item) {
    updateEntry(item->history());
    updateEntry(item->topic());
    scheduleRefresh();
  }, _lifetime);
  data.itemRemoved() | rpl::on_next([=](not_null<const HistoryItem *> item) {
    updateEntry(item->history());
    updateEntry(item->topic());
  }, _lifetime);
  data.itemDataChanges() | rpl::on_next([=](not_null<HistoryItem *> item) {
    if (item->history()->chatListMessage() == item) {
      updateEntry(item->history());
    }
    if (const auto topic = item->topic(); topic && topic->chatListMessage() == item) {
      updateEntry(topic);
    }
  }, _lifetime);
  data.dialogsRowReplacements() | rpl::on_next([=] { scheduleRefresh(); }, _lifetime);
  data.sendActionManager().animationUpdated()
      | rpl::filter([](const Data::SendActionManager::AnimationUpdate &update) { return update.textUpdated; })
      | rpl::on_next([=](const Data::SendActionManager::AnimationUpdate &update) {
          updateEntry(update.thread->owningHistory());
          updateEntry(update.thread);
        }, _lifetime);

  // "Connecting..." in the header while there is no connection.
  auto *header = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(_page), "contrail-title"));
  const auto refreshConnecting = [=] {
    const auto state = _session->account().mtp().dcstate();
    const auto connecting = !DemoActive() && (state == MTP::ConnectingState || state == MTP::DisconnectedState
                             || (state < 0 && state > -600));
    adw_window_title_set_subtitle(ADW_WINDOW_TITLE(header),
                                  connecting ? U8(tr::lng_status_connecting(tr::now)).c_str() : "");
  };
  auto *timer = new base::Timer(refreshConnecting);
  timer->callEach(1000);
  _lifetime.add([=] { delete timer; });

  if (_forum) {
    _forum->requestTopics();
    // The group stopped being a forum: there are no topics to show.
    _forum->destroyed() | rpl::on_next([=] {
      crl::on_main(_window.get(), [window = _window, forum = _forum] { window->closeForum(forum); });
    }, _lifetime);
  } else if (_folder) {
    _session->api().requestDialogs(_folder);
  } else {
    _session->api().requestMoreDialogsIfNeeded();
  }
}

not_null<Dialogs::MainList *> ChatList::list() const {
  return _forum ? _forum->topicsList() : _session->data().chatsList(_folder);
}

QString ChatList::title() const {
  return _forum ? _forum->peer()->name() : _folder ? tr::lng_archived_name(tr::now) : u"Contrail"_q;
}

ChatList::~ChatList() {
  if (_globalRequest) {
    _session->api().request(_globalRequest).cancel();
  }
  _lifetime.destroy();
  g_object_unref(_page);
}

void ChatList::buildPage() {
  auto *view = adw_toolbar_view_new();
  auto *header = adw_header_bar_new();
  auto *title = adw_window_title_new(U8(this->title()).c_str(), "");
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title);
  if (!_folder && !_forum) {
    auto *menuButton = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menuButton), "open-menu-symbolic");
    gtk_menu_button_set_primary(GTK_MENU_BUTTON(menuButton), TRUE);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(menuButton), buildMenu());
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), menuButton);
  }
  auto *searchToggle = gtk_toggle_button_new();
  gtk_button_set_icon_name(GTK_BUTTON(searchToggle), "system-search-symbolic");
  gtk_widget_set_tooltip_text(searchToggle, U8(tr::lng_dlg_filter(tr::now)).c_str());
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), searchToggle);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

  _searchEntry = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(_searchEntry), U8(tr::lng_dlg_filter(tr::now)).c_str());
  gtk_widget_set_hexpand(_searchEntry, TRUE);
  auto *clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(clamp), _searchEntry);
  _searchBar = gtk_search_bar_new();
  gtk_search_bar_set_child(GTK_SEARCH_BAR(_searchBar), clamp);
  gtk_search_bar_connect_entry(GTK_SEARCH_BAR(_searchBar), GTK_EDITABLE(_searchEntry));
  g_object_bind_property(searchToggle, "active", _searchBar, "search-mode-enabled", G_BINDING_BIDIRECTIONAL);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), _searchBar);
  Connect<void(GtkSearchEntry *)>(_searchEntry, "search-changed", [this](GtkSearchEntry *) { applySearch(); });
  Connect<void(GtkSearchEntry *)>(_searchEntry, "activate", [this](GtkSearchEntry *) {
    // Enter opens the first chat found.
    if (auto *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(_list), 0)) {
      activated(row);
    }
  });
  Connect<void(GtkSearchEntry *)>(_searchEntry, "stop-search", [=](GtkSearchEntry *) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(searchToggle), FALSE);
  });

  _list = gtk_list_box_new();
  gtk_widget_add_css_class(_list, "navigation-sidebar");
  gtk_widget_add_css_class(_list, "chat-list");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(_list), GTK_SELECTION_SINGLE);
  // Same order as Telegram Desktop: pinned chats first, then by the last message.
  gtk_list_box_set_sort_func(
      GTK_LIST_BOX(_list),
      +[](GtkListBoxRow *a, GtkListBoxRow *b, gpointer data) -> int {
        auto *self = static_cast<ChatList *>(data);
        const auto filterId = FilterId();
        auto *ea = static_cast<Dialogs::Entry *>(g_object_get_data(G_OBJECT(a), kRowKey));
        auto *eb = static_cast<Dialogs::Entry *>(g_object_get_data(G_OBJECT(b), kRowKey));
        const auto ka = ea->sortKeyInChatList(filterId), kb = eb->sortKeyInChatList(filterId);
        (void)self;
        return (ka > kb) ? -1 : (ka < kb) ? 1 : 0;
      },
      this, nullptr);
  gtk_list_box_set_filter_func(
      GTK_LIST_BOX(_list),
      +[](GtkListBoxRow *row, gpointer data) -> gboolean {
        auto *self = static_cast<ChatList *>(data);
        if (self->_query.isEmpty()) {
          return TRUE;
        }
        auto *entry = static_cast<Dialogs::Entry *>(g_object_get_data(G_OBJECT(row), kRowKey));
        const auto name = EntryName(entry).toLower();
        for (const auto &word : self->_query.toLower().split(' ', Qt::SkipEmptyParts)) {
          if (!name.contains(word)) {
            if (const auto history = entry->asHistory()) {
              if (const auto username = history->peer->username(); username.toLower().startsWith(word)) {
                continue;
              }
            }
            return FALSE;
          }
        }
        return TRUE;
      },
      this, nullptr);
  Connect<void(GtkListBox *, GtkListBoxRow *)>(_list, "row-activated", [this](GtkListBox *, GtkListBoxRow *row) {
    activated(row);
  });

  // Global search results (people, groups, channels by username).
  _globalHeader = gtk_label_new(U8(tr::lng_search_global_results(tr::now)).c_str());
  gtk_widget_add_css_class(_globalHeader, "heading");
  gtk_widget_add_css_class(_globalHeader, "dim-label");
  gtk_label_set_xalign(GTK_LABEL(_globalHeader), 0);
  gtk_widget_set_margin_start(_globalHeader, 18);
  gtk_widget_set_margin_top(_globalHeader, 12);
  gtk_widget_set_margin_bottom(_globalHeader, 6);
  gtk_widget_set_visible(_globalHeader, FALSE);
  _globalList = gtk_list_box_new();
  gtk_widget_add_css_class(_globalList, "navigation-sidebar");
  gtk_widget_set_visible(_globalList, FALSE);
  Connect<void(GtkListBox *, GtkListBoxRow *)>(_globalList, "row-activated", [this](GtkListBox *, GtkListBoxRow *row) {
    auto *peer = static_cast<PeerData *>(g_object_get_data(G_OBJECT(row), "contrail-peer"));
    if (peer) {
      _window->openPeer(peer);
    }
  });

  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(box), _list);
  gtk_box_append(GTK_BOX(box), _globalHeader);
  gtk_box_append(GTK_BOX(box), _globalList);

  _scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(_scroll), box);
  gtk_widget_set_vexpand(_scroll, TRUE);
  Connect<void(GtkScrolledWindow *, GtkPositionType)>(_scroll, "edge-reached", [this](GtkScrolledWindow *, GtkPositionType pos) {
    if (pos == GTK_POS_BOTTOM) {
      loadMore();
    }
  });

  _stack = gtk_stack_new();
  auto *loading = adw_spinner_new();
  gtk_widget_set_size_request(loading, 32, 32);
  gtk_widget_set_halign(loading, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(loading, GTK_ALIGN_CENTER);
  gtk_stack_add_named(GTK_STACK(_stack), loading, "loading");
  gtk_stack_add_named(GTK_STACK(_stack), _scroll, "list");
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), _stack);

  _page = adw_navigation_page_new(view, U8(this->title()).c_str());
  adw_navigation_page_set_tag(_page, _forum ? "forum" : _folder ? "archive" : "chats");
  g_object_ref_sink(_page);
  g_object_set_data(G_OBJECT(_page), "contrail-title", title);
}

GtkWidget *ChatList::buildMenu() {
  auto *menu = g_menu_new();
  auto *section = g_menu_new();
  g_menu_append(section, U8(tr::lng_saved_messages(tr::now)).c_str(), "contrail.saved");
  g_menu_append(section, U8(tr::lng_contacts_header(tr::now)).c_str(), "contrail.contacts");
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
  auto *section2 = g_menu_new();
  g_menu_append(section2, U8(tr::lng_menu_settings(tr::now)).c_str(), "contrail.settings");
  g_menu_append(section2, U8(tr::lng_menu_about(tr::now) + u" Contrail"_q).c_str(), "contrail.about");
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(section2));
  auto *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  g_object_unref(section);
  g_object_unref(section2);
  g_object_unref(menu);

  auto *group = g_simple_action_group_new();
  const auto add = [&](const char *name, std::function<void()> fn) {
    auto *action = g_simple_action_new(name, nullptr);
    Connect<void(GSimpleAction *, GVariant *)>(action, "activate", [fn](GSimpleAction *, GVariant *) { fn(); });
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    g_object_unref(action);
  };
  add("saved", [this] { _window->openPeer(_session->user()); });
  add("contacts", [this] { _window->openContacts(); });
  add("settings", [this] { _window->openSettings(); });
  add("about", [this] { _window->showAbout(); });
  gtk_widget_insert_action_group(popover, "contrail", G_ACTION_GROUP(group));
  g_object_unref(group);
  return popover;
}

void ChatList::scheduleRefresh() {
  if (_refreshScheduled) {
    return;
  }
  _refreshScheduled = true;
  crl::on_main(this, [=] {
    _refreshScheduled = false;
    refreshRows();
  });
}

void ChatList::refreshRows() {
  const auto list = this->list();
  std::unordered_map<Dialogs::Entry *, bool> present;
  for (const auto &row : list->indexed()->all()) {
    const auto entry = row->entry().get();
    // In the main list the archive is a row of its own; topics are in a list of their own.
    if (_forum ? !entry->asTopic() : (!entry->asHistory() && !entry->asFolder())) {
      continue;
    }
    present[entry] = true;
    if (!_rows.contains(entry)) {
      createRow(entry);
    }
  }
  for (auto i = _rows.begin(); i != _rows.end();) {
    if (!present.contains(i->first)) {
      gtk_list_box_remove(GTK_LIST_BOX(_list), i->second->row);
      i = _rows.erase(i);
    } else {
      ++i;
    }
  }
  gtk_list_box_invalidate_sort(GTK_LIST_BOX(_list));
  if (_forum && g_getenv("CONTRAIL_DEBUG")) g_message("Topics: %d", int(_rows.size()));
  const auto loaded = list->loaded() || !_rows.empty();
  gtk_stack_set_visible_child_name(GTK_STACK(_stack), loaded ? "list" : "loading");
}

ChatList::Row &ChatList::createRow(not_null<Dialogs::Entry *> entry) {
  auto row = std::make_unique<Row>();
  row->entry = entry;
  row->row = gtk_list_box_row_new();
  g_object_set_data(G_OBJECT(row->row), kRowKey, entry.get());

  auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);
  if (const auto history = entry->asHistory()) {
    row->avatar = MakeAvatar(history->peer, 48, true);
  } else if (const auto topic = entry->asTopic()) {
    // A topic has a colored letter (or "#" for General) instead of a picture.
    row->avatar = adw_avatar_new(48, U8(topic->isGeneral() ? u"#"_q : topic->title()).c_str(), TRUE);
  } else {
    row->avatar = adw_avatar_new(48, nullptr, FALSE);
    adw_avatar_set_icon_name(ADW_AVATAR(row->avatar), "folder-symbolic");
    gtk_widget_add_css_class(row->avatar, "archive-avatar");
  }
  gtk_box_append(GTK_BOX(box), row->avatar);

  auto *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(column, TRUE);
  gtk_widget_set_valign(column, GTK_ALIGN_CENTER);

  auto *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  row->name = gtk_label_new(nullptr);
  gtk_label_set_ellipsize(GTK_LABEL(row->name), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(row->name), 0);
  gtk_widget_add_css_class(row->name, "chat-name");
  gtk_box_append(GTK_BOX(top), row->name);
  row->muted = gtk_image_new_from_icon_name("notifications-disabled-symbolic");
  gtk_widget_add_css_class(row->muted, "dim-label");
  gtk_image_set_pixel_size(GTK_IMAGE(row->muted), 12);
  gtk_box_append(GTK_BOX(top), row->muted);
  auto *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(top), spacer);
  row->status = gtk_image_new();
  gtk_image_set_pixel_size(GTK_IMAGE(row->status), 14);
  gtk_widget_add_css_class(row->status, "message-status");
  gtk_box_append(GTK_BOX(top), row->status);
  row->date = gtk_label_new(nullptr);
  gtk_widget_add_css_class(row->date, "dim-label");
  gtk_widget_add_css_class(row->date, "caption");
  gtk_box_append(GTK_BOX(top), row->date);
  gtk_box_append(GTK_BOX(column), top);

  auto *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  row->preview = gtk_label_new(nullptr);
  gtk_label_set_ellipsize(GTK_LABEL(row->preview), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(row->preview), 0);
  gtk_label_set_single_line_mode(GTK_LABEL(row->preview), TRUE);
  gtk_widget_set_hexpand(row->preview, TRUE);
  gtk_widget_add_css_class(row->preview, "dim-label");
  gtk_box_append(GTK_BOX(bottom), row->preview);
  row->pinned = gtk_image_new_from_icon_name("view-pin-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(row->pinned), 14);
  gtk_widget_add_css_class(row->pinned, "dim-label");
  gtk_box_append(GTK_BOX(bottom), row->pinned);
  row->badge = gtk_label_new(nullptr);
  gtk_widget_add_css_class(row->badge, "unread-badge");
  gtk_widget_set_valign(row->badge, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(bottom), row->badge);
  gtk_box_append(GTK_BOX(column), bottom);

  gtk_box_append(GTK_BOX(box), column);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row->row), box);

  // Long press (touch) or right click: the chat's menu.
  auto *raw = row.get();
  auto *longPress = gtk_gesture_long_press_new();
  gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(longPress), TRUE);
  Connect<void(GtkGestureLongPress *, double, double)>(longPress, "pressed", [this, raw](GtkGestureLongPress *g, double x, double y) {
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    showContextMenu(*raw, x, y);
  });
  gtk_widget_add_controller(row->row, GTK_EVENT_CONTROLLER(longPress));
  auto *rightClick = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rightClick), GDK_BUTTON_SECONDARY);
  Connect<void(GtkGestureClick *, int, double, double)>(rightClick, "pressed", [this, raw](GtkGestureClick *, int, double x, double y) {
    showContextMenu(*raw, x, y);
  });
  gtk_widget_add_controller(row->row, GTK_EVENT_CONTROLLER(rightClick));

  gtk_list_box_append(GTK_LIST_BOX(_list), row->row);
  updateRow(*row);
  auto &result = *row;
  _rows.emplace(entry, std::move(row));
  return result;
}

void ChatList::updateEntry(Dialogs::Entry *entry) {
  if (!entry) {
    return;
  }
  if (const auto i = _rows.find(entry); i != _rows.end()) {
    updateRow(*i->second);
    gtk_list_box_row_changed(GTK_LIST_BOX_ROW(i->second->row));
  }
}

void ChatList::removeEntry(Dialogs::Entry *entry) {
  if (const auto i = _rows.find(entry); i != _rows.end()) {
    gtk_list_box_remove(GTK_LIST_BOX(_list), i->second->row);
    _rows.erase(i);
    if (_current == entry) {
      _current = nullptr;
    }
  }
}

void ChatList::updateRow(Row &row) {
  const auto entry = row.entry;
  gtk_label_set_text(GTK_LABEL(row.name), U8(EntryName(entry)).c_str());
  const auto thread = entry->asThread();
  const auto history = thread ? thread->owningHistory().get() : nullptr;
  gtk_widget_set_visible(row.muted, thread && thread->muted());
  if (const auto topic = entry->asTopic(); topic && row.avatar) {
    adw_avatar_set_text(ADW_AVATAR(row.avatar), U8(topic->isGeneral() ? u"#"_q : topic->title()).c_str());
  }

  const auto item = entry->chatListMessage();
  const auto time = entry->chatListTimeId();
  gtk_label_set_text(GTK_LABEL(row.date), time ? U8(FormatListDate(time)).c_str() : "");

  // Sent / read ticks for the last outgoing message.
  const char *statusIcon = nullptr;
  if (history && item && item->out() && !item->isService() && !history->peer->isSelf()) {
    if (item->hasFailed()) {
      statusIcon = "dialog-error-symbolic";
    } else if (item->isSending()) {
      statusIcon = "contrail-clock-symbolic";
    } else if (item->unread(thread)) {
      statusIcon = "contrail-check-symbolic";
    } else {
      statusIcon = "contrail-check-double-symbolic";
    }
  }
  gtk_widget_set_visible(row.status, statusIcon != nullptr);
  if (statusIcon) {
    gtk_image_set_from_icon_name(GTK_IMAGE(row.status), statusIcon);
  }

  if (const auto action = thread ? thread->sendActionPainter()->actionText() : QString(); !action.isEmpty()) {
    gtk_label_set_markup(GTK_LABEL(row.preview), ("<span color=\"#3584e4\">" + Escape(action) + "</span>").c_str());
  } else {
    gtk_label_set_markup(GTK_LABEL(row.preview), ToMarkup(OneLine(EntryPreview(entry)), false, false).c_str());
  }

  const auto badges = entry->chatListBadgesState();
  const auto pinned = entry->isPinnedDialog(FilterId());
  if (badges.unread || badges.mention) {
    auto text = badges.mention ? u"@"_q : QString();
    if (badges.unreadCounter > 0) {
      text = (text.isEmpty() ? QString() : text + ' ') + QString::number(badges.unreadCounter);
    } else if (text.isEmpty()) {
      text = u" "_q;
    }
    gtk_label_set_text(GTK_LABEL(row.badge), U8(text).c_str());
    if (badges.unreadMuted && !badges.mention) {
      gtk_widget_add_css_class(row.badge, "muted");
    } else {
      gtk_widget_remove_css_class(row.badge, "muted");
    }
    gtk_widget_set_visible(row.badge, TRUE);
    gtk_widget_set_visible(row.pinned, FALSE);
  } else {
    gtk_widget_set_visible(row.badge, FALSE);
    gtk_widget_set_visible(row.pinned, pinned);
  }
}

void ChatList::setCurrent(Data::Thread *thread) {
  // The main list marks the group of an open topic.
  if (thread && !_forum && thread->asTopic()) {
    thread = thread->owningHistory();
  }
  _current = thread;
  if (thread) {
    if (const auto i = _rows.find(thread); i != _rows.end()) {
      gtk_list_box_select_row(GTK_LIST_BOX(_list), GTK_LIST_BOX_ROW(i->second->row));
      return;
    }
  }
  gtk_list_box_unselect_all(GTK_LIST_BOX(_list));
}

void ChatList::activated(GtkListBoxRow *row) {
  auto *entry = static_cast<Dialogs::Entry *>(g_object_get_data(G_OBJECT(row), kRowKey));
  if (!entry) {
    return;
  }
  if (entry->asFolder()) {
    _window->openArchive();
  } else if (const auto topic = entry->asTopic()) {
    _window->openThread(topic);
  } else if (const auto history = entry->asHistory()) {
    _window->openHistory(history);
  }
}

void ChatList::openRow(int index) {
  if (auto *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(_list), index)) {
    activated(row);
  }
}

void ChatList::showContextMenu(Row &row, double x, double y) {
  const auto thread = row.entry->asThread();
  const auto topic = row.entry->asTopic();
  const auto history = row.entry->asHistory();
  if (!thread) {
    return;
  }
  const auto peer = thread->peer();
  auto *menu = g_menu_new();
  if (history) {
    const auto pinned = history->isPinnedDialog(FilterId());
    g_menu_append(menu, U8(pinned ? tr::lng_context_unpin_from_top(tr::now) : tr::lng_context_pin_to_top(tr::now)).c_str(),
                  "row.pin");
  }
  if (!peer->isSelf()) {
    g_menu_append(menu, U8(thread->muted() ? tr::lng_context_unmute(tr::now) : tr::lng_context_mute(tr::now)).c_str(),
                  "row.mute");
  }
  if (thread->chatListBadgesState().unread) {
    g_menu_append(menu, U8(tr::lng_context_mark_read(tr::now)).c_str(), "row.read");
  }
  if (topic) {
    // Pinning, archiving and leaving are for the whole group.
  } else if (!peer->isSelf() && !peer->isRepliesChat()) {
    const auto archived = history->folder() != nullptr;
    g_menu_append(menu, U8(archived ? tr::lng_archived_remove(tr::now) : tr::lng_archived_add(tr::now)).c_str(),
                  "row.archive");
  }
  if (history) {
    g_menu_append(menu,
                  U8(peer->isChannel() ? (peer->isMegagroup() ? tr::lng_profile_leave_group(tr::now)
                                                              : tr::lng_profile_leave_channel(tr::now))
                                       : tr::lng_profile_delete_conversation(tr::now))
                      .c_str(),
                  "row.delete");
  }

  auto *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  g_object_unref(menu);
  gtk_widget_set_parent(popover, row.row);
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
  const auto session = _session;
  add("pin", [=] {
    const auto owner = &history->owner();
    const auto pin = !history->isPinnedDialog(FilterId());
    owner->setChatPinned(history, FilterId(), pin);
    session->api()
        .request(MTPmessages_ToggleDialogPin(MTP_flags(pin ? MTPmessages_ToggleDialogPin::Flag::f_pinned
                                                           : MTPmessages_ToggleDialogPin::Flag(0)),
                                             MTP_inputDialogPeer(history->peer->input())))
        .done([=] { owner->notifyPinnedDialogsOrderUpdated(); })
        .fail([=](const MTP::Error &error) {
          owner->setChatPinned(history, FilterId(), !pin);
          ShowToast(error.type());
        })
        .send();
  });
  add("mute", [=] {
    auto value = Data::MuteValue();
    if (thread->muted()) {
      value.unmute = true;
    } else {
      value.forever = true;
    }
    session->data().notifySettings().update(thread, value);
  });
  add("read", [=] {
    if (topic) {
      topic->readTillEnd();
    } else {
      session->data().histories().readInbox(history);
    }
  });
  add("archive", [=] {
    const auto archive = (history->folder() == nullptr);
    session->api().toggleHistoryArchived(history, archive, [=] {
      ShowToast(archive ? tr::lng_archived_added(tr::now) : tr::lng_archived_removed(tr::now));
    });
  });
  add("delete", [=] { _window->confirmDeleteChat(peer); });
  gtk_widget_insert_action_group(popover, "row", G_ACTION_GROUP(group));
  g_object_unref(group);
  gtk_popover_popup(GTK_POPOVER(popover));
}

void ChatList::loadMore() {
  if (_forum) {
    _forum->requestTopics();
  } else if (_folder) {
    _session->api().requestDialogs(_folder);
  } else {
    _session->api().requestMoreBlockedByDateDialogs();
  }
}

void ChatList::startSearch(const QString &query) {
  gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(_searchBar), TRUE);
  if (!query.isEmpty()) {
    gtk_editable_set_text(GTK_EDITABLE(_searchEntry), U8(query).c_str());
  }
  gtk_widget_grab_focus(_searchEntry);
}

void ChatList::applySearch() {
  _query = Q(gtk_editable_get_text(GTK_EDITABLE(_searchEntry))).trimmed();
  gtk_list_box_invalidate_filter(GTK_LIST_BOX(_list));
  ClearBox(_globalList);
  gtk_widget_set_visible(_globalHeader, FALSE);
  gtk_widget_set_visible(_globalList, FALSE);
  if (_globalRequest) {
    _session->api().request(base::take(_globalRequest)).cancel();
  }
  if (_query.size() >= 3 && !_folder && !_forum) {
    _searchTimer.callOnce(400);
  } else {
    _searchTimer.cancel();
  }
}

void ChatList::searchGlobal(const QString &query) {
  _globalRequest = _session->api()
                       .request(MTPcontacts_Search(MTP_flags(0), MTP_string(query), MTP_int(30)))
                       .done([=](const MTPcontacts_Found &result) {
                         _globalRequest = 0;
                         if (query != _query) {
                           return;
                         }
                         const auto &data = result.data();
                         _session->data().processUsers(data.vusers());
                         _session->data().processChats(data.vchats());
                         auto count = 0;
                         const auto add = [&](const MTPPeer &mtpPeer) {
                           const auto peer = _session->data().peerLoaded(peerFromMTP(mtpPeer));
                           if (!peer) {
                             return;
                           }
                           // Chats already in the list are shown there.
                           if (const auto history = _session->data().historyLoaded(peer)) {
                             if (_rows.contains(history)) {
                               return;
                             }
                           }
                           auto *row = gtk_list_box_row_new();
                           g_object_set_data(G_OBJECT(row), "contrail-peer", peer);
                           auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
                           gtk_widget_set_margin_top(box, 6);
                           gtk_widget_set_margin_bottom(box, 6);
                           gtk_box_append(GTK_BOX(box), MakeAvatar(peer, 40));
                           auto *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
                           gtk_widget_set_valign(column, GTK_ALIGN_CENTER);
                           auto *name = gtk_label_new(U8(peer->name()).c_str());
                           gtk_label_set_xalign(GTK_LABEL(name), 0);
                           gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
                           gtk_widget_add_css_class(name, "chat-name");
                           gtk_box_append(GTK_BOX(column), name);
                           if (const auto username = peer->username(); !username.isEmpty()) {
                             auto *sub = gtk_label_new(("@" + U8(username)).c_str());
                             gtk_label_set_xalign(GTK_LABEL(sub), 0);
                             gtk_widget_add_css_class(sub, "dim-label");
                             gtk_widget_add_css_class(sub, "caption");
                             gtk_box_append(GTK_BOX(column), sub);
                           }
                           gtk_box_append(GTK_BOX(box), column);
                           gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
                           gtk_list_box_append(GTK_LIST_BOX(_globalList), row);
                           ++count;
                         };
                         for (const auto &peer : data.vmy_results().v) {
                           add(peer);
                         }
                         for (const auto &peer : data.vresults().v) {
                           add(peer);
                         }
                         gtk_widget_set_visible(_globalHeader, count > 0);
                         gtk_widget_set_visible(_globalList, count > 0);
                       })
                       .fail([=] { _globalRequest = 0; })
                       .send();
}

}  // namespace Contrail
