#include "topic_list.hpp"

#include "main_window.hpp"
#include "td/chat_row.hpp"
#include "td/dates.hpp"
#include "td/strings.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace Contrail {
namespace {

namespace api = Td::api;

constexpr auto kTopicKey = "contrail-topic";
constexpr auto kAvatarSize = 48;

// What comes first in a preview: "Draft:", "You:".
std::string Lead(const std::string &text) {
  return "<span weight=\"600\">" + Escape(text + ": ") + "</span>";
}

std::int32_t RowTopic(GtkListBoxRow *row) {
  return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), kTopicKey));
}

std::string Trimmed(const char *text) {
  auto *copy = g_strstrip(g_strdup(text));
  auto result = std::string(copy);
  g_free(copy);
  return result;
}

}  // namespace

TopicList::TopicList(Td::Store &store, Td::ChatId chat, MainWindow &window)
    : _store(store), _chat(chat), _window(window), _hold(store.holdTopics(chat)) {
  buildPage();
  _store.onTopic([this](Td::ChatId chat, std::int32_t id) {
    if (chat != _chat) return;
    if (id) topicChanged(id);
    else refresh();
  }, _lifetime);
  _store.onChat([this](Td::ChatId id) {
    if (id == _chat) chatChanged();
  }, _lifetime);
  _store.onUser([this](Td::UserId id) {
    // A row names who wrote the last message.
    for (const auto &[topic, row] : _rows) {
      if (const auto known = _store.topic(_chat, topic); known && known->row.lastSenderUser == id) updateRow(*row);
    }
  }, _lifetime);
  refresh();
  _store.loadTopics(_chat);
}

TopicList::~TopicList() {
  if (_closeSource) g_source_remove(_closeSource);
  if (_loadSource) g_source_remove(_loadSource);
  _connections.clear();
  g_object_unref(_page);
}

void TopicList::buildPage() {
  auto *view = adw_toolbar_view_new();
  auto *header = adw_header_bar_new();
  const auto known = _store.chat(_chat);
  const auto title = known ? known->title : std::string();
  _title = adw_window_title_new(title.c_str(), "");
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), _title);
  auto *searchToggle = gtk_toggle_button_new();
  gtk_button_set_icon_name(GTK_BUTTON(searchToggle), "system-search-symbolic");
  gtk_widget_set_tooltip_text(searchToggle, Tr("lng_dlg_filter").c_str());
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), searchToggle);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

  // The topics already here are filtered by their names.
  _searchEntry = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(_searchEntry), Tr("lng_dlg_filter").c_str());
  gtk_widget_set_hexpand(_searchEntry, TRUE);
  auto *clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(clamp), _searchEntry);
  auto *searchBar = gtk_search_bar_new();
  gtk_search_bar_set_child(GTK_SEARCH_BAR(searchBar), clamp);
  gtk_search_bar_connect_entry(GTK_SEARCH_BAR(searchBar), GTK_EDITABLE(_searchEntry));
  g_object_bind_property(searchToggle, "active", searchBar, "search-mode-enabled",
                         GBindingFlags(G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE));
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), searchBar);
  _connections.connect<void(GtkSearchEntry *)>(_searchEntry, "search-changed", [this](GtkSearchEntry *) {
    _words = Td::SearchWords(Trimmed(gtk_editable_get_text(GTK_EDITABLE(_searchEntry))));
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(_list));
  });
  _connections.connect<void(GtkSearchEntry *)>(_searchEntry, "activate", [this](GtkSearchEntry *) { openRow(0); });
  _connections.connect<void(GtkSearchEntry *)>(_searchEntry, "stop-search", [searchToggle](GtkSearchEntry *) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(searchToggle), FALSE);
  });

  _list = gtk_list_box_new();
  gtk_widget_add_css_class(_list, "navigation-sidebar");
  gtk_widget_add_css_class(_list, "chat-list");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(_list), GTK_SELECTION_SINGLE);
  gtk_list_box_set_sort_func(GTK_LIST_BOX(_list), compareRows, this, nullptr);
  gtk_list_box_set_filter_func(GTK_LIST_BOX(_list), filterRow, this, nullptr);
  _connections.connect<void(GtkListBox *, GtkListBoxRow *)>(_list, "row-activated", [this](GtkListBox *, GtkListBoxRow *row) {
    _window.openTopic(_chat, RowTopic(row));
  });

  _scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(_scroll), _list);
  gtk_widget_set_vexpand(_scroll, TRUE);
  auto *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(_scroll));
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

  _page = ADW_NAVIGATION_PAGE(adw_navigation_page_new(view, title.c_str()));
  adw_navigation_page_set_tag(_page, "forum");
  g_object_ref_sink(_page);
}

