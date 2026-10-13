#include "settings.hpp"

#include "main_window.hpp"
#include "userpics.hpp"

#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "ui/text/format_values.h"

namespace Contrail {
namespace {

GtkWidget *SwitchRow(const QString &title, bool value, std::function<void(bool)> changed) {
  auto *row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), U8(title).c_str());
  adw_switch_row_set_active(ADW_SWITCH_ROW(row), value);
  Connect<void(GObject *, GParamSpec *)>(row, "notify::active", [changed](GObject *row, GParamSpec *) {
    changed(adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
  });
  return row;
}

}  // namespace

SettingsView::SettingsView(not_null<Main::Session *> session, not_null<MainWindow *> window)
    : _session(session), _window(window) {
  auto *view = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());
  auto *prefs = adw_preferences_page_new();
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), prefs);

  // Account.
  const auto user = _session->user();
  auto *account = adw_preferences_group_new();
  auto *me = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(me), U8(user->name()).c_str());
  auto subtitle = Ui::FormatPhone(user->phone());
  if (!user->username().isEmpty()) {
    subtitle += u"  •  @"_q + user->username();
  }
  adw_action_row_set_subtitle(ADW_ACTION_ROW(me), U8(subtitle).c_str());
  adw_action_row_add_prefix(ADW_ACTION_ROW(me), MakeAvatar(user, 48));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(account), me);
  auto *saved = adw_button_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(saved), U8(tr::lng_saved_messages(tr::now)).c_str());
  adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(saved), "user-bookmarks-symbolic");
  Connect<void(AdwButtonRow *)>(saved, "activated", [this](AdwButtonRow *) { _window->openPeer(_session->user()); });
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(account), saved);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(prefs), ADW_PREFERENCES_GROUP(account));

  // Notifications.
  auto &settings = Core::App().settings();
  auto *notify = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(notify), U8(tr::lng_settings_section_notify(tr::now)).c_str());
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(notify),
                            SwitchRow(tr::lng_settings_desktop_notify(tr::now), settings.desktopNotify(), [](bool value) {
                              Core::App().settings().setDesktopNotify(value);
                              Core::App().saveSettingsDelayed();
                            }));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(notify),
                            SwitchRow(tr::lng_settings_sound_allowed(tr::now), settings.soundNotify(), [](bool value) {
                              Core::App().settings().setSoundNotify(value);
                              Core::App().saveSettingsDelayed();
                            }));
  using NotifyView = Core::Settings::NotifyView;
  adw_preferences_group_add(
      ADW_PREFERENCES_GROUP(notify),
      SwitchRow(tr::lng_notification_preview(tr::now), settings.notifyView() == NotifyView::ShowPreview, [](bool value) {
        Core::App().settings().setNotifyView(value ? NotifyView::ShowPreview : NotifyView::ShowName);
        Core::App().saveSettingsDelayed();
      }));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(prefs), ADW_PREFERENCES_GROUP(notify));

  // About and logging out.
  auto *other = adw_preferences_group_new();
  auto *about = adw_button_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(about), U8(tr::lng_menu_about(tr::now) + u" Contrail"_q).c_str());
  adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(about), "help-about-symbolic");
  Connect<void(AdwButtonRow *)>(about, "activated", [this](AdwButtonRow *) { _window->showAbout(); });
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(other), about);
  auto *logout = adw_button_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(logout), U8(tr::lng_settings_logout(tr::now)).c_str());
  adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(logout), "system-log-out-symbolic");
  gtk_widget_add_css_class(logout, "destructive-action");
  Connect<void(AdwButtonRow *)>(logout, "activated", [this](AdwButtonRow *) {
    auto *dialog = adw_alert_dialog_new(U8(tr::lng_sure_logout(tr::now)).c_str(), nullptr);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", U8(tr::lng_cancel(tr::now)).c_str(), "logout",
                                   U8(tr::lng_settings_logout(tr::now)).c_str(), nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "logout", ADW_RESPONSE_DESTRUCTIVE);
    const auto account = &_session->account();
    Connect<void(AdwAlertDialog *, const char *)>(dialog, "response", [account](AdwAlertDialog *, const char *response) {
      if (g_strcmp0(response, "logout") == 0) {
        Core::App().logout(account);
      }
    });
    adw_dialog_present(ADW_DIALOG(dialog), _window->widget());
  });
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(other), logout);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(prefs), ADW_PREFERENCES_GROUP(other));

  _page = adw_navigation_page_new(view, U8(tr::lng_menu_settings(tr::now)).c_str());
  adw_navigation_page_set_tag(_page, "settings");
  g_object_ref_sink(_page);
}

SettingsView::~SettingsView() {
  g_object_unref(_page);
}

}  // namespace Contrail
