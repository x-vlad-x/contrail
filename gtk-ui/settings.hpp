#pragma once

#include "app.hpp"
#include "td/store.hpp"

#include <map>
#include <string>

namespace Contrail {

class MainWindow;

// Settings: the account, notifications, what closing the window does, about, logging out. The values are this
// device's (local_state), as Telegram Desktop kept them in its own settings for 0.3.1.
class SettingsView final {
 public:
  SettingsView(Td::Store &store, MainWindow &window);
  ~SettingsView();
  SettingsView(const SettingsView &) = delete;
  SettingsView &operator=(const SettingsView &) = delete;

  [[nodiscard]] AdwNavigationPage *page() const { return _page; }

  // "settings: notify=1 ..." into the log; a switch turned as a tap would.
  void debugState() const;
  void debugSet(const std::string &key, bool value);

 private:
  void buildAccount(GtkWidget *prefs);
  void buildNotifications(GtkWidget *prefs);
  void buildBackground(GtkWidget *prefs);
  void buildOther(GtkWidget *prefs);
  GtkWidget *addSwitch(GtkWidget *group, const char *key, const std::string &title, bool value);
  void switched(const std::string &key, bool value);
  void syncSensitive();

  Td::Store &_store;
  MainWindow &_window;
  AdwNavigationPage *_page = nullptr;
  GtkWidget *_account = nullptr;
  std::map<std::string, GtkWidget *> _switches;
  std::string _phone;
  Td::Lifetime _lifetime;
};

// "Are you sure you want to log out?", then Telegram ends this session.
void ConfirmLogOut(GtkWidget *parent);

}  // namespace Contrail
