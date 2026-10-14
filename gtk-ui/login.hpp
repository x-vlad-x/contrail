#pragma once

#include "app.hpp"

#include "core/core_cloud_password.h"
#include "mtproto/sender.h"

#include <rpl/lifetime.h>

namespace Main {
class Account;
} // namespace Main

namespace Contrail {

// Sign-in: phone number -> code -> cloud password (if set) -> name (for a new account).
// Follows Telegram Desktop's intro steps (Telegram/SourceFiles/intro/), finishing the same way:
// the account gets a session and the main window switches to the chats.
class LoginView final {
public:
  explicit LoginView(not_null<Main::Account *> account);
  ~LoginView();

  [[nodiscard]] GtkWidget *widget() const { return _root; }
  // Testing: fills a step ("phone", "code", "name First Last") and submits it.
  void debugSubmit(const QString &step, const QString &value);

private:
  struct Page {
    AdwNavigationPage *page = nullptr;
    GtkWidget *description = nullptr;
    GtkWidget *error = nullptr;
    GtkWidget *button = nullptr;
    GtkWidget *spinner = nullptr;
  };

  Page makePage(const QString &title, const QString &description, GtkWidget *group, std::function<void()> submit);
  void setBusy(Page &page, bool busy);
  void showError(Page &page, const QString &text);

  void buildPhonePage();
  void buildCodePage();
  void buildPasswordPage();
  void buildSignupPage();

  void submitPhone();
  void submitCode();
  void submitPassword();
  void submitSignup();

  void sentCode(const MTPauth_SentCode &result);
  void requestPassword();
  void finish(const MTPauth_Authorization &auth);
  void finish(const MTPUser &user);
  void createSession(const MTPUser &user, const QVector<MTPDialogFilter> &filters, bool tagsEnabled);

  const not_null<Main::Account *> _account;
  std::optional<MTP::Sender> _api;
  mtpRequestId _request = 0;

  GtkWidget *_root = nullptr;
  AdwNavigationView *_nav = nullptr;

  Page _phonePage;
  Page _codePage;
  Page _passwordPage;
  Page _signupPage;
  GtkWidget *_phone = nullptr;
  GtkWidget *_code = nullptr;
  GtkWidget *_password = nullptr;
  GtkWidget *_firstName = nullptr;
  GtkWidget *_lastName = nullptr;

  QString _sentPhone;
  QByteArray _phoneHash;
  Core::CloudPasswordState _passwordState;
  rpl::lifetime _lifetime;
};

}  // namespace Contrail
