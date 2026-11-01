#include "login.hpp"

#include "td/login_errors.hpp"
#include "td/session.hpp"
#include "td/strings.hpp"

#include <algorithm>
#include <cctype>

namespace Contrail {
namespace {

namespace api = Td::api;

std::string DigitsOnly(const std::string &text) {
  auto result = std::string();
  std::ranges::copy_if(text, std::back_inserter(result), [](unsigned char c) { return std::isdigit(c); });
  return result;
}

std::string EntryText(GtkWidget *entry) {
  auto *stripped = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry))));
  auto result = std::string(stripped);
  g_free(stripped);
  return result;
}

// Telegram Desktop's descriptions bold parts with ** and break lines for its fixed-width intro.
std::string Plain(std::string text) {
  for (auto at = text.find("**"); at != std::string::npos; at = text.find("**", at)) text.erase(at, 2);
  std::ranges::replace(text, '\n', ' ');
  return text;
}

// The two-letter region of the system's locale, upper case; empty if it has none.
std::string SystemCountry() {
  for (auto name = g_get_language_names(); name && *name; ++name) {
    const auto text = std::string(*name);
    const auto underscore = text.find('_');
    if (underscore == std::string::npos || text.size() < underscore + 3) continue;
    auto country = text.substr(underscore + 1, 2);
    std::ranges::transform(country, country.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return country;
  }
  return {};
}

std::string CodeDescription(const api::AuthenticationCodeType &type) {
  return type.get_id() == api::authenticationCodeTypeTelegramMessage::ID ? Tr("lng_code_from_telegram")
                                                                         : Tr("lng_code_desc");
}

}  // namespace

LoginView::LoginView(Td::Session &session) : _session(session) {
  _nav = ADW_NAVIGATION_VIEW(adw_navigation_view_new());
  _root = GTK_WIDGET(_nav);
  g_object_ref_sink(_root);
  buildPhonePage();
  adw_navigation_view_add(_nav, _phonePage.page);
  prefillCallingCode();
}

LoginView::~LoginView() {
  for (auto *page : {_codePage.page, _emailPage.page, _emailCodePage.page, _passwordPage.page, _signupPage.page}) {
    if (page) g_object_unref(page);
  }
  g_object_unref(_root);
}

LoginView::Page LoginView::makePage(const std::string &title, const std::string &description, GtkWidget *group,
                                    std::function<void()> submit) {
  Page result;
  auto *view = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());

  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
  SetMarginAll(box, 18);

  result.title = gtk_label_new(title.c_str());
  gtk_widget_add_css_class(result.title, "title-1");
  gtk_label_set_wrap(GTK_LABEL(result.title), TRUE);
  gtk_label_set_justify(GTK_LABEL(result.title), GTK_JUSTIFY_CENTER);
  gtk_box_append(GTK_BOX(box), result.title);

  result.description = gtk_label_new(Plain(description).c_str());
  gtk_label_set_wrap(GTK_LABEL(result.description), TRUE);
  gtk_label_set_justify(GTK_LABEL(result.description), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class(result.description, "dim-label");
  gtk_box_append(GTK_BOX(box), result.description);

  gtk_box_append(GTK_BOX(box), group);

  result.error = gtk_label_new("");
  gtk_label_set_wrap(GTK_LABEL(result.error), TRUE);
  gtk_label_set_justify(GTK_LABEL(result.error), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class(result.error, "error");
  gtk_widget_set_visible(result.error, FALSE);
  gtk_box_append(GTK_BOX(box), result.error);

  auto *buttonBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  result.button = gtk_button_new();
  auto *buttonContent = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(buttonContent), gtk_label_new(Tr("lng_intro_next").c_str()), "label");
  gtk_stack_add_named(GTK_STACK(buttonContent), adw_spinner_new(), "spinner");
  gtk_button_set_child(GTK_BUTTON(result.button), buttonContent);
  gtk_widget_add_css_class(result.button, "pill");
  gtk_widget_add_css_class(result.button, "suggested-action");
  gtk_widget_set_halign(result.button, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(result.button, 200, -1);
  gtk_widget_set_hexpand(result.button, TRUE);
  gtk_box_append(GTK_BOX(buttonBox), result.button);
  gtk_box_append(GTK_BOX(box), buttonBox);
  OnClicked(result.button, std::move(submit));

  auto *clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 400);
  adw_clamp_set_child(ADW_CLAMP(clamp), box);
  auto *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroll);

  result.page = adw_navigation_page_new(view, title.c_str());
  return result;
}