TopicList::Row &TopicList::addRow(std::int32_t id) {
  auto owned = std::make_unique<Row>();
  auto *row = owned.get();
  row->id = id;
  row->row = gtk_list_box_row_new();
  g_object_set_data(G_OBJECT(row->row), kTopicKey, GINT_TO_POINTER(id));

  auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);
  // A topic has a colored letter (or "#" for General) instead of a picture.
  row->avatar = adw_avatar_new(kAvatarSize, nullptr, TRUE);
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
  row->closed = gtk_image_new_from_icon_name("system-lock-screen-symbolic");
  gtk_widget_add_css_class(row->closed, "dim-label");
  gtk_image_set_pixel_size(GTK_IMAGE(row->closed), 12);
  gtk_box_append(GTK_BOX(top), row->closed);
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

  // Long press (touch) or right click: the topic's menu. It hangs on the box: a box lets go of a popover still open
  // when the list goes, a list row doesn't.
  auto *longPress = gtk_gesture_long_press_new();
  gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(longPress), TRUE);
  Connect<void(GtkGestureLongPress *, double, double)>(longPress, "pressed", [this, row](GtkGestureLongPress *gesture, double x, double y) {
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    showContextMenu(row->id, gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row->row)), x, y);
  });
  gtk_widget_add_controller(row->row, GTK_EVENT_CONTROLLER(longPress));
  auto *rightClick = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rightClick), GDK_BUTTON_SECONDARY);
  Connect<void(GtkGestureClick *, int, double, double)>(rightClick, "pressed", [this, row](GtkGestureClick *, int, double x, double y) {
    showContextMenu(row->id, gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row->row)), x, y);
  });
  gtk_widget_add_controller(row->row, GTK_EVENT_CONTROLLER(rightClick));

  gtk_list_box_append(GTK_LIST_BOX(_list), row->row);
  updateRow(*row);
  return *_rows.emplace(id, std::move(owned)).first->second;
}

void TopicList::updateRow(Row &row) {
  const auto topic = _store.topic(_chat, row.id);
  if (!topic) return;
  const auto &data = topic->row;
  gtk_label_set_text(GTK_LABEL(row.name), data.title.c_str());
  adw_avatar_set_text(ADW_AVATAR(row.avatar), topic->general ? "#" : data.title.c_str());
  gtk_widget_set_visible(row.muted, data.muted);
  gtk_widget_set_visible(row.closed, topic->closed);
  gtk_label_set_text(GTK_LABEL(row.date), data.lastDate ? Td::FormatListDate(data.lastDate).c_str() : "");

  const auto icon = Td::ChatStatusIcon(data, _store.selfId());
  gtk_widget_set_visible(row.status, icon != nullptr);
  if (icon) gtk_image_set_from_icon_name(GTK_IMAGE(row.status), icon);

  auto markup = std::string();
  if (!data.draft.empty()) {
    markup = Lead(Tr("lng_from_draft")) + Escape(Td::OneLine(data.draft));
  } else if (const auto sender = Td::PreviewSender(data, _store); !sender.empty()) {
    markup = Lead(sender) + Escape(data.preview);
  } else {
    markup = Escape(data.preview);
  }
  if (row.previewMarkup != markup) {
    row.previewMarkup = std::move(markup);
    gtk_label_set_markup(GTK_LABEL(row.preview), row.previewMarkup.c_str());
  }

  const auto badge = Td::ChatBadge(data);
  gtk_widget_set_visible(row.badge, badge.visible);
  if (badge.visible) {
    gtk_label_set_text(GTK_LABEL(row.badge), badge.text.c_str());
    if (badge.muted) gtk_widget_add_css_class(row.badge, "muted");
    else gtk_widget_remove_css_class(row.badge, "muted");
  }
  gtk_widget_set_visible(row.pinned, !badge.visible && topic->pinned);
}

void TopicList::topicChanged(std::int32_t id) {
  const auto it = _rows.find(id);
  if (it == _rows.end()) {
    if (_store.topic(_chat, id)) refresh();
    return;
  }
  if (!_store.topic(_chat, id)) {
    refresh();  // Deleted.
    return;
  }
  updateRow(*it->second);
  // The name may take it into the filter or out of it.
  if (!_words.empty()) gtk_list_box_row_changed(GTK_LIST_BOX_ROW(it->second->row));
}

void TopicList::refresh() {
  const auto &ids = _store.topics(_chat);
  _position.clear();
  for (auto i = 0; i < int(ids.size()); ++i) _position[ids[i]] = i;
  for (auto it = _rows.begin(); it != _rows.end();) {
    if (_position.contains(it->first)) {
      ++it;
      continue;
    }
    gtk_list_box_remove(GTK_LIST_BOX(_list), it->second->row);
    it = _rows.erase(it);
  }
  for (const auto id : ids) {
    if (const auto it = _rows.find(id); it != _rows.end()) updateRow(*it->second);
    else addRow(id);
  }
  gtk_list_box_invalidate_sort(GTK_LIST_BOX(_list));
  setCurrent(_current);
  const auto loaded = _store.topicsLoaded(_chat) || !_rows.empty();
  gtk_stack_set_visible_child_name(GTK_STACK(_stack), loaded ? "list" : "loading");
  if (g_getenv("CONTRAIL_DEBUG")) g_message("Topics: %d", int(_rows.size()));
}

