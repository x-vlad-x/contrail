#include "forward.hpp"

#include "main_window.hpp"
#include "td/chat_row.hpp"
#include "td/strings.hpp"
#include "userpics.hpp"

#include <algorithm>
#include <utility>

namespace Contrail {
namespace {

namespace api = Td::api;

constexpr auto kChatKey = "contrail-chat";
constexpr auto kTopicKey = "contrail-topic";
constexpr auto kForumKey = "contrail-forum";
constexpr auto kNameKey = "contrail-name";
constexpr auto kUsernameKey = "contrail-username";
constexpr auto kAvatarSize = 40;
// More topics come when the list is scrolled this close to its end.
constexpr auto kLoadMoreDistance = 600.;

Td::ChatId RowChat(GtkListBoxRow *row) {
  const auto value = static_cast<const Td::ChatId *>(g_object_get_data(G_OBJECT(row), kChatKey));
  return value ? *value : 0;
}

std::int32_t RowTopic(GtkListBoxRow *row) {
  return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), kTopicKey));
}

const char *RowText(GtkListBoxRow *row, const char *key) {
  const auto value = static_cast<const char *>(g_object_get_data(G_OBJECT(row), key));
  return value ? value : "";
}

std::string Trimmed(const char *text) {
  auto *copy = g_strstrip(g_strdup(text));
  auto result = std::string(copy);
  g_free(copy);
  return result;
}

}  // namespace

ForwardPicker::ForwardPicker(Td::Store &store, MainWindow &window, ForwardDraft draft)
    : _store(store), _window(window), _draft(std::move(draft)) {
  _dialog = GTK_WIDGET(adw_dialog_new());
  g_object_ref_sink(_dialog);
  adw_dialog_set_content_width(ADW_DIALOG(_dialog), 480);
  adw_dialog_set_content_height(ADW_DIALOG(_dialog), 1000);
  _nav = GTK_WIDGET(adw_navigation_view_new());
  auto *page = adw_navigation_page_new(makePage(_chats), Tr("lng_forward_choose").c_str());
  adw_navigation_view_add(ADW_NAVIGATION_VIEW(_nav), page);
  adw_dialog_set_child(ADW_DIALOG(_dialog), _nav);
  // Back from a group's topics.
  _signals.connect<void(AdwNavigationView *, AdwNavigationPage *)>(
      _nav, "popped", [this](AdwNavigationView *, AdwNavigationPage *) { closeTopics(); });
  _signals.connect<void(AdwDialog *)>(_dialog, "closed", [this](AdwDialog *) { _window.forwardPickerClosed(); });

  // Saved Messages is offered even before it is used: TDLib gives its chat when asked.
  if (const auto self = _store.chat(_store.selfId()); self && self->kind == Td::ChatKind::Private) _self = self->id;
  _store.client().request<api::chat>(api::make_object<api::createPrivateChat>(_store.selfId(), false), _lifetime,
                                     [this](api::chat &chat) {
                                       _self = chat.id_;
                                       _shown.clear();
                                       fillChats();
                                     },
                                     [](api::error &) {});
  _store.onList([this](Td::ListKind kind) {
    if (kind == Td::ListKind::Main) fillChats();
  }, _lifetime);
  _store.onChat([this](Td::ChatId id) {
    // Whether one may write there changed, or the group stopped having topics.
    if (_forum == id) {
      const auto known = _store.chat(id);
      if (!known || !known->forum || !canForwardTo(*known)) {
        adw_navigation_view_pop(ADW_NAVIGATION_VIEW(_nav));
        return;
      }
    }
    if (std::ranges::find(_shown, id) != _shown.end()) {
      const auto known = _store.chat(id);
      if (!known || !canForwardTo(*known)) {
        _shown.clear();
        fillChats();
      }
    }
  }, _lifetime);
  fillChats();
}

ForwardPicker::~ForwardPicker() {
  _topicSignals.clear();
  _signals.clear();
  // Still shown: the account changed or the window goes.
  if (gtk_widget_get_parent(_dialog)) adw_dialog_force_close(ADW_DIALOG(_dialog));
  g_object_unref(_dialog);
}

void ForwardPicker::present() {
  adw_dialog_present(ADW_DIALOG(_dialog), _window.widget());
}

