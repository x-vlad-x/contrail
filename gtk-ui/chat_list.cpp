#include "chat_list.hpp"

#include "main_window.hpp"
#include "td/chat_row.hpp"
#include "td/dates.hpp"
#include "td/strings.hpp"
#include "userpics.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <utility>

// An item of the list's model: which chat a row shows. The widgets come and go, this stays for every chat.
G_DECLARE_FINAL_TYPE(ContrailListEntry, contrail_list_entry, CONTRAIL, LIST_ENTRY, GObject)

struct _ContrailListEntry {
  GObject parent_instance;
  int kind;
  gint64 id;
};

G_DEFINE_FINAL_TYPE(ContrailListEntry, contrail_list_entry, G_TYPE_OBJECT)

static void contrail_list_entry_class_init(ContrailListEntryClass *) {}
static void contrail_list_entry_init(ContrailListEntry *) {}

namespace Contrail {
namespace {

namespace api = Td::api;

constexpr auto kRowKey = "contrail-row";
constexpr auto kAvatarSize = 48;
constexpr auto kFoundAvatarSize = 40;
constexpr auto kArchiveNames = 4;
constexpr auto kSearchDelayMs = 400;
constexpr auto kGlobalSearchMinLength = 3;
constexpr auto kGlobalSearchLimit = 30;

std::string Trimmed(const char *text) {
  auto *copy = g_strstrip(g_strdup(text));
  auto result = std::string(copy);
  g_free(copy);
  return result;
}

// What comes first in a preview: "Draft:", "You:".
std::string Lead(const std::string &text) {
  return "<span weight=\"600\">" + Escape(text + ": ") + "</span>";
}

api::object_ptr<api::ChatList> MakeList(Td::ListKind kind) {
  if (kind == Td::ListKind::Main) return api::make_object<api::chatListMain>();
  return api::make_object<api::chatListArchive>();
}

}  // namespace

ChatList::ChatList(Td::Store &store, Td::ListKind kind, MainWindow &window)
    : _store(store), _kind(kind), _window(window) {
  buildPage();
  refresh();

  _store.onList([this](Td::ListKind list) {
    if (list == Td::ListKind::Archive && _archiveRow) updateRow(*_archiveRow);
    if (list == _kind || _kind == Td::ListKind::Main) scheduleRefresh();
  }, _lifetime);
  _store.onChat([this](Td::ChatId id) { chatChanged(id); }, _lifetime);
  _store.onUser([this](Td::UserId id) { userChanged(id); }, _lifetime);
  _store.onConnection([this] { connectionChanged(); }, _lifetime);
  connectionChanged();

  // The archive has a row of its own in the main list: its first chats say whether to show it and what to write there.
  if (_kind == Td::ListKind::Main) _store.loadMore(Td::ListKind::Archive);
}

ChatList::~ChatList() {
  if (_searchTimer) g_source_remove(_searchTimer);
  if (_refreshSource) g_source_remove(_refreshSource);
  if (_loadSource) g_source_remove(_loadSource);
  // The page can outlive this object in a navigation animation: its rows go now, while unbinding still works.
  gtk_list_view_set_model(GTK_LIST_VIEW(_list), nullptr);
  gtk_list_view_set_factory(GTK_LIST_VIEW(_list), nullptr);
  _connections.clear();
  g_object_unref(_page);
}

void ChatList::buildPage() {
  auto *view = adw_toolbar_view_new();
  auto *header = adw_header_bar_new();
  const auto archive = _kind == Td::ListKind::Archive;
  const auto titleText = archive ? Tr("lng_archived_name") : std::string("Contrail");
  _title = adw_window_title_new(titleText.c_str(), "");
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), _title);
  if (!archive) {
    auto *menuButton = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menuButton), "open-menu-symbolic");
    gtk_menu_button_set_primary(GTK_MENU_BUTTON(menuButton), TRUE);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(menuButton), buildMenu());
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), menuButton);
  }
  auto *searchToggle = gtk_toggle_button_new();
  gtk_button_set_icon_name(GTK_BUTTON(searchToggle), "system-search-symbolic");
  gtk_widget_set_tooltip_text(searchToggle, Tr("lng_dlg_filter").c_str());
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), searchToggle);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

  _searchEntry = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(_searchEntry), Tr("lng_dlg_filter").c_str());
  gtk_widget_set_hexpand(_searchEntry, TRUE);
  auto *clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(clamp), _searchEntry);
  _searchBar = gtk_search_bar_new();
  gtk_search_bar_set_child(GTK_SEARCH_BAR(_searchBar), clamp);
  gtk_search_bar_connect_entry(GTK_SEARCH_BAR(_searchBar), GTK_EDITABLE(_searchEntry));
  g_object_bind_property(searchToggle, "active", _searchBar, "search-mode-enabled",
                         GBindingFlags(G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE));
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), _searchBar);
  _connections.connect<void(GtkSearchEntry *)>(_searchEntry, "search-changed", [this](GtkSearchEntry *) {
    applySearch();
  });
  _connections.connect<void(GtkSearchEntry *)>(_searchEntry, "activate", [this](GtkSearchEntry *) {
    // Enter opens the first chat found.
    openRow(0);
  });
  _connections.connect<void(GtkSearchEntry *)>(_searchEntry, "stop-search", [searchToggle](GtkSearchEntry *) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(searchToggle), FALSE);
  });

  _model = g_list_store_new(contrail_list_entry_get_type());
  _selection = gtk_single_selection_new(G_LIST_MODEL(_model));
  // Selected means open here: a tap doesn't select, and nothing is open at first.
  gtk_single_selection_set_autoselect(_selection, FALSE);
  gtk_single_selection_set_can_unselect(_selection, TRUE);
  auto *factory = gtk_signal_list_item_factory_new();
  _connections.connect<void(GtkSignalListItemFactory *, GObject *)>(
      factory, "setup", [this](GtkSignalListItemFactory *, GObject *item) { setupRow(GTK_LIST_ITEM(item)); });
  _connections.connect<void(GtkSignalListItemFactory *, GObject *)>(
      factory, "bind", [this](GtkSignalListItemFactory *, GObject *item) { bindRow(GTK_LIST_ITEM(item)); });
  _connections.connect<void(GtkSignalListItemFactory *, GObject *)>(
      factory, "unbind", [this](GtkSignalListItemFactory *, GObject *item) { unbindRow(GTK_LIST_ITEM(item)); });
  _list = gtk_list_view_new(GTK_SELECTION_MODEL(_selection), factory);
  gtk_widget_add_css_class(_list, "navigation-sidebar");
  gtk_widget_add_css_class(_list, "chat-list");
  gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(_list), TRUE);
  _connections.connect<void(GtkListView *, guint)>(_list, "activate", [this](GtkListView *, guint position) {
    if (position < _shown.size()) activate(_shown[position]);
  });

  _scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(_scroll), _list);
  gtk_widget_set_vexpand(_scroll, TRUE);
  auto *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(_scroll));
  // Before the end comes into view, and also when a short list doesn't fill the window and can't be scrolled.
  _connections.connect<void(GtkAdjustment *)>(adjustment, "value-changed", [this](GtkAdjustment *) { loadMoreIfNear(); });
  _connections.connect<void(GtkAdjustment *)>(adjustment, "changed", [this](GtkAdjustment *) { loadMoreIfNear(); });

  _stack = gtk_stack_new();
  auto *loading = adw_spinner_new();
  gtk_widget_set_size_request(loading, 32, 32);
  gtk_widget_set_halign(loading, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(loading, GTK_ALIGN_CENTER);
  gtk_stack_add_named(GTK_STACK(_stack), loading, "loading");
  gtk_stack_add_named(GTK_STACK(_stack), _scroll, "list");
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), _stack);

  _page = ADW_NAVIGATION_PAGE(adw_navigation_page_new(view, titleText.c_str()));
  adw_navigation_page_set_tag(_page, archive ? "archive" : "chats");
  g_object_ref_sink(_page);
}