void TopicList::chatChanged() {
  const auto known = _store.chat(_chat);
  if (known) {
    adw_window_title_set_title(ADW_WINDOW_TITLE(_title), known->title.c_str());
    adw_navigation_page_set_title(_page, known->title.c_str());
  }
  // The group stopped being a forum, or is gone: there are no topics to show. Not now: this runs inside the
  // store's notification, and closing destroys this list.
  if ((known && known->forum) || _closeSource) return;
  _closeSource = g_idle_add_once(+[](gpointer data) {
    auto *self = static_cast<TopicList *>(data);
    self->_closeSource = 0;
    self->_window.closeForum(self->_chat);
  }, this);
}

void TopicList::loadMoreIfNear() {
  if (_loadSource || _store.topicsComplete(_chat)) return;
  auto *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(_scroll));
  const auto page = gtk_adjustment_get_page_size(adjustment);
  if (page <= 0. || gtk_adjustment_get_value(adjustment) + 3 * page < gtk_adjustment_get_upper(adjustment)) return;
  // This can run in the middle of a layout.
  _loadSource = g_idle_add_once(+[](gpointer data) {
    auto *self = static_cast<TopicList *>(data);
    self->_loadSource = 0;
    self->_store.loadTopics(self->_chat);
  }, this);
}

int TopicList::compareRows(GtkListBoxRow *a, GtkListBoxRow *b, gpointer data) {
  const auto self = static_cast<TopicList *>(data);
  const auto position = [&](GtkListBoxRow *row) {
    const auto it = self->_position.find(RowTopic(row));
    return it == self->_position.end() ? int(self->_position.size()) : it->second;
  };
  return position(a) - position(b);
}

gboolean TopicList::filterRow(GtkListBoxRow *row, gpointer data) {
  const auto self = static_cast<TopicList *>(data);
  if (self->_words.empty()) return TRUE;
  const auto topic = self->_store.topic(self->_chat, RowTopic(row));
  return topic && Td::MatchesSearch(topic->row.title, std::string(), self->_words);
}

void TopicList::setCurrent(std::int32_t topic) {
  _current = topic;
  if (const auto it = _rows.find(topic); topic && it != _rows.end()) {
    gtk_list_box_select_row(GTK_LIST_BOX(_list), GTK_LIST_BOX_ROW(it->second->row));
  } else {
    gtk_list_box_unselect_all(GTK_LIST_BOX(_list));
  }
}

std::int32_t TopicList::topicAt(int index) const {
  // The order on the screen: filtered and sorted.
  auto shown = 0;
  for (auto i = 0; auto *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(_list), i); ++i) {
    if (!gtk_widget_get_child_visible(GTK_WIDGET(row))) continue;
    if (shown++ == index) return RowTopic(row);
  }
  return 0;
}

void TopicList::openRow(int index) {
  if (const auto id = topicAt(index)) _window.openTopic(_chat, id);
}

void TopicList::showContextMenu(std::int32_t id, GtkWidget *parent, double x, double y) {
  const auto topic = _store.topic(_chat, id);
  if (!topic) return;
  auto *menu = g_menu_new();
  g_menu_append(menu, Tr(topic->row.muted ? "lng_context_unmute" : "lng_context_mute").c_str(), "row.mute");
  if (Td::ChatIsUnread(topic->row)) g_menu_append(menu, Tr("lng_context_mark_read").c_str(), "row.read");
  // Pinning, archiving and leaving are for the whole group.

  auto *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  g_object_unref(menu);
  gtk_widget_set_parent(popover, parent);
  GdkRectangle rect = {int(x), int(y), 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  DestroyPopoverOnClose(popover);

  auto *group = g_simple_action_group_new();
  // This list may be gone by the time an action runs.
  const auto token = _lifetime.token();
  for (const auto *name : {"mute", "read"}) {
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

void TopicList::rowAction(std::int32_t id, const std::string &action) {
  const auto topic = _store.topic(_chat, id);
  if (!topic) return;
  const auto failed = [](api::error &error) { ShowToast(error.message_); };
  if (action == "mute") {
    _store.setTopicMuted(_chat, id, !topic->row.muted, failed);
  } else if (action == "read") {
    _store.markTopicRead(_chat, id, failed);
  }
}

void TopicList::debugMenu(int index) {
  const auto id = topicAt(index);
  if (const auto it = _rows.find(id); it != _rows.end()) {
    showContextMenu(id, gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(it->second->row)), 10, 10);
  }
}

void TopicList::debugAction(int index, const std::string &action) {
  if (const auto id = topicAt(index)) rowAction(id, action);
}

void TopicList::debugState() const {
  auto line = std::string();
  for (auto i = 0; auto *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(_list), i); ++i) {
    const auto topic = _store.topic(_chat, RowTopic(row));
    if (!topic) continue;
    line += " [" + std::to_string(topic->id) + (topic->pinned ? " pinned" : "") + (topic->row.muted ? " muted" : "")
        + " unread=" + std::to_string(topic->row.unread) + (gtk_list_box_row_is_selected(row) ? " current" : "") + "]";
  }
  g_message("topics chat=%lld loaded=%d complete=%d:%s", (long long)_chat, int(_store.topicsLoaded(_chat)),
            int(_store.topicsComplete(_chat)), line.c_str());
}

}  // namespace Contrail
