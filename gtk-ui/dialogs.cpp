#include "dialogs.hpp"

#include "chat_list.hpp"
#include "main_window.hpp"
#include "td/dates.hpp"
#include "td/strings.hpp"
#include "userpics.hpp"

#include "contrail_version.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

namespace Contrail {
namespace api = Td::api;
namespace {

constexpr auto kMembersShown = 50;  // The rest are in the group itself, as the phone apps show the first ones.

std::function<void(Td::UserId, bool)> &CallHandler() {
  static auto handler = std::function<void(Td::UserId, bool)>();
  return handler;
}

template <typename Type>
[[nodiscard]] const Type *Cast(const api::Object *object) {
  return (object && object->get_id() == Type::ID) ? static_cast<const Type *>(object) : nullptr;
}

[[nodiscard]] std::string PhoneText(const std::string &phone) {
  return (phone.empty() || phone.front() == '+') ? phone : "+" + phone;
}

GtkWidget *InfoRow(const std::string &title, const std::string &value) {
  auto *row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), value.c_str());
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), Escape(title).c_str());
  adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), FALSE);
  gtk_widget_add_css_class(row, "property");
  auto *copy = gtk_button_new_from_icon_name("edit-copy-symbolic");
  gtk_widget_add_css_class(copy, "flat");
  gtk_widget_set_valign(copy, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(copy, Tr("lng_context_copy_text").c_str());
  Connect<void(GtkButton *)>(copy, "clicked", [value](GtkButton *) {
    CopyToClipboard(value);
    ShowToast(Tr("lng_text_copied"));
  });
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), copy);
  return row;
}

// A page of the card's lists: a user with the avatar, the name and a line under it.
GtkWidget *PersonRow(Td::Store &store, Td::UserId user, const std::string &subtitle) {
  auto *row = adw_action_row_new();
  const auto known = store.user(user);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), Escape(known ? Td::DisplayName(*known) : std::string()).c_str());
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), Escape(subtitle).c_str());
  adw_action_row_add_prefix(ADW_ACTION_ROW(row), MakeUserAvatar(store, user, 36));
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
  g_object_set_data(G_OBJECT(row), "contrail-user", GINT_TO_POINTER(1));
  return row;
}

// Opens the chat with a user (it is made if there is none yet), or calls `then` with its id instead.
void WithPrivateChat(MainWindow &window, Td::UserId user, std::function<void(Td::ChatId)> then) {
  const auto store = window.store();
  if (!store) return;
  // The window outlives every answer: it holds the store and the client.
  store->client().send(api::make_object<api::createPrivateChat>(user, false), [then](Td::ObjectPtr object) {
    if (const auto chat = Cast<api::chat>(object.get())) then(chat->id_);
  });
}

// --- the card ----------------------------------------------------------------

class ProfileCard final {
 public:
  ProfileCard(MainWindow &window, Td::ChatId chat);

  [[nodiscard]] GtkWidget *dialog() const { return _dialog; }
  void debug(const std::string &name, const std::string &rest);

 private:
  void build();
  void refresh();
  void requestFull();
  void setInfo(std::vector<std::pair<std::string, std::string>> rows);
  void showMembers(std::vector<std::pair<Td::UserId, std::string>> members, int total);
  [[nodiscard]] std::string statusText() const;

  MainWindow &_window;
  Td::Store &_store;
  const Td::ChatId _chat;
  GtkWidget *_dialog = nullptr;
  GtkWidget *_page = nullptr;
  GtkWidget *_name = nullptr;
  GtkWidget *_status = nullptr;
  GtkWidget *_info = nullptr;
  std::vector<GtkWidget *> _infoRows;
  std::vector<GtkWidget *> _callRows;  // A call, then a video call: a person's card only.
  GtkWidget *_mute = nullptr;
  GtkWidget *_members = nullptr;
  GtkWidget *_membersList = nullptr;
  std::string _phone;
  std::string _about;
  int _memberCount = 0;
  bool _muteChanging = false;
  Td::Lifetime _lifetime;
  SignalConnections _signals;
};