GtkWidget *ChatList::buildMenu() {
  auto *menu = g_menu_new();
  auto *section = g_menu_new();
  g_menu_append(section, Tr("lng_saved_messages").c_str(), "contrail.saved");
  g_menu_append(section, Tr("lng_contacts_header").c_str(), "contrail.contacts");
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
  auto *section2 = g_menu_new();
  g_menu_append(section2, Tr("lng_menu_settings").c_str(), "contrail.settings");
  g_menu_append(section2, (Tr("lng_menu_about") + " Contrail").c_str(), "contrail.about");
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(section2));
  auto *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  g_object_unref(section);
  g_object_unref(section2);
  g_object_unref(menu);

  auto *group = g_simple_action_group_new();
  auto *saved = g_simple_action_new("saved", nullptr);
  _connections.connect<void(GSimpleAction *, GVariant *)>(saved, "activate", [this](GSimpleAction *, GVariant *) {
    openSavedMessages();
  });
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(saved));
  g_object_unref(saved);
  auto *contacts = g_simple_action_new("contacts", nullptr);
  _connections.connect<void(GSimpleAction *, GVariant *)>(contacts, "activate", [this](GSimpleAction *, GVariant *) {
    _window.openContacts();
  });
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(contacts));
  g_object_unref(contacts);
  auto *about = g_simple_action_new("about", nullptr);
  _connections.connect<void(GSimpleAction *, GVariant *)>(about, "activate", [this](GSimpleAction *, GVariant *) {
    _window.showAbout();
  });
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(about));
  g_object_unref(about);
  // Its page comes with a later stage. Greyed out rather than hidden: the menu keeps its shape.
  for (const auto *name : {"settings"}) {
    auto *action = g_simple_action_new(name, nullptr);
    g_simple_action_set_enabled(action, FALSE);
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    g_object_unref(action);
  }
  gtk_widget_insert_action_group(popover, "contrail", G_ACTION_GROUP(group));
  g_object_unref(group);
  return popover;
}

