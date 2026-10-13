#pragma once

#include "app.hpp"

#include <rpl/lifetime.h>

namespace Main {
class Session;
} // namespace Main

namespace Contrail {

class MainWindow;

// Settings: the account, notifications, logging out. Telegram Desktop keeps the values.
class SettingsView final {
public:
  SettingsView(not_null<Main::Session *> session, not_null<MainWindow *> window);
  ~SettingsView();

  [[nodiscard]] AdwNavigationPage *page() const { return _page; }

private:
  const not_null<Main::Session *> _session;
  const not_null<MainWindow *> _window;
  AdwNavigationPage *_page = nullptr;
  rpl::lifetime _lifetime;
};

}  // namespace Contrail
