#include "settings.hpp"

#include "contrail_version.h"
#include "local_state.hpp"
#include "main_window.hpp"
#include "notifications.hpp"
#include "td/session.hpp"
#include "td/strings.hpp"
#include "userpics.hpp"

namespace Contrail {
namespace {

namespace api = Td::api;

[[nodiscard]] bool *SettingField(AppSettings &settings, const std::string &key) {
  if (key == "notify") return &settings.notify;
  if (key == "sound") return &settings.sound;
  if (key == "preview") return &settings.preview;
  if (key == "background") return &settings.background;
  return nullptr;
}

[[nodiscard]] std::string AccountSubtitle(const std::string &phone, const std::string &username) {
  auto result = phone.empty() ? std::string() : "+" + phone;
  if (!username.empty()) result += (result.empty() ? "@" : "  •  @") + username;
  return result;
}

}  // namespace

SettingsView::SettingsView(Td::Store &store, MainWindow &window) : _store(store), _window(window) {
  auto *view = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());
  auto *prefs = adw_preferences_page_new();
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), prefs);
  buildAccount(prefs);
  buildNotifications(prefs);
  buildBackground(prefs);
  buildOther(prefs);
  syncSensitive();

  _page = adw_navigation_page_new(view, Tr("lng_menu_settings").c_str());
  adw_navigation_page_set_tag(_page, "settings");
  g_object_ref_sink(_page);
}

SettingsView::~SettingsView() {
  g_object_unref(_page);
}

void SettingsView::buildAccount(GtkWidget *prefs) {
  auto *group = adw_preferences_group_new();
  const auto self = _store.selfId();
  _account = adw_action_row_new();
  const auto *user = _store.user(self);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_account), user ? Escape(Td::DisplayName(*user)).c_str() : "");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(_account), Escape(AccountSubtitle({}, user ? user->username : "")).c_str());
  adw_action_row_add_prefix(ADW_ACTION_ROW(_account), MakeUserAvatar(_store, self, 48));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), _account);
  // The store keeps no phone numbers: only this row shows one.
  _store.client().request<api::user>(api::make_object<api::getMe>(), _lifetime, [this](api::user &me) {
    _phone = me.phone_number_;
    const auto username = me.usernames_ && !me.usernames_->active_usernames_.empty()
        ? me.usernames_->active_usernames_.front() : std::string();
    adw_action_row_set_subtitle(ADW_ACTION_ROW(_account), Escape(AccountSubtitle(_phone, username)).c_str());
  }, [](api::error &) {});

  auto *saved = adw_button_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(saved), Tr("lng_saved_messages").c_str());
  adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(saved), "user-bookmarks-symbolic");
  Connect<void(AdwButtonRow *)>(saved, "activated", [this](AdwButtonRow *) {
    _store.client().request<api::chat>(api::make_object<api::createPrivateChat>(_store.selfId(), false), _lifetime,
                                       [this](api::chat &chat) { _window.openChat(chat.id_); },
                                       [](api::error &error) { ShowToast(error.message_); });
  });
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), saved);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(prefs), ADW_PREFERENCES_GROUP(group));
}

void SettingsView::buildNotifications(GtkWidget *prefs) {
  const auto &settings = Settings();
  auto *group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), Tr("lng_settings_section_notify").c_str());
  addSwitch(group, "notify", Tr("lng_settings_desktop_notify"), settings.notify);
  addSwitch(group, "sound", Tr("lng_settings_sound_allowed"), settings.sound);
  addSwitch(group, "preview", Translated("Show message text", "Показывать текст сообщения",
                                         "Показувати текст повідомлення"),
            settings.preview);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(prefs), ADW_PREFERENCES_GROUP(group));
}

// When the window is closed: quit, or keep receiving messages and calls (with a tray icon on a desktop).
void SettingsView::buildBackground(GtkWidget *prefs) {
  auto *group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), Tr("lng_settings_window_close").c_str());
  addSwitch(group, "background", Tr("lng_settings_run_in_background"), Settings().background);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(prefs), ADW_PREFERENCES_GROUP(group));

  auto *network = adw_preferences_group_new();
  auto *proxy = adw_button_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(proxy), Tr("lng_proxy_settings").c_str());
  adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(proxy), "network-server-symbolic");
  Connect<void(AdwButtonRow *)>(proxy, "activated", [this](AdwButtonRow *) { _window.openProxy(); });
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(network), proxy);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(prefs), ADW_PREFERENCES_GROUP(network));
}