void ChatList::setupRow(GtkListItem *item) {
  auto owned = std::make_unique<Row>();
  auto *row = owned.get();
  // Not selectable by the user, so that hovering a row with single-click activation doesn't select it.
  gtk_list_item_set_selectable(item, FALSE);

  row->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  row->header = gtk_label_new(Tr("lng_search_global_results").c_str());
  gtk_widget_add_css_class(row->header, "heading");
  gtk_widget_add_css_class(row->header, "dim-label");
  gtk_label_set_xalign(GTK_LABEL(row->header), 0);
  gtk_widget_set_margin_start(row->header, 6);
  gtk_widget_set_margin_top(row->header, 12);
  gtk_widget_set_margin_bottom(row->header, 6);
  gtk_box_append(GTK_BOX(row->root), row->header);

  row->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(row->box, 6);
  gtk_widget_set_margin_bottom(row->box, 6);
  row->avatar = adw_avatar_new(kAvatarSize, nullptr, TRUE);
  gtk_box_append(GTK_BOX(row->box), row->avatar);

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

  gtk_box_append(GTK_BOX(row->box), column);
  gtk_box_append(GTK_BOX(row->root), row->box);

  // Long press (touch) or right click: the chat's menu. The rows go before this object does, see the destructor.
  const auto menu = [this, row](double x, double y) {
    if (row->entry.kind == RowKind::Chat) showContextMenu(row->entry.id, row->root, x, y);
  };
  auto *longPress = gtk_gesture_long_press_new();
  gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(longPress), TRUE);
  Connect<void(GtkGestureLongPress *, double, double)>(longPress, "pressed", [menu](GtkGestureLongPress *gesture, double x, double y) {
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    menu(x, y);
  });
  gtk_widget_add_controller(row->root, GTK_EVENT_CONTROLLER(longPress));
  auto *rightClick = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rightClick), GDK_BUTTON_SECONDARY);
  Connect<void(GtkGestureClick *, int, double, double)>(rightClick, "pressed", [menu](GtkGestureClick *, int, double x, double y) {
    menu(x, y);
  });
  gtk_widget_add_controller(row->root, GTK_EVENT_CONTROLLER(rightClick));

  gtk_list_item_set_child(item, row->root);
  AttachData(item, kRowKey, std::move(owned));
}

void ChatList::bindRow(GtkListItem *item) {
  auto *row = static_cast<Row *>(g_object_get_data(G_OBJECT(item), kRowKey));
  auto *entry = CONTRAIL_LIST_ENTRY(gtk_list_item_get_item(item));
  row->entry = {RowKind(entry->kind), entry->id};
  gtk_list_item_set_activatable(item, row->entry.kind != RowKind::Header);
  if (row->entry.kind == RowKind::Archive) {
    _archiveRow = row;
  } else if (row->entry.kind != RowKind::Header) {
    _bound[row->entry.id] = row;
  }
  updateRow(*row);
}

void ChatList::unbindRow(GtkListItem *item) {
  auto *row = static_cast<Row *>(g_object_get_data(G_OBJECT(item), kRowKey));
  // In one change a chat can get a new row before its old one lets go.
  if (const auto it = _bound.find(row->entry.id); it != _bound.end() && it->second == row) _bound.erase(it);
  if (_archiveRow == row) _archiveRow = nullptr;
  UnbindAvatar(ADW_AVATAR(row->avatar));
}

void ChatList::updateRow(Row &row) {
  const auto header = row.entry.kind == RowKind::Header;
  gtk_widget_set_visible(row.header, header);
  gtk_widget_set_visible(row.box, !header);
  switch (row.entry.kind) {
    case RowKind::Chat: updateChatRow(row); break;
    case RowKind::Archive: updateArchiveRow(row); break;
    case RowKind::Found: updateFoundRow(row); break;
    case RowKind::Header: break;
  }
}