ProfileCard::ProfileCard(MainWindow &window, Td::ChatId chat)
    : _window(window), _store(*window.store()), _chat(chat) {
  build();
  refresh();
  requestFull();
  _store.onChat([this](Td::ChatId id) {
    if (id == _chat) refresh();
  }, _lifetime);
  _store.onUser([this](Td::UserId id) {
    const auto known = _store.chat(_chat);
    if (known && known->kind == Td::ChatKind::Private && known->peerId == id) refresh();
  }, _lifetime);
  _signals.connect<void(AdwDialog *)>(_dialog, "closed", [this](AdwDialog *) { _lifetime.reset(); });
}

void ProfileCard::build() {
  const auto known = _store.chat(_chat);
  _dialog = GTK_WIDGET(adw_dialog_new());
  adw_dialog_set_content_width(ADW_DIALOG(_dialog), 420);
  adw_dialog_set_content_height(ADW_DIALOG(_dialog), 640);
  auto *view = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());

  _page = adw_preferences_page_new();
  auto *top = adw_preferences_group_new();
  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append(GTK_BOX(box), MakeAvatar(_store, _chat, 96, true));
  _name = gtk_label_new(nullptr);
  gtk_widget_add_css_class(_name, "title-2");
  gtk_label_set_wrap(GTK_LABEL(_name), TRUE);
  gtk_label_set_justify(GTK_LABEL(_name), GTK_JUSTIFY_CENTER);
  gtk_box_append(GTK_BOX(box), _name);
  _status = gtk_label_new(nullptr);
  gtk_widget_add_css_class(_status, "dim-label");
  gtk_box_append(GTK_BOX(box), _status);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(top), box);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(_page), ADW_PREFERENCES_GROUP(top));

  _info = adw_preferences_group_new();
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(_page), ADW_PREFERENCES_GROUP(_info));

  const auto self = known && known->kind == Td::ChatKind::Private && known->peerId == _store.selfId();
  const auto user = (known && known->kind == Td::ChatKind::Private) ? _store.user(known->peerId) : nullptr;
  auto *actions = adw_preferences_group_new();
  auto *send = adw_button_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(send), Tr("lng_profile_send_message").c_str());
  adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(send), "chat-message-new-symbolic");
  _signals.connect<void(AdwButtonRow *)>(send, "activated", [this](AdwButtonRow *) {
    const auto chat = _chat;
    auto &window = _window;
    adw_dialog_close(ADW_DIALOG(_dialog));
    window.openChat(chat);
  });
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(actions), send);
  if (user && !self && !user->bot && CallHandler()) {
    for (const auto video : {false, true}) {
      auto *call = adw_button_row_new();
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(call),
                                    Tr(video ? "lng_call_start_video" : "lng_call_start").c_str());
      adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(call), video ? "camera-video-symbolic" : "call-start-symbolic");
      const auto id = user->id;
      _signals.connect<void(AdwButtonRow *)>(call, "activated", [this, id, video](AdwButtonRow *) {
        adw_dialog_close(ADW_DIALOG(_dialog));
        if (CallHandler()) CallHandler()(id, video);
      });
      adw_preferences_group_add(ADW_PREFERENCES_GROUP(actions), call);
      _callRows.push_back(call);
    }
  }
  if (!self) {
    _mute = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_mute), Tr("lng_context_mute").c_str());
    _signals.connect<void(GObject *, GParamSpec *)>(_mute, "notify::active", [this](GObject *, GParamSpec *) {
      if (_muteChanging) return;
      const auto muted = adw_switch_row_get_active(ADW_SWITCH_ROW(_mute));
      _store.setMuted(_chat, muted, [](api::error &error) { ShowToast(error.message_); });
    });
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(actions), _mute);
  }
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(_page), ADW_PREFERENCES_GROUP(actions));

  // A group's members (a channel's subscribers aren't listed, as in the phone apps).
  if (known && known->kind == Td::ChatKind::Group) {
    _members = adw_preferences_group_new();
    _membersList = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(_membersList), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(_membersList, "boxed-list");
    _signals.connect<void(GtkListBox *, GtkListBoxRow *)>(
        _membersList, "row-activated", [this](GtkListBox *, GtkListBoxRow *row) {
          const auto user = Td::UserId(std::atoll(gtk_widget_get_name(GTK_WIDGET(row))));
          if (!user) return;
          auto &window = _window;
          WithPrivateChat(window, user, [&window](Td::ChatId chat) { ShowProfile(window, chat); });
        });
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(_members), _membersList);
    gtk_widget_set_visible(_members, FALSE);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(_page), ADW_PREFERENCES_GROUP(_members));
  }

  // Deleting the chat, or leaving the group, with the list's question.
  if (known && _window.chatList()) {
    auto *danger = adw_preferences_group_new();
    auto *remove = adw_button_row_new();
    const auto leave = !known->supergroup ? "lng_profile_delete_conversation"
                       : known->kind == Td::ChatKind::Channel ? "lng_profile_leave_channel"
                                                              : "lng_profile_leave_group";
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(remove), Tr(leave).c_str());
    adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(remove), "user-trash-symbolic");
    gtk_widget_add_css_class(remove, "destructive-action");
    _signals.connect<void(AdwButtonRow *)>(remove, "activated", [this](AdwButtonRow *) {
      const auto chat = _chat;
      auto &window = _window;
      adw_dialog_close(ADW_DIALOG(_dialog));
      if (window.chatList()) window.chatList()->askToDelete(chat);
    });
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(danger), remove);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(_page), ADW_PREFERENCES_GROUP(danger));
  }

  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), _page);
  adw_dialog_set_child(ADW_DIALOG(_dialog), view);
}