void SettingsView::buildOther(GtkWidget *prefs) {
  auto *group = adw_preferences_group_new();
  auto *about = adw_button_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(about), (Tr("lng_menu_about") + " Contrail").c_str());
  adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(about), "help-about-symbolic");
  Connect<void(AdwButtonRow *)>(about, "activated", [this](AdwButtonRow *) { ShowAbout(_window.widget()); });
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), about);
  auto *logout = adw_button_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(logout), Tr("lng_settings_logout").c_str());
  adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(logout), "system-log-out-symbolic");
  gtk_widget_add_css_class(logout, "destructive-action");
  Connect<void(AdwButtonRow *)>(logout, "activated", [this](AdwButtonRow *) { ConfirmLogOut(_window.widget()); });
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), logout);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(prefs), ADW_PREFERENCES_GROUP(group));
}

GtkWidget *SettingsView::addSwitch(GtkWidget *group, const char *key, const std::string &title, bool value) {
  auto *row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title.c_str());
  adw_switch_row_set_active(ADW_SWITCH_ROW(row), value);
  Connect<void(GObject *, GParamSpec *)>(row, "notify::active", [this, key = std::string(key)](GObject *row, GParamSpec *) {
    switched(key, adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
  });
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
  _switches[key] = row;
  return row;
}

void SettingsView::switched(const std::string &key, bool value) {
  auto settings = Settings();
  auto *field = SettingField(settings, key);
  if (!field || *field == value) return;
  *field = value;
  SetSettings(settings);
  syncSensitive();
  if (key == "background") _window.backgroundChanged();
  if (key == "notify" && _window.notifier()) _window.notifier()->settingsChanged();
}

// Sound and the text of a message mean nothing without notifications.
void SettingsView::syncSensitive() {
  const auto notify = Settings().notify;
  for (const auto *key : {"sound", "preview"}) {
    if (const auto it = _switches.find(key); it != _switches.end()) gtk_widget_set_sensitive(it->second, notify);
  }
}

void SettingsView::debugState() const {
  const auto &settings = Settings();
  g_message("settings: notify=%d sound=%d preview=%d background=%d account=\"%s\" phone=%s", int(settings.notify),
            int(settings.sound), int(settings.preview), int(settings.background),
            _account ? adw_preferences_row_get_title(ADW_PREFERENCES_ROW(_account)) : "",
            _phone.empty() ? "-" : _phone.c_str());
}

void SettingsView::debugSet(const std::string &key, bool value) {
  const auto it = _switches.find(key);
  if (it == _switches.end()) {
    g_message("settings: no switch %s", key.c_str());
    return;
  }
  adw_switch_row_set_active(ADW_SWITCH_ROW(it->second), value);
}

void ShowAbout(GtkWidget *parent) {
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
  // The same terms as before the move to TDLib (see LICENSE and NOTICE).
  adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(about), GTK_LICENSE_CUSTOM);
  adw_about_dialog_set_license(
      ADW_ABOUT_DIALOG(about),
      "This program is distributed under the GNU General Public License, version 3, with the OpenSSL exception. "
      "It comes with absolutely no warranty. See "
      "<a href=\"https://www.gnu.org/licenses/gpl-3.0.html\">the GNU General Public License</a> and the LICENSE "
      "and NOTICE files for details.");
  const char *tdlib[] = {"TDLib authors https://github.com/tdlib/td", nullptr};
  adw_about_dialog_add_credit_section(ADW_ABOUT_DIALOG(about), "TDLib", tdlib);
  adw_about_dialog_add_legal_section(ADW_ABOUT_DIALOG(about), "TDLib", "Copyright © Aliaksei Levin, Arseny Smirnov",
                                     GTK_LICENSE_CUSTOM,
                                     "Boost Software License, version 1.0. Telegram is a trademark of Telegram FZ-LLC.");
  adw_dialog_present(ADW_DIALOG(about), parent);
}

void ConfirmLogOut(GtkWidget *parent) {
  auto *dialog = adw_alert_dialog_new(Tr("lng_sure_logout").c_str(), nullptr);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", Tr("lng_cancel").c_str(), "logout",
                                 Tr("lng_settings_logout").c_str(), nullptr);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "logout", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  Connect<void(AdwAlertDialog *, const char *)>(dialog, "response", [](AdwAlertDialog *, const char *response) {
    if (g_strcmp0(response, "logout") == 0 && GetApp().session) GetApp().session->logOut();
  });
  adw_dialog_present(ADW_DIALOG(dialog), parent);
}

}  // namespace Contrail