void LoginView::setBusy(Page &page, bool busy) {
  _busy = busy;
  auto *stack = GTK_STACK(gtk_button_get_child(GTK_BUTTON(page.button)));
  gtk_stack_set_visible_child_name(stack, busy ? "spinner" : "label");
  gtk_widget_set_sensitive(page.button, !busy);
  if (busy) gtk_widget_set_visible(page.error, FALSE);
}

void LoginView::showError(Page &page, const std::string &text) {
  setBusy(page, false);
  gtk_label_set_text(GTK_LABEL(page.error), text.c_str());
  gtk_widget_set_visible(page.error, TRUE);
}

void LoginView::showFailure(Page &page, api::error &error) {
  switch (Td::ClassifyLoginError(error.code_, error.message_)) {
    case Td::LoginError::Flood: showError(page, Tr("lng_flood_error")); break;
    case Td::LoginError::BadPhone: showError(page, Tr("lng_bad_phone")); break;
    case Td::LoginError::BannedPhone: showError(page, Tr("lng_signin_banned_text")); break;
    case Td::LoginError::BadCode: showError(page, Tr("lng_bad_code")); break;
    case Td::LoginError::BadPassword: showError(page, Tr("lng_signin_bad_password")); break;
    case Td::LoginError::BadName: showError(page, Tr("lng_bad_name")); break;
    case Td::LoginError::ExpiredCode:
      // A new code starts from the phone number, as in Telegram Desktop.
      setBusy(page, false);
      adw_navigation_view_pop_to_page(_nav, _phonePage.page);
      showError(_phonePage, Tr("lng_bad_code"));
      break;
    case Td::LoginError::Other: showError(page, error.message_); break;
  }
}

void LoginView::open(Page &page, GtkWidget *focus) {
  setBusy(_phonePage, false);
  if (adw_navigation_view_get_visible_page(_nav) != page.page) {
    adw_navigation_view_pop_to_page(_nav, _phonePage.page);
    adw_navigation_view_push(_nav, page.page);
  }
  setBusy(page, false);
  gtk_widget_grab_focus(focus);
}

void LoginView::send(Page &page, Td::FunctionPtr request) {
  setBusy(page, true);
  // Success shows up as the next authorization state.
  _session.client().request<api::ok>(std::move(request), _lifetime, [](api::ok &) {},
                                     [this, &page](api::error &error) { showFailure(page, error); });
}

void LoginView::show(api::AuthorizationState &state) {
  switch (state.get_id()) {
    case api::authorizationStateWaitPhoneNumber::ID:
      adw_navigation_view_pop_to_page(_nav, _phonePage.page);
      setBusy(_phonePage, false);
      break;
    case api::authorizationStateWaitCode::ID: {
      const auto &info = *static_cast<api::authorizationStateWaitCode &>(state).code_info_;
      buildCodePage();
      gtk_label_set_text(GTK_LABEL(_codePage.title), ("+" + DigitsOnly(info.phone_number_)).c_str());
      gtk_label_set_text(GTK_LABEL(_codePage.description), Plain(CodeDescription(*info.type_)).c_str());
      gtk_editable_set_text(GTK_EDITABLE(_code), "");
      gtk_widget_set_visible(_codePage.error, FALSE);
      open(_codePage, _code);
      break;
    }
    case api::authorizationStateWaitEmailAddress::ID:
      buildEmailPage();
      open(_emailPage, _email);
      break;
    case api::authorizationStateWaitEmailCode::ID: {
      const auto &info = *static_cast<api::authorizationStateWaitEmailCode &>(state).code_info_;
      buildEmailCodePage();
      gtk_label_set_text(GTK_LABEL(_emailCodePage.description),
                         Plain(Tr("lng_intro_email_confirm_subtitle", {{"email", info.email_address_pattern_}})).c_str());
      open(_emailCodePage, _emailCode);
      break;
    }
    case api::authorizationStateWaitPassword::ID: {
      const auto &hint = static_cast<api::authorizationStateWaitPassword &>(state).password_hint_;
      buildPasswordPage();
      gtk_label_set_text(GTK_LABEL(_passwordPage.description),
                         hint.empty() ? Tr("lng_signin_desc").c_str()
                                      : Tr("lng_signin_hint", {{"password_hint", hint}}).c_str());
      gtk_editable_set_text(GTK_EDITABLE(_password), "");
      open(_passwordPage, _password);
      break;
    }
    case api::authorizationStateWaitRegistration::ID:
      buildSignupPage();
      open(_signupPage, _firstName);
      break;
    case api::authorizationStateWaitPremiumPurchase::ID:
      showError(_phonePage, Translated("Telegram requires a payment to sign in with this number.",
                                       "Telegram требует оплату для входа с этим номером.",
                                       "Telegram вимагає оплату для входу з цим номером."));
      break;
    default: break;
  }
}