void ChatList::setPreview(Row &row, const std::string &markup) {
  // Statuses and typing reach every row on the screen: a label not touched stays laid out.
  if (row.previewMarkup == markup) return;
  row.previewMarkup = markup;
  gtk_label_set_markup(GTK_LABEL(row.preview), markup.c_str());
}

void ChatList::setBadge(Row &row, bool visible, const std::string &text, bool muted) {
  gtk_widget_set_visible(row.badge, visible);
  if (!visible) return;
  gtk_label_set_text(GTK_LABEL(row.badge), text.c_str());
  if (muted) {
    gtk_widget_add_css_class(row.badge, "muted");
  } else {
    gtk_widget_remove_css_class(row.badge, "muted");
  }
}

std::string ChatList::chatName(const Td::Chat &chat) const {
  if (isSelf(chat)) return Tr("lng_saved_messages");
  if (chat.id == _store.repliesChatId()) return Tr("lng_replies_messages");
  return chat.title;
}

bool ChatList::isSelf(const Td::Chat &chat) const {
  return chat.kind == Td::ChatKind::Private && chat.peerId == _store.selfId();
}

void ChatList::updateChatRow(Row &row) {
  const auto chat = _store.chat(row.entry.id);
  if (!chat) return;
  gtk_widget_remove_css_class(row.avatar, "archive-avatar");
  adw_avatar_set_size(ADW_AVATAR(row.avatar), kAvatarSize);
  BindAvatar(ADW_AVATAR(row.avatar), _store, chat->id, true);
  gtk_label_set_text(GTK_LABEL(row.name), chatName(*chat).c_str());
  gtk_widget_set_visible(row.muted, chat->muted);

  gtk_widget_set_visible(row.date, true);
  gtk_label_set_text(GTK_LABEL(row.date), chat->lastDate ? Td::FormatListDate(chat->lastDate).c_str() : "");

  // Sent / read ticks for the last outgoing message.
  const auto icon = Td::ChatStatusIcon(*chat, _store.selfId());
  gtk_widget_set_visible(row.status, icon != nullptr);
  if (icon) gtk_image_set_from_icon_name(GTK_IMAGE(row.status), icon);

  gtk_widget_set_visible(row.preview, true);
  gtk_widget_remove_css_class(row.preview, "caption");
  if (const auto typing = Td::TypingText(*chat, _store); !typing.empty()) {
    setPreview(row, "<span color=\"#3584e4\">" + Escape(typing) + "</span>");
  } else if (!chat->draft.empty()) {
    setPreview(row, Lead(Tr("lng_from_draft")) + Escape(Td::OneLine(chat->draft)));
  } else if (const auto sender = Td::PreviewSender(*chat, _store); !sender.empty()) {
    setPreview(row, Lead(sender) + Escape(chat->preview));
  } else {
    setPreview(row, Escape(chat->preview));
  }

  const auto badge = Td::ChatBadge(*chat);
  const auto &position = _kind == Td::ListKind::Main ? chat->main : chat->archive;
  setBadge(row, badge.visible, badge.text, badge.muted);
  gtk_widget_set_visible(row.pinned, !badge.visible && position.pinned);
}

void ChatList::updateArchiveRow(Row &row) {
  UnbindAvatar(ADW_AVATAR(row.avatar));
  adw_avatar_set_size(ADW_AVATAR(row.avatar), kAvatarSize);
  adw_avatar_set_custom_image(ADW_AVATAR(row.avatar), nullptr);
  adw_avatar_set_text(ADW_AVATAR(row.avatar), "");
  adw_avatar_set_show_initials(ADW_AVATAR(row.avatar), FALSE);
  adw_avatar_set_icon_name(ADW_AVATAR(row.avatar), "folder-symbolic");
  gtk_widget_remove_css_class(row.avatar, "saved-messages");
  gtk_widget_add_css_class(row.avatar, "archive-avatar");
  gtk_label_set_text(GTK_LABEL(row.name), Tr("lng_archived_name").c_str());
  gtk_widget_set_visible(row.muted, false);
  gtk_widget_set_visible(row.status, false);
  gtk_widget_set_visible(row.pinned, false);

  // The names of the chats on top of it, and the time of the newest.
  auto names = std::string();
  auto count = 0;
  auto date = 0;
  for (const auto id : _store.list(Td::ListKind::Archive)) {
    const auto chat = _store.chat(id);
    if (!chat) continue;
    date = std::max(date, chat->lastDate);
    if (count < kArchiveNames) {
      names += (count ? ", " : "") + chatName(*chat);
      ++count;
    } else if (!chat->archive.pinned) {
      break;  // Past the pinned chats the newest has been seen, and an archive can be long.
    }
  }
  gtk_widget_set_visible(row.date, true);
  gtk_label_set_text(GTK_LABEL(row.date), date ? Td::FormatListDate(date).c_str() : "");
  gtk_widget_set_visible(row.preview, true);
  gtk_widget_remove_css_class(row.preview, "caption");
  setPreview(row, Escape(names));
  const auto unread = _store.unreadChats(Td::ListKind::Archive);
  setBadge(row, unread > 0, std::to_string(unread), true);
}