std::string ProfileCard::statusText() const {
  const auto known = _store.chat(_chat);
  if (!known) return {};
  if (known->kind == Td::ChatKind::Private || known->kind == Td::ChatKind::Secret) {
    if (known->peerId == _store.selfId()) return {};
    const auto user = _store.user(known->peerId);
    return !user ? std::string() : user->bot ? Tr("lng_status_bot") : UserStatusText(*user);
  }
  const auto group = _store.group(*known);
  const auto count = std::max(_memberCount, group ? group->members : 0);
  return TrCount(known->kind == Td::ChatKind::Channel ? "lng_chat_status_subscribers" : "lng_chat_status_members",
                 count);
}

void ProfileCard::refresh() {
  const auto known = _store.chat(_chat);
  if (!known) return;
  const auto self = known->kind == Td::ChatKind::Private && known->peerId == _store.selfId();
  gtk_label_set_text(GTK_LABEL(_name), self ? Tr("lng_saved_messages").c_str() : known->title.c_str());
  const auto status = statusText();
  gtk_label_set_text(GTK_LABEL(_status), status.c_str());
  gtk_widget_set_visible(_status, !status.empty());
  if (_mute && adw_switch_row_get_active(ADW_SWITCH_ROW(_mute)) != known->muted) {
    _muteChanging = true;
    adw_switch_row_set_active(ADW_SWITCH_ROW(_mute), known->muted);
    _muteChanging = false;
  }
  auto rows = std::vector<std::pair<std::string, std::string>>();
  const auto username = _store.username(*known);
  if (!_phone.empty()) rows.emplace_back(Tr("lng_info_mobile_label"), PhoneText(_phone));
  if (!username.empty()) {
    rows.emplace_back(Tr("lng_info_username_label"), "@" + username);
    rows.emplace_back(Tr("lng_info_link_label"), "https://t.me/" + username);
  }
  if (!_about.empty()) {
    rows.emplace_back(Tr(known->kind == Td::ChatKind::Private ? "lng_info_bio_label" : "lng_info_about_label"), _about);
  }
  setInfo(std::move(rows));
}

void ProfileCard::setInfo(std::vector<std::pair<std::string, std::string>> rows) {
  for (auto *row : _infoRows) adw_preferences_group_remove(ADW_PREFERENCES_GROUP(_info), row);
  _infoRows.clear();
  for (const auto &[title, value] : rows) {
    auto *row = InfoRow(title, value);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(_info), row);
    _infoRows.push_back(row);
  }
  gtk_widget_set_visible(_info, !_infoRows.empty());
}