// --- phone -------------------------------------------------------------------

void LoginView::buildPhonePage() {
  auto *group = adw_preferences_group_new();
  _phone = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_phone), Tr("lng_phone_title").c_str());
  adw_entry_row_set_input_purpose(ADW_ENTRY_ROW(_phone), GTK_INPUT_PURPOSE_PHONE);
  gtk_editable_set_text(GTK_EDITABLE(_phone), "+");
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), _phone);
  _phonePage = makePage(Tr("lng_phone_title"), Tr("lng_phone_desc"), group, [this] { submitPhone(); });
  Connect<void(AdwEntryRow *)>(_phone, "entry-activated", [this](AdwEntryRow *) { submitPhone(); });
  Connect<void(GtkEditable *)>(_phone, "changed", [this](GtkEditable *) { _phoneEdited = true; });
  _phoneEdited = false;
}

void LoginView::prefillCallingCode() {
  const auto country = SystemCountry();
  if (country.empty()) return;
  _session.client().request<api::countries>(
      api::make_object<api::getCountries>(), _lifetime,
      [this, country](api::countries &result) {
        if (_phoneEdited) return;
        for (const auto &item : result.countries_) {
          if (item->country_code_ == country && !item->calling_codes_.empty()) {
            gtk_editable_set_text(GTK_EDITABLE(_phone), ("+" + item->calling_codes_.front()).c_str());
            _phoneEdited = false;
            return;
          }
        }
      },
      [](api::error &) {});  // The field just stays with "+".
}

void LoginView::submitPhone() {
  if (_busy) return;
  const auto phone = DigitsOnly(EntryText(_phone));
  if (phone.size() < 5) {
    showError(_phonePage, Tr("lng_bad_phone"));
    return;
  }
  send(_phonePage, api::make_object<api::setAuthenticationPhoneNumber>("+" + phone, nullptr));
}

// --- code --------------------------------------------------------------------

void LoginView::buildCodePage() {
  if (_codePage.page) return;
  auto *group = adw_preferences_group_new();
  _code = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_code), Tr("lng_code_ph").c_str());
  adw_entry_row_set_input_purpose(ADW_ENTRY_ROW(_code), GTK_INPUT_PURPOSE_DIGITS);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), _code);
  _codePage = makePage("+", Tr("lng_code_desc"), group, [this] { submitCode(); });
  g_object_ref_sink(_codePage.page);
  Connect<void(AdwEntryRow *)>(_code, "entry-activated", [this](AdwEntryRow *) { submitCode(); });
}

void LoginView::submitCode() {
  if (_busy) return;
  const auto code = DigitsOnly(EntryText(_code));
  if (code.empty()) {
    showError(_codePage, Tr("lng_bad_code"));
    return;
  }
  send(_codePage, api::make_object<api::checkAuthenticationCode>(code));
}

// --- email (Telegram can ask for one during sign-in) ----------------------------

void LoginView::buildEmailPage() {
  if (_emailPage.page) return;
  auto *group = adw_preferences_group_new();
  _email = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_email), Tr("lng_intro_email_setup_title").c_str());
  adw_entry_row_set_input_purpose(ADW_ENTRY_ROW(_email), GTK_INPUT_PURPOSE_EMAIL);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), _email);
  _emailPage = makePage(Tr("lng_intro_email_setup_title"), "", group, [this] { submitEmail(); });
  g_object_ref_sink(_emailPage.page);
  Connect<void(AdwEntryRow *)>(_email, "entry-activated", [this](AdwEntryRow *) { submitEmail(); });
}