void ChatList::updateFoundRow(Row &row) {
  const auto chat = _store.chat(row.entry.id);
  if (!chat) return;
  gtk_widget_remove_css_class(row.avatar, "archive-avatar");
  adw_avatar_set_size(ADW_AVATAR(row.avatar), kFoundAvatarSize);
  BindAvatar(ADW_AVATAR(row.avatar), _store, chat->id);
  gtk_label_set_text(GTK_LABEL(row.name), chat->title.c_str());
  gtk_widget_set_visible(row.muted, false);
  gtk_widget_set_visible(row.status, false);
  gtk_widget_set_visible(row.date, false);
  gtk_widget_set_visible(row.pinned, false);
  gtk_widget_set_visible(row.badge, false);
  const auto username = _store.username(*chat);
  gtk_widget_set_visible(row.preview, !username.empty());
  gtk_widget_add_css_class(row.preview, "caption");
  setPreview(row, Escape("@" + username));
}

void ChatList::chatChanged(Td::ChatId id) {
  if (const auto it = _bound.find(id); it != _bound.end()) updateRow(*it->second);
  if (_archiveRow) {
    const auto chat = _store.chat(id);
    if (chat && chat->archive.order) updateRow(*_archiveRow);
  }
  // A new title can bring a chat into the results or take it out.
  if (!_query.empty()) scheduleRefresh();
}

void ChatList::userChanged(Td::UserId id) {
  // A row shows a user's name in the preview or in the title; the list on the screen is short.
  for (const auto &[chatId, row] : _bound) {
    const auto chat = _store.chat(chatId);
    if (!chat) continue;
    if ((chat->kind == Td::ChatKind::Private && chat->peerId == id) || chat->lastSenderUser == id) updateRow(*row);
  }
}

void ChatList::connectionChanged() {
  const auto state = _store.connection();
  const auto id = state ? state->get_id() : 0;
  const auto connecting = id == api::connectionStateWaitingForNetwork::ID || id == api::connectionStateConnecting::ID
      || id == api::connectionStateConnectingToProxy::ID;
  adw_window_title_set_subtitle(ADW_WINDOW_TITLE(_title), connecting ? Tr("lng_status_connecting").c_str() : "");
}

std::vector<ChatList::Entry> ChatList::wantedEntries() {
  auto result = std::vector<Entry>();
  const auto &ids = _store.list(_kind);
  result.reserve(ids.size() + _found.size() + 2);
  const auto matches = [&](const std::string &name, const std::string &username) {
    return _words.empty() || Td::MatchesSearch(name, username, _words);
  };
  // TDLib doesn't say where the archive stands among the chats: it goes on top, where the official apps show it.
  if (_kind == Td::ListKind::Main && !_store.list(Td::ListKind::Archive).empty()
      && matches(Tr("lng_archived_name"), {})) {
    result.push_back({RowKind::Archive, 0});
  }
  for (const auto id : ids) {
    const auto chat = _store.chat(id);
    if (chat && matches(chatName(*chat), _store.username(*chat))) result.push_back({RowKind::Chat, id});
  }
  // Chats already shown above aren't repeated.
  auto found = std::vector<Entry>();
  for (const auto id : _found) {
    const auto entry = Entry{RowKind::Found, id};
    if (std::ranges::find(result, Entry{RowKind::Chat, id}) == result.end()
        && std::ranges::find(found, entry) == found.end()) {
      found.push_back(entry);
    }
  }
  if (!found.empty()) {
    result.push_back({RowKind::Header, 0});
    result.insert(result.end(), found.begin(), found.end());
  }
  return result;
}

void ChatList::scheduleRefresh() {
  if (_refreshSource) return;
  // Updates come in bursts, and a change of the model must not happen in the middle of a layout.
  _refreshSource = g_idle_add_once(+[](gpointer data) {
    auto *self = static_cast<ChatList *>(data);
    self->_refreshSource = 0;
    self->refresh();
  }, this);
}

