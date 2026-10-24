#include "forward.hpp"

#include "main_window.hpp"
#include "text.hpp"
#include "userpics.hpp"

#include "base/weak_ptr.h"
#include "data/data_chat_participant_status.h"
#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "history/history_item_helpers.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"

#include <rpl/lifetime.h>

namespace Contrail {
namespace {

constexpr auto kThreadKey = "contrail-thread";
constexpr auto kNameKey = "contrail-name";

QString ThreadName(not_null<Data::Thread *> thread) {
  if (const auto topic = thread->asTopic()) return topic->title();
  const auto peer = thread->peer();
  return peer->isSelf() ? tr::lng_saved_messages(tr::now) : peer->name();
}

bool Matches(const QString &name, const QString &username, const QString &query) {
  const auto lower = name.toLower();
  for (const auto &word : query.toLower().split(' ', Qt::SkipEmptyParts)) {
    if (!lower.contains(word) && !username.toLower().startsWith(word)) return false;
  }
  return true;
}

class ForwardPicker final : public base::has_weak_ptr {
public:
  ForwardPicker(not_null<MainWindow *> window, not_null<Main::Session *> session, MessageIdsList ids);

  [[nodiscard]] GtkWidget *dialog() const { return _dialog; }

private:
  GtkWidget *makeList(GtkWidget **search, GtkWidget **list);
  GtkWidget *makeRow(not_null<Data::Thread *> thread, bool forum);
  void fillChats();
  void showTopics(not_null<Data::Forum *> forum);
  void fillTopics();
  void chosen(not_null<Data::Thread *> thread);

  const not_null<MainWindow *> _window;
  const not_null<Main::Session *> _session;
  const MessageIdsList _ids;
  GtkWidget *_dialog = nullptr;
  GtkWidget *_nav = nullptr;
  GtkWidget *_chats = nullptr;
  GtkWidget *_chatsSearch = nullptr;
  GtkWidget *_topics = nullptr;
  GtkWidget *_topicsSearch = nullptr;
  Data::Forum *_forum = nullptr;
  rpl::lifetime _forumLifetime;
  rpl::lifetime _lifetime;
};

ForwardPicker::ForwardPicker(not_null<MainWindow *> window, not_null<Main::Session *> session, MessageIdsList ids)
    : _window(window), _session(session), _ids(std::move(ids)) {
  _dialog = GTK_WIDGET(adw_dialog_new());
  adw_dialog_set_content_width(ADW_DIALOG(_dialog), 480);
  adw_dialog_set_content_height(ADW_DIALOG(_dialog), 1000);
  _nav = GTK_WIDGET(adw_navigation_view_new());
  auto *page = adw_navigation_page_new(makeList(&_chatsSearch, &_chats), U8(tr::lng_forward_choose(tr::now)).c_str());
  adw_navigation_view_add(ADW_NAVIGATION_VIEW(_nav), page);
  adw_dialog_set_child(ADW_DIALOG(_dialog), _nav);
  // Back from a group's topics.
  Connect<void(AdwNavigationView *, AdwNavigationPage *)>(_nav, "popped", crl::guard(this, [=](AdwNavigationView *, AdwNavigationPage *) {
    _forumLifetime.destroy();
    _forum = nullptr;
    _topics = nullptr;
  }));
  fillChats();
  _session->data().chatsListChanges() | rpl::filter([](Data::Folder *folder) { return !folder; })
      | rpl::on_next([=] { fillChats(); }, _lifetime);
}

// A page: the header, a search field and the list.
GtkWidget *ForwardPicker::makeList(GtkWidget **search, GtkWidget **list) {
  auto *view = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());
  *search = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(*search), U8(tr::lng_dlg_filter(tr::now)).c_str());
  auto *searchClamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(searchClamp), *search);
  gtk_widget_set_margin_start(searchClamp, 12);
  gtk_widget_set_margin_end(searchClamp, 12);
  gtk_widget_set_margin_bottom(searchClamp, 6);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), searchClamp);

  *list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(*list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(*list, "navigation-sidebar");
  auto *field = *search;
  gtk_list_box_set_filter_func(
      GTK_LIST_BOX(*list),
      +[](GtkListBoxRow *row, gpointer data) -> gboolean {
        const auto query = Q(gtk_editable_get_text(GTK_EDITABLE(data))).trimmed();
        if (query.isEmpty()) return TRUE;
        const auto thread = static_cast<Data::Thread *>(g_object_get_data(G_OBJECT(row), kThreadKey));
        const auto name = Q(static_cast<const char *>(g_object_get_data(G_OBJECT(row), kNameKey)));
        return Matches(name, thread->asTopic() ? QString() : thread->peer()->username(), query);
      },
      field, nullptr);
  Connect<void(GtkSearchEntry *)>(field, "search-changed", [list = *list](GtkSearchEntry *) {
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(list));
  });
  Connect<void(GtkListBox *, GtkListBoxRow *)>(*list, "row-activated", crl::guard(this, [=](GtkListBox *, GtkListBoxRow *row) {
    const auto thread = static_cast<Data::Thread *>(g_object_get_data(G_OBJECT(row), kThreadKey));
    if (!thread) return;
    if (const auto forum = thread->asTopic() ? nullptr : thread->peer()->forum()) {
      showTopics(forum);
    } else {
      chosen(thread);
    }
  }));
  // Enter in the search field picks the first chat found.
  Connect<void(GtkSearchEntry *)>(field, "activate", [list = *list](GtkSearchEntry *) {
    for (auto *child = gtk_widget_get_first_child(list); child; child = gtk_widget_get_next_sibling(child)) {
      if (GTK_IS_LIST_BOX_ROW(child) && gtk_widget_get_child_visible(child)) {
        g_signal_emit_by_name(list, "row-activated", child);
        return;
      }
    }
  });

  auto *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), *list);
  gtk_widget_set_vexpand(scroll, TRUE);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroll);
  return view;
}