// The phone, the bio or the description and the members come with the full info.
void ProfileCard::requestFull() {
  const auto known = _store.chat(_chat);
  if (!known) return;
  auto &client = _store.client();
  const auto ignore = [](api::error &) {};
  if (known->kind == Td::ChatKind::Private || known->kind == Td::ChatKind::Secret) {
    if (known->peerId == _store.selfId()) return;
    client.request<api::user>(api::make_object<api::getUser>(known->peerId), _lifetime, [this](api::user &user) {
      _phone = user.phone_number_;
      refresh();
    }, ignore);
    client.request<api::userFullInfo>(api::make_object<api::getUserFullInfo>(known->peerId), _lifetime,
                                      [this](api::userFullInfo &full) {
                                        _about = full.bio_ ? full.bio_->text_ : std::string();
                                        if (full.bot_info_ && _about.empty()) _about = full.bot_info_->short_description_;
                                        refresh();
                                      }, ignore);
    return;
  }
  const auto status = [](const api::ChatMemberStatus *status) {
    if (Cast<api::chatMemberStatusCreator>(status)) return Tr("lng_owner_badge");
    if (Cast<api::chatMemberStatusAdministrator>(status)) return Tr("lng_admin_badge");
    return std::string();
  };
  const auto members = [this, status](const std::vector<api::object_ptr<api::chatMember>> &list, int total) {
    auto result = std::vector<std::pair<Td::UserId, std::string>>();
    for (const auto &member : list) {
      const auto user = member ? Cast<api::messageSenderUser>(member->member_id_.get()) : nullptr;
      if (!user) continue;
      auto line = status(member->status_.get());
      if (line.empty()) {
        const auto known = _store.user(user->user_id_);
        line = known ? (known->bot ? Tr("lng_status_bot") : UserStatusText(*known)) : std::string();
      }
      result.emplace_back(user->user_id_, line);
      if (int(result.size()) >= kMembersShown) break;
    }
    showMembers(std::move(result), total);
  };
  if (known->supergroup) {
    client.request<api::supergroupFullInfo>(
        api::make_object<api::getSupergroupFullInfo>(known->peerId), _lifetime,
        [this](api::supergroupFullInfo &full) {
          _about = full.description_;
          _memberCount = full.member_count_;
          refresh();
        }, ignore);
    if (known->kind == Td::ChatKind::Channel) return;
    client.request<api::chatMembers>(
        api::make_object<api::getSupergroupMembers>(known->peerId, api::make_object<api::supergroupMembersFilterRecent>(),
                                                    0, kMembersShown),
        _lifetime, [members](api::chatMembers &result) { members(result.members_, result.total_count_); }, ignore);
  } else {
    client.request<api::basicGroupFullInfo>(
        api::make_object<api::getBasicGroupFullInfo>(known->peerId), _lifetime,
        [this, members](api::basicGroupFullInfo &full) {
          _about = full.description_;
          _memberCount = int(full.members_.size());
          refresh();
          members(full.members_, int(full.members_.size()));
        }, ignore);
  }
}

void ProfileCard::showMembers(std::vector<std::pair<Td::UserId, std::string>> members, int total) {
  if (!_members) return;
  gtk_list_box_remove_all(GTK_LIST_BOX(_membersList));
  for (const auto &[user, line] : members) {
    auto *row = PersonRow(_store, user, line);
    gtk_widget_set_name(row, std::to_string(user).c_str());
    gtk_list_box_append(GTK_LIST_BOX(_membersList), row);
  }
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(_members),
                                  TrCount("lng_chat_status_members", std::max(total, int(members.size()))).c_str());
  gtk_widget_set_visible(_members, !members.empty());
}

// profile-state | profile-member N (a tap on member N) | profile-mute | profile-call 0|1 (1: video)
void ProfileCard::debug(const std::string &name, const std::string &rest) {
  if (name == "profile-call") {
    const auto index = std::size_t(std::atoi(rest.c_str()));
    if (index < _callRows.size()) g_signal_emit_by_name(_callRows[index], "activated");
    return;
  }
  if (name == "profile-member" && _membersList) {
    if (auto *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(_membersList), std::atoi(rest.c_str()))) {
      g_signal_emit_by_name(_membersList, "row-activated", row);
    }
    return;
  } else if (name == "profile-mute" && _mute) {
    adw_switch_row_set_active(ADW_SWITCH_ROW(_mute), !adw_switch_row_get_active(ADW_SWITCH_ROW(_mute)));
  }
  auto info = std::string();
  for (auto *row : _infoRows) {
    info += std::string(" [") + adw_action_row_get_subtitle(ADW_ACTION_ROW(row)) + "="
            + adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row)) + "]";
  }
  auto members = std::string();
  if (_membersList && gtk_widget_get_visible(_members)) {
    for (auto *row = gtk_widget_get_first_child(_membersList); row; row = gtk_widget_get_next_sibling(row)) {
      members += std::string(" \"") + adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row)) + "\"";
    }
  }
  g_message("Profile: name=\"%s\" status=\"%s\" mute=%d calls=%d info:%s members:%s",
            gtk_label_get_text(GTK_LABEL(_name)), gtk_label_get_text(GTK_LABEL(_status)),
            _mute ? int(adw_switch_row_get_active(ADW_SWITCH_ROW(_mute))) : -1, int(_callRows.size()), info.c_str(),
            members.c_str());
}

