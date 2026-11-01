#pragma once

#include "app.hpp"
#include "td/client.hpp"

namespace Contrail {

// Sign-in: phone number -> code -> cloud password (if set) -> name (for a new account). TDLib leads: its
// authorization state decides the page, a page only sends the answer.
class LoginView final {
 public:
  explicit LoginView(Td::Session &session);
  ~LoginView();

  [[nodiscard]] GtkWidget *widget() const { return _root; }
  // A state that needs an answer from the user.
  void show(Td::api::AuthorizationState &state);
  // Testing: fills a step ("phone", "code", "password", "name First Last") and submits it.
  void debugSubmit(const std::string &step, const std::string &value);

 private:
  struct Page {
    AdwNavigationPage *page = nullptr;
    GtkWidget *title = nullptr;
    GtkWidget *description = nullptr;
    GtkWidget *error = nullptr;
    GtkWidget *button = nullptr;
  };

  Page makePage(const std::string &title, const std::string &description, GtkWidget *group,
                std::function<void()> submit);
  void setBusy(Page &page, bool busy);
  void showError(Page &page, const std::string &text);
  void showFailure(Page &page, Td::api::error &error);
  // Shows a page, leaving only the phone page under it, so that Back always leads there.
  void open(Page &page, GtkWidget *focus);
  void send(Page &page, Td::FunctionPtr request);

  void buildPhonePage();
  void buildCodePage();
  void buildEmailPage();
  void buildEmailCodePage();
  void buildPasswordPage();
  void buildSignupPage();
  void prefillCallingCode();

  void submitPhone();
  void submitCode();
  void submitEmail();
  void submitEmailCode();
  void submitPassword();
  void submitSignup();

  Td::Session &_session;
  Td::Lifetime _lifetime;

  GtkWidget *_root = nullptr;
  AdwNavigationView *_nav = nullptr;

  Page _phonePage;
  Page _codePage;
  Page _emailPage;
  Page _emailCodePage;
  Page _passwordPage;
  Page _signupPage;
  GtkWidget *_phone = nullptr;
  GtkWidget *_code = nullptr;
  GtkWidget *_email = nullptr;
  GtkWidget *_emailCode = nullptr;
  GtkWidget *_password = nullptr;
  GtkWidget *_firstName = nullptr;
  GtkWidget *_lastName = nullptr;
  bool _phoneEdited = false;
  bool _busy = false;
};

}  // namespace Contrail