// A page: a search field over the list.
GtkWidget *ForwardPicker::makePage(Page &page) {
  auto &signals = (&page == &_chats) ? _signals : _topicSignals;
  page.view = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(page.view), adw_header_bar_new());
  page.search = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(page.search), Tr("lng_dlg_filter").c_str());
  auto *clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(clamp), page.search);
  gtk_widget_set_margin_start(clamp, 12);
  gtk_widget_set_margin_end(clamp, 12);
  gtk_widget_set_margin_bottom(clamp, 6);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(page.view), clamp);

  page.list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(page.list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(page.list, "navigation-sidebar");
  gtk_list_box_set_filter_func(GTK_LIST_BOX(page.list), filterRow, &page, nullptr);
  signals.connect<void(GtkSearchEntry *)>(page.search, "search-changed", [&page](GtkSearchEntry *) {
    page.words = Td::SearchWords(Trimmed(gtk_editable_get_text(GTK_EDITABLE(page.search))));
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(page.list));
  });
  signals.connect<void(GtkListBox *, GtkListBoxRow *)>(page.list, "row-activated",
                                                       [this](GtkListBox *, GtkListBoxRow *row) { activated(row); });
  // Enter in the search field picks the first one found.
  signals.connect<void(GtkSearchEntry *)>(page.search, "activate", [this, &page](GtkSearchEntry *) {
    for (auto *child = gtk_widget_get_first_child(page.list); child; child = gtk_widget_get_next_sibling(child)) {
      if (GTK_IS_LIST_BOX_ROW(child) && gtk_widget_get_child_visible(child)) {
        activated(GTK_LIST_BOX_ROW(child));
        return;
      }
    }
  });

  page.scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(page.scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(page.scroll), page.list);
  gtk_widget_set_vexpand(page.scroll, TRUE);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(page.view), page.scroll);
  return page.view;
}

gboolean ForwardPicker::filterRow(GtkListBoxRow *row, gpointer data) {
  const auto &page = *static_cast<const Page *>(data);
  return page.words.empty() || Td::MatchesSearch(RowText(row, kNameKey), RowText(row, kUsernameKey), page.words);
}

GtkWidget *ForwardPicker::makeRow(Td::ChatId chat, std::int32_t topic, bool forum) {
  auto name = std::string();
  auto username = std::string();
  GtkWidget *avatar = nullptr;
  if (topic) {
    const auto known = _store.topic(chat, topic);
    name = known ? known->row.title : std::string();
    // A topic has a colored letter (or "#" for General) instead of a picture.
    avatar = adw_avatar_new(kAvatarSize, (known && known->general) ? "#" : name.c_str(), TRUE);
  } else {
    const auto known = _store.chat(chat);
    name = known ? chatName(*known) : std::string();
    username = known ? _store.username(*known) : std::string();
    avatar = MakeAvatar(_store, chat, kAvatarSize, true);
  }
  auto *row = gtk_list_box_row_new();
  g_object_set_data_full(G_OBJECT(row), kChatKey, new Td::ChatId(chat),
                         [](gpointer data) { delete static_cast<Td::ChatId *>(data); });
  g_object_set_data(G_OBJECT(row), kTopicKey, GINT_TO_POINTER(topic));
  g_object_set_data(G_OBJECT(row), kForumKey, GINT_TO_POINTER(forum));
  g_object_set_data_full(G_OBJECT(row), kNameKey, g_strdup(name.c_str()), g_free);
  g_object_set_data_full(G_OBJECT(row), kUsernameKey, g_strdup(username.c_str()), g_free);
  auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(box, 4);
  gtk_widget_set_margin_bottom(box, 4);
  gtk_box_append(GTK_BOX(box), avatar);
  auto *label = gtk_label_new(name.c_str());
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_widget_add_css_class(label, "chat-name");
  gtk_box_append(GTK_BOX(box), label);
  if (forum) gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name("go-next-symbolic"));
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  return row;
}

std::string ForwardPicker::chatName(const Td::Chat &chat) const {
  if (chat.id == _self) return Tr("lng_saved_messages");
  return chat.title;
}

bool ForwardPicker::canForwardTo(const Td::Chat &chat) const {
  if (chat.id == _store.repliesChatId() || !_store.canWrite(chat)) return false;
  const auto group = _store.group(chat);
  return !group || group->member;
}

void ForwardPicker::fillChats() {
  // Saved Messages first, as in the phone apps.
  auto ids = std::vector<Td::ChatId>();
  if (_self && _store.chat(_self)) ids.push_back(_self);
  for (const auto id : _store.list(Td::ListKind::Main)) {
    const auto known = _store.chat(id);
    if (id != _self && known && canForwardTo(*known)) ids.push_back(id);
  }
  // A new message only moves a chat: the rows stay where they are while one is chosen.
  if (ids.size() == _shown.size()) {
    auto now = ids, before = _shown;
    std::ranges::sort(now);
    std::ranges::sort(before);
    if (now == before) return;
  }
  _shown = std::move(ids);
  gtk_list_box_remove_all(GTK_LIST_BOX(_chats.list));
  for (const auto id : _shown) {
    const auto known = _store.chat(id);
    gtk_list_box_append(GTK_LIST_BOX(_chats.list), makeRow(id, 0, known && known->forum));
  }
}