void ChatList::refresh() {
  auto wanted = wantedEntries();
  const auto change = Td::DiffLists(_shown, wanted);
  if (change.removed || change.added) {
    auto *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(_scroll));
    const auto atTop = gtk_adjustment_get_value(adjustment) <= 0.;
    // Items that only moved stay the same objects: their rows keep their widgets and aren't drawn anew.
    auto reused = std::map<std::pair<RowKind, Td::ChatId>, gpointer>();
    for (auto i = std::size_t(0); i < change.removed; ++i) {
      const auto &entry = _shown[change.first + i];
      reused[{entry.kind, entry.id}] = g_list_model_get_item(G_LIST_MODEL(_model), guint(change.first + i));
    }
    auto added = std::vector<gpointer>();
    added.reserve(change.added);
    for (auto i = std::size_t(0); i < change.added; ++i) {
      const auto &entry = wanted[change.first + i];
      if (const auto it = reused.find({entry.kind, entry.id}); it != reused.end()) {
        added.push_back(it->second);
        reused.erase(it);
      } else {
        auto *item = CONTRAIL_LIST_ENTRY(g_object_new(contrail_list_entry_get_type(), nullptr));
        item->kind = int(entry.kind);
        item->id = entry.id;
        added.push_back(item);
      }
    }
    _shown = std::move(wanted);
    g_list_store_splice(_model, guint(change.first), guint(change.removed), added.data(), guint(added.size()));
    std::ranges::for_each(added, g_object_unref);
    for (const auto &[key, item] : reused) g_object_unref(item);
    // The view follows the row at its top. A list at its top stays there: a new message must come into view.
    if (atTop && !_shown.empty()) gtk_list_view_scroll_to(GTK_LIST_VIEW(_list), 0, GTK_LIST_SCROLL_NONE, nullptr);
  }
  applySelection();
  const auto ready = _store.listComplete(_kind) || !_store.list(_kind).empty();
  gtk_stack_set_visible_child_name(GTK_STACK(_stack), ready ? "list" : "loading");
}

void ChatList::applySelection() {
  auto position = guint(GTK_INVALID_LIST_POSITION);
  if (_current) {
    const auto found = std::ranges::find_if(_shown, [&](const Entry &entry) {
      return entry.id == _current && (entry.kind == RowKind::Chat || entry.kind == RowKind::Found);
    });
    if (found != _shown.end()) position = guint(found - _shown.begin());
  }
  gtk_single_selection_set_selected(_selection, position);
}

void ChatList::setCurrent(Td::ChatId chat) {
  _current = chat;
  applySelection();
}

void ChatList::activate(const Entry &entry) {
  switch (entry.kind) {
    case RowKind::Archive: _window.openArchive(); break;
    case RowKind::Chat:
    case RowKind::Found: _window.openChat(entry.id); break;
    case RowKind::Header: break;
  }
}

void ChatList::openRow(int index) {
  if (index >= 0 && std::size_t(index) < _shown.size()) activate(_shown[index]);
}

void ChatList::loadMoreIfNear() {
  // A filtered list is short whatever the account has.
  if (_loadSource || !_query.empty() || _store.listComplete(_kind)) return;
  auto *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(_scroll));
  const auto page = gtk_adjustment_get_page_size(adjustment);
  if (page <= 0. || gtk_adjustment_get_value(adjustment) + 3 * page < gtk_adjustment_get_upper(adjustment)) return;
  // This can run in the middle of a layout.
  _loadSource = g_idle_add_once(+[](gpointer data) {
    auto *self = static_cast<ChatList *>(data);
    self->_loadSource = 0;
    self->_store.loadMore(self->_kind);
  }, this);
}