// --- contacts ----------------------------------------------------------------

class ContactsPage final {
 public:
  explicit ContactsPage(MainWindow &window);

  [[nodiscard]] AdwNavigationPage *page() const { return _page; }
  void debug(const std::string &name, const std::string &rest);

 private:
  void fill();
  void searchServer();
  [[nodiscard]] bool matches(const Td::User &user) const;

  MainWindow &_window;
  Td::Store &_store;
  AdwNavigationPage *_page = nullptr;
  GtkWidget *_search = nullptr;
  GtkWidget *_list = nullptr;
  GtkWidget *_empty = nullptr;
  std::vector<Td::UserId> _contacts;
  std::vector<Td::UserId> _found;  // The server's answer for the query: contacts not loaded yet.
  std::string _query;
  bool _loaded = false;
  Td::Lifetime _lifetime;
  Td::Lifetime _searchLifetime;
  SignalConnections _signals;
};

ContactsPage::ContactsPage(MainWindow &window) : _window(window), _store(*window.store()) {
  auto *view = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());
  _search = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(_search), Tr("lng_dlg_filter").c_str());
  gtk_widget_set_hexpand(_search, TRUE);
  auto *searchClamp = adw_clamp_new();
  gtk_widget_set_margin_start(searchClamp, 12);
  gtk_widget_set_margin_end(searchClamp, 12);
  gtk_widget_set_margin_bottom(searchClamp, 6);
  adw_clamp_set_child(ADW_CLAMP(searchClamp), _search);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), searchClamp);
  _signals.connect<void(GtkSearchEntry *)>(_search, "search-changed", [this](GtkSearchEntry *) {
    _query = gtk_editable_get_text(GTK_EDITABLE(_search));
    _found.clear();
    fill();
    searchServer();
  });
  _list = gtk_list_box_new();
  gtk_widget_add_css_class(_list, "navigation-sidebar");
  _signals.connect<void(GtkListBox *, GtkListBoxRow *)>(_list, "row-activated", [this](GtkListBox *, GtkListBoxRow *row) {
    const auto user = Td::UserId(std::atoll(gtk_widget_get_name(GTK_WIDGET(row))));
    auto &window = _window;
    if (user) WithPrivateChat(window, user, [&window](Td::ChatId chat) { window.openChat(chat); });
  });
  _empty = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(_empty), "avatar-default-symbolic");
  auto *stack = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(stack), _list);
  gtk_box_append(GTK_BOX(stack), _empty);
  gtk_widget_set_vexpand(_empty, TRUE);
  auto *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), stack);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroll);
  _page = ADW_NAVIGATION_PAGE(adw_navigation_page_new(view, Tr("lng_contacts_header").c_str()));
  adw_navigation_page_set_tag(_page, "contacts");

  _store.client().request<api::users>(api::make_object<api::getContacts>(), _lifetime, [this](api::users &users) {
    _contacts = users.user_ids_;
    _loaded = true;
    fill();
  }, [this](api::error &error) {
    _loaded = true;
    ShowToast(error.message_);
    fill();
  });
  _store.onUser([this](Td::UserId id) {
    if (std::ranges::find(_contacts, id) != _contacts.end()) fill();
  }, _lifetime);
  fill();
}