void LoginView::submitEmail() {
  if (_busy) return;
  const auto email = EntryText(_email);
  if (email.empty()) return;
  send(_emailPage, api::make_object<api::setAuthenticationEmailAddress>(email));
}

void LoginView::buildEmailCodePage() {
  if (_emailCodePage.page) return;
  auto *group = adw_preferences_group_new();
  _emailCode = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_emailCode), Tr("lng_code_ph").c_str());
  adw_entry_row_set_input_purpose(ADW_ENTRY_ROW(_emailCode), GTK_INPUT_PURPOSE_DIGITS);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), _emailCode);
  _emailCodePage = makePage(Tr("lng_intro_email_setup_title"), "", group, [this] { submitEmailCode(); });
  g_object_ref_sink(_emailCodePage.page);
  Connect<void(AdwEntryRow *)>(_emailCode, "entry-activated", [this](AdwEntryRow *) { submitEmailCode(); });
}

void LoginView::submitEmailCode() {
  if (_busy) return;
  const auto code = DigitsOnly(EntryText(_emailCode));
  if (code.empty()) {
    showError(_emailCodePage, Tr("lng_bad_code"));
    return;
  }
  send(_emailCodePage, api::make_object<api::checkAuthenticationEmailCode>(
                           api::make_object<api::emailAddressAuthenticationCode>(code)));
}

// --- cloud password ------------------------------------------------------------

void LoginView::buildPasswordPage() {
  if (_passwordPage.page) return;
  auto *group = adw_preferences_group_new();
  _password = adw_password_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_password), Tr("lng_signin_password").c_str());
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), _password);
  _passwordPage = makePage(Tr("lng_signin_title"), Tr("lng_signin_desc"), group, [this] { submitPassword(); });
  g_object_ref_sink(_passwordPage.page);
  Connect<void(AdwEntryRow *)>(_password, "entry-activated", [this](AdwEntryRow *) { submitPassword(); });
}

void LoginView::submitPassword() {
  if (_busy) return;
  // Not trimmed: spaces can be part of a password.
  const auto password = std::string(gtk_editable_get_text(GTK_EDITABLE(_password)));
  if (password.empty()) {
    showError(_passwordPage, Tr("lng_signin_bad_password"));
    return;
  }
  send(_passwordPage, api::make_object<api::checkAuthenticationPassword>(password));
}

// --- sign up (a number without an account) ----------------------------------

void LoginView::buildSignupPage() {
  if (_signupPage.page) return;
  auto *group = adw_preferences_group_new();
  _firstName = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_firstName), Tr("lng_signup_firstname").c_str());
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), _firstName);
  _lastName = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_lastName), Tr("lng_signup_lastname").c_str());
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), _lastName);
  _signupPage = makePage(Tr("lng_signup_title"), Tr("lng_signup_desc"), group, [this] { submitSignup(); });
  g_object_ref_sink(_signupPage.page);
}

void LoginView::submitSignup() {
  if (_busy) return;
  const auto first = EntryText(_firstName);
  if (first.empty()) {
    showError(_signupPage, Tr("lng_bad_name"));
    return;
  }
  send(_signupPage, api::make_object<api::registerUser>(first, EntryText(_lastName), false));
}

void LoginView::debugSubmit(const std::string &step, const std::string &value) {
  if (step == "phone") {
    gtk_editable_set_text(GTK_EDITABLE(_phone), value.c_str());
    submitPhone();
  } else if (step == "code" && _code) {
    gtk_editable_set_text(GTK_EDITABLE(_code), value.c_str());
    submitCode();
  } else if (step == "password" && _password) {
    gtk_editable_set_text(GTK_EDITABLE(_password), value.c_str());
    submitPassword();
  } else if (step == "name" && _firstName) {
    const auto space = value.find(' ');
    gtk_editable_set_text(GTK_EDITABLE(_firstName), value.substr(0, space).c_str());
    gtk_editable_set_text(GTK_EDITABLE(_lastName), space == std::string::npos ? "" : value.c_str() + space + 1);
    submitSignup();
  }
}

}  // namespace Contrail