ForwardPicker::Page &ForwardPicker::current() {
  return _topics.list ? _topics : _chats;
}

void ForwardPicker::activated(GtkListBoxRow *row) {
  const auto chat = RowChat(row);
  if (!chat) return;
  if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), kForumKey))) {
    showTopics(chat);
  } else {
    chosen(chat, RowTopic(row));
  }
}

void ForwardPicker::showTopics(Td::ChatId chat) {
  const auto known = _store.chat(chat);
  if (!known) return;
  closeTopics();
  _forum = chat;
  _topicHold = _store.holdTopics(chat);
  auto *page = adw_navigation_page_new(makePage(_topics), known->title.c_str());
  auto *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(_topics.scroll));
  const auto loadMore = [this](GtkAdjustment *adjustment) {
    const auto left = gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_value(adjustment)
        - gtk_adjustment_get_page_size(adjustment);
    if (_forum && left < kLoadMoreDistance && !_store.topicsComplete(_forum)) _store.loadTopics(_forum);
  };
  _topicSignals.connect<void(GtkAdjustment *)>(adjustment, "value-changed", loadMore);
  _topicSignals.connect<void(GtkAdjustment *)>(adjustment, "changed", loadMore);
  _store.onTopic([this](Td::ChatId chat, std::int32_t) {
    if (chat == _forum) fillTopics();
  }, _forumLifetime);
  adw_navigation_view_push(ADW_NAVIGATION_VIEW(_nav), page);
  fillTopics();
  _store.loadTopics(chat);
}

void ForwardPicker::fillTopics() {
  if (!_forum || !_topics.list) return;
  const auto known = _store.chat(_forum);
  const auto group = known ? _store.group(*known) : nullptr;
  const auto manage = group && group->manageTopics;
  gtk_list_box_remove_all(GTK_LIST_BOX(_topics.list));
  for (const auto id : _store.topics(_forum)) {
    const auto topic = _store.topic(_forum, id);
    // Only those who manage topics write in a closed one.
    if (!topic || (topic->closed && !manage)) continue;
    gtk_list_box_append(GTK_LIST_BOX(_topics.list), makeRow(_forum, id, false));
  }
}

void ForwardPicker::closeTopics() {
  _topicSignals.clear();
  _forumLifetime.reset();
  _topicHold = nullptr;
  _forum = 0;
  _topics = Page();
}

void ForwardPicker::chosen(Td::ChatId chat, std::int32_t topic) {
  if (_draft.ids.empty()) {
    adw_dialog_close(ADW_DIALOG(_dialog));
    return;
  }
  // The window destroys this picker once the dialog has closed, not before.
  auto &window = _window;
  window.setForwardDraft(chat, topic, std::move(_draft));
  _draft = ForwardDraft();
  adw_dialog_close(ADW_DIALOG(_dialog));
  if (topic) {
    window.openTopic(chat, topic);
  } else {
    window.openChat(chat);
  }
}

void ForwardPicker::debugPick(int index, bool topics) {
  auto &page = topics ? _topics : _chats;
  if (!page.list) return;
  auto shown = 0;
  for (auto *child = gtk_widget_get_first_child(page.list); child; child = gtk_widget_get_next_sibling(child)) {
    if (!GTK_IS_LIST_BOX_ROW(child) || !gtk_widget_get_child_visible(child)) continue;
    if (shown++ == index) {
      activated(GTK_LIST_BOX_ROW(child));
      return;
    }
  }
}

void ForwardPicker::debugSearch(const std::string &text) {
  gtk_editable_set_text(GTK_EDITABLE(current().search), text.c_str());
}

void ForwardPicker::debugEnter() {
  g_signal_emit_by_name(current().search, "activate");
}

void ForwardPicker::debugState() const {
  const auto &page = _topics.list ? _topics : _chats;
  auto rows = std::string();
  auto count = 0;
  for (auto *child = gtk_widget_get_first_child(page.list); child; child = gtk_widget_get_next_sibling(child)) {
    if (!GTK_IS_LIST_BOX_ROW(child) || !gtk_widget_get_child_visible(child)) continue;
    if (count++ < 12) rows += std::string(" \"") + RowText(GTK_LIST_BOX_ROW(child), kNameKey) + "\"";
  }
  g_message("forward picker: page=%s forum=%lld shown=%d messages=%zu rows:%s", _topics.list ? "topics" : "chats",
            (long long)_forum, count, _draft.ids.size(), rows.c_str());
}

}  // namespace Contrail