bool ContactsPage::matches(const Td::User &user) const {
  if (_query.empty()) return true;
  auto *name = g_utf8_casefold(Td::DisplayName(user).c_str(), -1);
  auto *query = g_utf8_casefold(_query.c_str(), -1);
  const auto found = std::string(name).find(query) != std::string::npos
                     || user.username.find(_query) != std::string::npos;
  g_free(name);
  g_free(query);
  return found;
}

void ContactsPage::fill() {
  gtk_list_box_remove_all(GTK_LIST_BOX(_list));
  auto shown = std::vector<Td::UserId>();
  for (const auto id : _contacts) {
    const auto user = _store.user(id);
    if (user && matches(*user)) shown.push_back(id);
  }
  std::ranges::sort(shown, [this](Td::UserId a, Td::UserId b) {
    return Td::DisplayName(*_store.user(a)) < Td::DisplayName(*_store.user(b));
  });
  for (const auto id : _found) {
    if (std::ranges::find(shown, id) == shown.end() && _store.user(id)) shown.push_back(id);
  }
  for (const auto id : shown) {
    const auto user = _store.user(id);
    auto *row = PersonRow(_store, id, user->bot ? Tr("lng_status_bot") : UserStatusText(*user));
    gtk_widget_set_name(row, std::to_string(id).c_str());
    gtk_list_box_append(GTK_LIST_BOX(_list), row);
  }
  gtk_widget_set_visible(_list, !shown.empty());
  gtk_widget_set_visible(_empty, shown.empty());
  adw_status_page_set_title(ADW_STATUS_PAGE(_empty),
                            Tr(_loaded ? "lng_contacts_not_found" : "lng_contacts_loading").c_str());
}

void ContactsPage::searchServer() {
  _searchLifetime.reset();
  if (_query.empty()) return;
  _store.client().request<api::users>(api::make_object<api::searchContacts>(_query, 50), _searchLifetime,
                                      [this](api::users &users) {
                                        _found = users.user_ids_;
                                        fill();
                                      }, [](api::error &) {});
}

// contacts-state | contacts-search TEXT | contacts-open N
void ContactsPage::debug(const std::string &name, const std::string &rest) {
  if (name == "contacts-search") {
    gtk_editable_set_text(GTK_EDITABLE(_search), rest.c_str());
    return;
  } else if (name == "contacts-open") {
    if (auto *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(_list), std::atoi(rest.c_str()))) {
      g_signal_emit_by_name(_list, "row-activated", row);
    }
    return;
  }
  auto rows = std::string();
  for (auto *row = gtk_widget_get_first_child(_list); row; row = gtk_widget_get_next_sibling(row)) {
    rows += std::string(" \"") + adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row)) + "\"";
  }
  g_message("Contacts: loaded=%d query=\"%s\" empty=\"%s\" rows:%s", int(_loaded), _query.c_str(),
            gtk_widget_get_visible(_empty) ? adw_status_page_get_title(ADW_STATUS_PAGE(_empty)) : "", rows.c_str());
}

}  // namespace