void ChatList::showContextMenu(Td::ChatId id, GtkWidget *parent, double x, double y) {
  const auto chat = _store.chat(id);
  if (!chat) return;
  const auto self = isSelf(*chat);
  const auto &position = _kind == Td::ListKind::Main ? chat->main : chat->archive;
  auto *menu = g_menu_new();
  g_menu_append(menu, Tr(position.pinned ? "lng_context_unpin_from_top" : "lng_context_pin_to_top").c_str(), "row.pin");
  if (!self) g_menu_append(menu, Tr(chat->muted ? "lng_context_unmute" : "lng_context_mute").c_str(), "row.mute");
  if (chat->unread > 0 || chat->markedUnread) g_menu_append(menu, Tr("lng_context_mark_read").c_str(), "row.read");
  if (!self && chat->id != _store.repliesChatId()) {
    g_menu_append(menu, Tr(chat->archive.order ? "lng_archived_remove" : "lng_archived_add").c_str(), "row.archive");
  }
  const auto leave = !chat->supergroup ? "lng_profile_delete_conversation"
      : chat->kind == Td::ChatKind::Channel ? "lng_profile_leave_channel" : "lng_profile_leave_group";
  g_menu_append(menu, Tr(leave).c_str(), "row.delete");

  auto *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  g_object_unref(menu);
  gtk_widget_set_parent(popover, parent);
  GdkRectangle rect = {int(x), int(y), 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  DestroyPopoverOnClose(popover);

  auto *group = g_simple_action_group_new();
  // The row may show another chat by the time an action runs, and this list may be gone with the session.
  const auto token = _lifetime.token();
  for (const auto *name : {"pin", "mute", "read", "archive", "delete"}) {
    auto *action = g_simple_action_new(name, nullptr);
    Connect<void(GSimpleAction *, GVariant *)>(action, "activate", [this, token, id, name = std::string(name)](GSimpleAction *, GVariant *) {
      if (!token.expired()) rowAction(id, name);
    });
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    g_object_unref(action);
  }
  gtk_widget_insert_action_group(popover, "row", G_ACTION_GROUP(group));
  g_object_unref(group);
  gtk_popover_popup(GTK_POPOVER(popover));
}

void ChatList::rowAction(Td::ChatId id, const std::string &action) {
  const auto chat = _store.chat(id);
  if (!chat) return;
  const auto failed = [](api::error &error) { ShowToast(error.message_); };
  if (action == "pin") {
    const auto &position = _kind == Td::ListKind::Main ? chat->main : chat->archive;
    send(api::make_object<api::toggleChatIsPinned>(MakeList(_kind), id, !position.pinned));
  } else if (action == "mute") {
    _store.setMuted(id, !chat->muted, failed);
  } else if (action == "read") {
    _store.markRead(id, failed);
  } else if (action == "archive") {
    toggleArchived(id);
  } else if (action == "delete") {
    confirmDelete(id);
  } else if (action == "delete-now") {
    deleteChat(id, false);
  }
}

void ChatList::send(Td::FunctionPtr function) {
  _store.client().request<api::ok>(std::move(function), _lifetime, [](api::ok &) {},
                                   [](api::error &error) { ShowToast(error.message_); });
}

void ChatList::toggleArchived(Td::ChatId id) {
  const auto chat = _store.chat(id);
  if (!chat) return;
  const auto archive = !chat->archive.order;
  _store.client().request<api::ok>(
      api::make_object<api::addChatToList>(id, MakeList(archive ? Td::ListKind::Archive : Td::ListKind::Main)),
      _lifetime, [archive](api::ok &) { ShowToast(Tr(archive ? "lng_archived_added" : "lng_archived_removed")); },
      [](api::error &error) { ShowToast(error.message_); });
}

void ChatList::confirmDelete(Td::ChatId id) {
  const auto chat = _store.chat(id);
  if (!chat) return;
  auto text = std::string();
  auto button = Tr("lng_box_delete");
  if (chat->supergroup) {
    text = Tr(chat->kind == Td::ChatKind::Channel ? "lng_sure_leave_channel" : "lng_sure_leave_group");
    button = Tr("lng_box_leave");
  } else if (chat->kind == Td::ChatKind::Group) {
    text = Tr("lng_sure_delete_and_exit", {{"group", chat->title}});
  } else if (isSelf(*chat)) {
    text = Tr("lng_sure_delete_saved_messages");
  } else {
    text = Tr("lng_sure_delete_history", {{"contact", chatName(*chat)}});
  }
  std::ranges::replace(text, '\n', ' ');
  auto *dialog = adw_alert_dialog_new(nullptr, text.c_str());
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", Tr("lng_cancel").c_str(), "delete", button.c_str(),
                                 nullptr);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);
  GtkWidget *revoke = nullptr;
  if (const auto user = chat->kind == Td::ChatKind::Private ? _store.user(chat->peerId) : nullptr;
      user && !isSelf(*chat) && !user->bot) {
    const auto name = user->firstName.empty() ? Td::DisplayName(*user) : user->firstName;
    revoke = gtk_check_button_new_with_label(Tr("lng_delete_for_other_check", {{"user", name}}).c_str());
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), revoke);
  }
  const auto token = _lifetime.token();
  Connect<void(AdwAlertDialog *, const char *)>(dialog, "response", [=, this](AdwAlertDialog *, const char *response) {
    if (token.expired() || g_strcmp0(response, "delete") != 0) return;
    deleteChat(id, revoke && gtk_check_button_get_active(GTK_CHECK_BUTTON(revoke)));
  });
  adw_dialog_present(ADW_DIALOG(dialog), _window.widget());
}

