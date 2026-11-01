#pragma once

#include "app.hpp"
#include "td/client.hpp"

#include <memory>
#include <string>

namespace Contrail {

class LoginView;

// The application's window: a spinner while TDLib starts, the login, then the chats.
class MainWindow final {
 public:
  explicit MainWindow(GtkApplication *application);
  ~MainWindow();

  [[nodiscard]] GtkWidget *widget() const { return _window; }
  void present();
  void showToast(const std::string &text);

  // Follows a new TDLib client: a lost session starts one with an empty database.
  void attach(Td::Session &session, const std::string &language);
  // Telegram ended the session: says why instead of just showing the login again.
  void showSessionLost();

  void debugLogin(const std::string &step, const std::string &value);

 private:
  void authorization(Td::api::AuthorizationState &state);
  void stringsReady();
  void showLogin(Td::api::AuthorizationState &state);
  void showChats();
  // A new login dies within minutes if nobody confirms it on another device.
  void warnAboutConfirmation();

  GtkWidget *_window = nullptr;
  AdwToastOverlay *_toasts = nullptr;
  GtkStack *_stack = nullptr;
  Td::Session *_session = nullptr;
  Td::Lifetime _sessionLifetime;
  std::string _language;
  guint _stringsTimeout = 0;
  bool _stringsAsked = false;
  bool _stringsReady = false;  // Pages are built with their text: nothing is shown before the strings.
  std::unique_ptr<LoginView> _login;
  bool _loginShown = false;  // This client went through the login: it isn't confirmed yet.
};

}  // namespace Contrail