std::string UserStatusText(const Td::User &user) {
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

void SetProfileCallHandler(std::function<void(Td::UserId, bool)> handler) {
  CallHandler() = std::move(handler);
}

void ShowProfile(MainWindow &window, Td::ChatId chat) {
  if (!window.widget() || !window.store() || !window.store()->chat(chat)) return;
  auto card = std::make_unique<ProfileCard>(window, chat);
  auto *dialog = card->dialog();
  AttachData(dialog, "contrail-profile", std::move(card));
  adw_dialog_present(ADW_DIALOG(dialog), window.widget());
  g_message("Profile: shown for %lld", (long long)chat);
}

void ShowAbout(MainWindow &window) {
  auto *about = adw_about_dialog_new();
  adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), "Contrail");
  adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about), kAppId);
  adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(about), "Vladislav Shadiuk");
  adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), kContrailVersion);
  adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(about),
                                "An unofficial Telegram client that uses the Telegram API, with a GNOME interface on "
                                "TDLib. Not affiliated with or endorsed by Telegram.");
  adw_about_dialog_set_website(ADW_ABOUT_DIALOG(about), kProjectUrl);
  adw_about_dialog_set_issue_url(ADW_ABOUT_DIALOG(about), (std::string(kProjectUrl) + "/issues").c_str());
  adw_about_dialog_set_copyright(ADW_ABOUT_DIALOG(about), "© 2026 Vladislav Shadiuk");
  // The same terms as Telegram Desktop, whose interface texts and parts the program keeps (see LICENSE and NOTICE).
  adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(about), GTK_LICENSE_CUSTOM);
  adw_about_dialog_set_license(
      ADW_ABOUT_DIALOG(about),
      "This program is distributed under the GNU General Public License, version 3, with the OpenSSL exception, "
      "the same license as Telegram Desktop. It comes with absolutely no warranty. See "
      "<a href=\"https://www.gnu.org/licenses/gpl-3.0.html\">the GNU General Public License</a> and the LICENSE "
      "and NOTICE files for details.");
  const char *tdlib[] = {"TDLib authors https://github.com/tdlib/td", nullptr};
  adw_about_dialog_add_credit_section(ADW_ABOUT_DIALOG(about), "TDLib", tdlib);
  const char *desktop[] = {"Telegram Desktop authors https://github.com/telegramdesktop/tdesktop", nullptr};
  adw_about_dialog_add_credit_section(ADW_ABOUT_DIALOG(about), "Telegram Desktop", desktop);
  adw_about_dialog_add_legal_section(ADW_ABOUT_DIALOG(about), "TDLib", "Copyright © 2014-2026 Aliaksei Levin, Arseny Smirnov",
                                     GTK_LICENSE_CUSTOM, "Boost Software License, version 1.0.");
  adw_about_dialog_add_legal_section(
      ADW_ABOUT_DIALOG(about), "Telegram Desktop", "Copyright © 2014-2026 The Telegram Desktop Authors",
      GTK_LICENSE_CUSTOM,
      "GNU General Public License, version 3, with a special exception to link with the OpenSSL library. "
      "Telegram is a trademark of Telegram FZ-LLC.");
  adw_dialog_present(ADW_DIALOG(about), window.widget());
  g_message("About: shown");
}

AdwNavigationPage *MakeContactsPage(MainWindow &window) {
  auto contacts = std::make_unique<ContactsPage>(window);
  auto *page = contacts->page();
  AttachData(page, "contrail-contacts", std::move(contacts));
  return page;
}

// The window's side of them, here with the pages it opens.
void MainWindow::openProfile(Td::ChatId chat) {
  ShowProfile(*this, chat);
}

void MainWindow::openContacts() {
  if (!_store || !_sidebarNav) return;
  adw_navigation_split_view_set_show_content(_split, FALSE);
  if (adw_navigation_view_find_page(_sidebarNav, "contacts")) {
    adw_navigation_view_pop_to_tag(_sidebarNav, "contacts");
    return;
  }
  adw_navigation_view_push(_sidebarNav, MakeContactsPage(*this));
}

void MainWindow::showAbout() {
  if (_window) ShowAbout(*this);
}

AdwNavigationPage *MainWindow::sidebarPage(const char *tag) const {
  return _sidebarNav ? adw_navigation_view_find_page(_sidebarNav, tag) : nullptr;
}

bool ProfileDebugCommand(MainWindow &window, const std::string &name, const std::string &rest) {
  if (name == "profile") {
    // "profile [CHAT_ID]": the card of the open chat, or of a chat by its id.
    const auto chat = rest.empty() ? window.currentChat() : Td::ChatId(std::atoll(rest.c_str()));
    if (chat) ShowProfile(window, chat);
    return true;
  } else if (name == "contacts") {
    window.openContacts();
    return true;
  } else if (name == "about") {
    ShowAbout(window);
    return true;
  }
  if (name.starts_with("profile-") && window.widget()) {
    auto *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window.widget()));
    const auto card = dialog ? static_cast<ProfileCard *>(g_object_get_data(G_OBJECT(dialog), "contrail-profile")) : nullptr;
    if (card) card->debug(name, rest);
    else g_message("Profile: none shown");
    return true;
  } else if (name.starts_with("contacts-")) {
    const auto page = window.sidebarPage("contacts");
    const auto contacts = page ? static_cast<ContactsPage *>(g_object_get_data(G_OBJECT(page), "contrail-contacts")) : nullptr;
    if (contacts) contacts->debug(name, rest);
    else g_message("Contacts: not shown");
    return true;
  }
  return false;
}

}  // namespace Contrail