GtkWidget *ForwardPicker::makeRow(not_null<Data::Thread *> thread, bool forum) {
  const auto name = ThreadName(thread);
  auto *row = gtk_list_box_row_new();
  g_object_set_data(G_OBJECT(row), kThreadKey, thread.get());
  g_object_set_data_full(G_OBJECT(row), kNameKey, g_strdup(U8(name).c_str()), g_free);
  auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(box, 4);
  gtk_widget_set_margin_bottom(box, 4);
  if (const auto topic = thread->asTopic()) {
    gtk_box_append(GTK_BOX(box), adw_avatar_new(40, U8(topic->isGeneral() ? u"#"_q : name).c_str(), TRUE));
  } else {
    gtk_box_append(GTK_BOX(box), MakeAvatar(thread->peer(), 40, true));
  }
  auto *label = gtk_label_new(U8(name).c_str());
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_widget_add_css_class(label, "chat-name");
  gtk_box_append(GTK_BOX(box), label);
  if (forum) gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name("go-next-symbolic"));
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  return row;
}

void ForwardPicker::fillChats() {
  gtk_list_box_remove_all(GTK_LIST_BOX(_chats));
  // Saved Messages first, as in the phone apps.
  const auto self = _session->data().history(_session->user());
  gtk_list_box_append(GTK_LIST_BOX(_chats), makeRow(self, false));
  for (const auto &row : _session->data().chatsList()->indexed()->all()) {
    const auto history = row->history();
    if (!history || history == self) continue;
    const auto forum = history->peer->isForum();
    if (!Data::CanSendAnything(history->peer, false)) continue;
    gtk_list_box_append(GTK_LIST_BOX(_chats), makeRow(history, forum));
  }
}

void ForwardPicker::showTopics(not_null<Data::Forum *> forum) {
  _forum = forum;
  _forumLifetime.destroy();
  auto *page = adw_navigation_page_new(makeList(&_topicsSearch, &_topics), U8(forum->peer()->name()).c_str());
  adw_navigation_view_push(ADW_NAVIGATION_VIEW(_nav), page);
  forum->requestTopics();
  forum->chatsListChanges() | rpl::on_next([=] { fillTopics(); }, _forumLifetime);
  forum->destroyed() | rpl::on_next([=] {
    _forum = nullptr;
    // Not from inside the lifetime that "popped" ends.
    crl::on_main(this, [=] { adw_navigation_view_pop(ADW_NAVIGATION_VIEW(_nav)); });
  }, _forumLifetime);
  fillTopics();
}

void ForwardPicker::fillTopics() {
  if (!_forum || !_topics) return;
  gtk_list_box_remove_all(GTK_LIST_BOX(_topics));
  for (const auto &row : _forum->topicsList()->indexed()->all()) {
    const auto topic = row->topic();
    if (!topic || !Data::CanSendAnything(topic)) continue;
    gtk_list_box_append(GTK_LIST_BOX(_topics), makeRow(topic, false));
  }
}

void ForwardPicker::chosen(not_null<Data::Thread *> thread) {
  const auto items = _session->data().idsToItems(_ids);
  if (items.empty()) {
    adw_dialog_close(ADW_DIALOG(_dialog));
    return;
  }
  if (const auto error = GetErrorForSending(thread, {.forward = &items}); !error.text.isEmpty()) {
    ShowToast(error.text);
    return;
  }
  const auto history = thread->owningHistory();
  history->setForwardDraft(thread->topicRootId(), thread->monoforumPeerId(), {.ids = _ids});
  adw_dialog_close(ADW_DIALOG(_dialog));
  _window->openThread(thread);
}

}  // namespace

void ShowForwardPicker(not_null<MainWindow *> window, MessageIdsList ids) {
  const auto session = window->session();
  if (!session || !window->widget() || ids.empty()) return;
  auto picker = std::make_unique<ForwardPicker>(window, session, std::move(ids));
  auto *dialog = picker->dialog();
  AttachData(dialog, "contrail-forward-picker", std::move(picker));
  adw_dialog_present(ADW_DIALOG(dialog), window->widget());
}

}  // namespace Contrail