void ChatList::deleteChat(Td::ChatId id, bool revoke) {
  const auto chat = _store.chat(id);
  if (!chat) return;
  if (_window.currentChat() == id) _window.closeChat();
  if (chat->supergroup) {
    send(api::make_object<api::leaveChat>(id));
  } else if (chat->kind == Td::ChatKind::Group) {
    // A basic group is left first: its history can go from the list only after that. Already left is fine too.
    _store.client().send(api::make_object<api::leaveChat>(id), [this, id](Td::ObjectPtr) {
      send(api::make_object<api::deleteChatHistory>(id, true, false));
    }, _lifetime);
  } else {
    send(api::make_object<api::deleteChatHistory>(id, true, revoke));
  }
}

void ChatList::openSavedMessages() {
  _store.client().request<api::chat>(api::make_object<api::createPrivateChat>(_store.selfId(), false), _lifetime,
                                     [this](api::chat &chat) { _window.openChat(chat.id_); },
                                     [](api::error &error) { ShowToast(error.message_); });
}

void ChatList::startSearch(const std::string &query) {
  gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(_searchBar), TRUE);
  if (!query.empty()) gtk_editable_set_text(GTK_EDITABLE(_searchEntry), query.c_str());
  gtk_widget_grab_focus(_searchEntry);
}

void ChatList::applySearch() {
  _query = Trimmed(gtk_editable_get_text(GTK_EDITABLE(_searchEntry)));
  _words = Td::SearchWords(_query);
  _found.clear();
  _searchLifetime.reset();
  if (_searchTimer) g_source_remove(_searchTimer);
  _searchTimer = 0;
  if (_kind == Td::ListKind::Main && g_utf8_strlen(_query.c_str(), -1) >= kGlobalSearchMinLength) {
    _searchTimer = g_timeout_add_once(kSearchDelayMs, +[](gpointer data) {
      auto *self = static_cast<ChatList *>(data);
      self->_searchTimer = 0;
      self->searchGlobal();
    }, this);
  }
  refresh();
  loadMoreIfNear();
}

void ChatList::searchGlobal() {
  auto &client = _store.client();
  // A failed search just finds nothing, as the query is usually still being typed.
  const auto failed = [](api::error &) {};
  // The chats and contacts of this account that aren't loaded yet, then everybody else's.
  client.request<api::chats>(api::make_object<api::searchChatsOnServer>(_query, nullptr, kGlobalSearchLimit),
                             _searchLifetime, [this](api::chats &chats) { addFound(chats, true); }, failed);
  client.request<api::chats>(api::make_object<api::searchPublicChats>(_query, nullptr), _searchLifetime,
                             [this](api::chats &chats) { addFound(chats, false); }, failed);
}

void ChatList::addFound(const api::chats &chats, bool own) {
  _found.insert(own ? _found.begin() : _found.end(), chats.chat_ids_.begin(), chats.chat_ids_.end());
  refresh();
}

void ChatList::debugMenu(int index) {
  if (index < 0 || std::size_t(index) >= _shown.size()) return;
  const auto id = _shown[index].id;
  if (const auto it = _bound.find(id); it != _bound.end()) showContextMenu(id, it->second->root, 10, 10);
}

void ChatList::debugAction(int index, const std::string &action) {
  if (action == "saved") openSavedMessages();
  else if (index >= 0 && std::size_t(index) < _shown.size()) rowAction(_shown[index].id, action);
}

void ChatList::debugScroll(const std::string &where) {
  auto *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(_scroll));
  const auto end = gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment);
  gtk_adjustment_set_value(adjustment, where == "bottom" ? end : where == "top" ? 0. : std::atof(where.c_str()));
}

void ChatList::debugState() {
  auto *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(_scroll));
  const auto selected = gtk_single_selection_get_selected(_selection);
  g_message("list shown=%d model=%u bound=%d complete=%d selected=%d scroll=%.0f/%.0f",
            int(_shown.size()), g_list_model_get_n_items(G_LIST_MODEL(_model)), int(_bound.size()),
            int(_store.listComplete(_kind)), selected == GTK_INVALID_LIST_POSITION ? -1 : int(selected),
            gtk_adjustment_get_value(adjustment), gtk_adjustment_get_upper(adjustment));
}

}  // namespace Contrail
