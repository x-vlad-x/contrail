#include "login.hpp"

#include "base/platform/base_platform_info.h"
#include "base/random.h"
#include "config.h"
#include "core/application.h"
#include "countries/countries_instance.h"
#include "data/data_chat_filters.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "lang/lang_cloud_manager.h"
#include "lang/lang_instance.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_app_config.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "mtproto/mtproto_config.h"
#include "storage/localstorage.h"
#include "storage/storage_account.h"

namespace Contrail {
namespace {

QString DigitsOnly(const QString &s) {
  QString result;
  for (const auto ch : s) {
    if (ch.isDigit()) {
      result.append(ch);
    }
  }
  return result;
}

QString EntryText(GtkWidget *entry) {
  return Q(gtk_editable_get_text(GTK_EDITABLE(entry))).trimmed();
}

QString StripMarkdown(QString text) {
  return text.replace(u"**"_q, QString());
}

}  // namespace

LoginView::LoginView(not_null<Main::Account *> account) : _account(account) {
  _account->mtpValue() | rpl::on_next([=](not_null<MTP::Instance *> instance) {
    _api.emplace(instance);
  }, _lifetime);

  _nav = ADW_NAVIGATION_VIEW(adw_navigation_view_new());
  _root = GTK_WIDGET(_nav);
  g_object_ref_sink(_root);
  buildPhonePage();
  adw_navigation_view_add(_nav, _phonePage.page);
}

LoginView::~LoginView() {
  if (_api && _request) {
    _api->request(base::take(_request)).cancel();
  }
  g_object_unref(_root);
}

LoginView::Page LoginView::makePage(const QString &title, const QString &description, GtkWidget *group,
                                    std::function<void()> submit) {
  Page result;
  auto *view = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());

  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
  SetMarginAll(box, 18);

  auto *titleLabel = gtk_label_new(U8(title).c_str());
  gtk_widget_add_css_class(titleLabel, "title-1");
  gtk_label_set_wrap(GTK_LABEL(titleLabel), TRUE);
  gtk_label_set_justify(GTK_LABEL(titleLabel), GTK_JUSTIFY_CENTER);
  gtk_box_append(GTK_BOX(box), titleLabel);

  result.description = gtk_label_new(U8(StripMarkdown(description).replace('\n', ' ')).c_str());
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
  gtk_stack_add_named(GTK_STACK(buttonContent), gtk_label_new(U8(tr::lng_intro_next(tr::now)).c_str()), "label");
  result.spinner = adw_spinner_new();
  gtk_stack_add_named(GTK_STACK(buttonContent), result.spinner, "spinner");
  gtk_button_set_child(GTK_BUTTON(result.button), buttonContent);
  gtk_widget_add_css_class(result.button, "pill");
  gtk_widget_add_css_class(result.button, "suggested-action");
  gtk_widget_set_halign(result.button, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(result.button, 200, -1);
  gtk_widget_set_hexpand(result.button, TRUE);
  gtk_box_append(GTK_BOX(buttonBox), result.button);
  gtk_box_append(GTK_BOX(box), buttonBox);
  OnClicked(result.button, submit);

  auto *clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 400);
  adw_clamp_set_child(ADW_CLAMP(clamp), box);
  auto *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroll);

  result.page = adw_navigation_page_new(view, U8(title).c_str());
  return result;
}

void LoginView::setBusy(Page &page, bool busy) {
  auto *stack = GTK_STACK(gtk_button_get_child(GTK_BUTTON(page.button)));
  gtk_stack_set_visible_child_name(stack, busy ? "spinner" : "label");
  gtk_widget_set_sensitive(page.button, !busy);
  if (busy) {
    gtk_widget_set_visible(page.error, FALSE);
  }
}

void LoginView::showError(Page &page, const QString &text) {
  setBusy(page, false);
  gtk_label_set_text(GTK_LABEL(page.error), U8(text).c_str());
  gtk_widget_set_visible(page.error, TRUE);
}

// --- phone -------------------------------------------------------------------

void LoginView::buildPhonePage() {
  auto *group = adw_preferences_group_new();
  _phone = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_phone), U8(tr::lng_phone_title(tr::now)).c_str());
  adw_entry_row_set_input_purpose(ADW_ENTRY_ROW(_phone), GTK_INPUT_PURPOSE_PHONE);
  adw_entry_row_set_activates_default(ADW_ENTRY_ROW(_phone), FALSE);
  // Start with the calling code of the system's country.
  auto code = QString();
  const auto &byIso = Countries::Instance().byISO2();
  if (const auto it = byIso.constFind(Platform::SystemCountry().toUpper()); it != byIso.cend()) {
    if (!(*it)->codes.empty()) {
      code = (*it)->codes.front().callingCode;
    }
  }
  gtk_editable_set_text(GTK_EDITABLE(_phone), ("+" + U8(code)).c_str());
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), _phone);
  _phonePage = makePage(tr::lng_phone_title(tr::now), tr::lng_phone_desc(tr::now), group, [this] { submitPhone(); });
  Connect<void(AdwEntryRow *)>(_phone, "entry-activated", [this](AdwEntryRow *) { submitPhone(); });
  adw_entry_row_set_activates_default(ADW_ENTRY_ROW(_phone), FALSE);
}

void LoginView::debugSubmit(const QString &step, const QString &value) {
  if (step == u"phone"_q) {
    gtk_editable_set_text(GTK_EDITABLE(_phone), U8(value).c_str());
    submitPhone();
  } else if (step == u"code"_q) {
    gtk_editable_set_text(GTK_EDITABLE(_code), U8(value).c_str());
    submitCode();
  } else if (step == u"name"_q) {
    const auto space = value.indexOf(' ');
    gtk_editable_set_text(GTK_EDITABLE(_firstName), U8(value.left(space)).c_str());
    gtk_editable_set_text(GTK_EDITABLE(_lastName), U8(space < 0 ? QString() : value.mid(space + 1)).c_str());
    submitSignup();
  }
}

void LoginView::submitPhone() {
  if (_request || !_api) {
    return;
  }
  const auto phone = DigitsOnly(EntryText(_phone));
  if (phone.size() < 5) {
    showError(_phonePage, tr::lng_bad_phone(tr::now));
    return;
  }
  // The same account is already signed in: just switch to it.
  for (const auto &[index, existing] : Core::App().domain().accounts()) {
    const auto raw = existing.get();
    if (const auto session = raw->maybeSession()) {
      if (raw->mtp().environment() == _account->mtp().environment()
          && DigitsOnly(session->user()->phone()) == phone) {
        crl::on_main(raw, [=] { Core::App().domain().activate(raw); });
        return;
      }
    }
  }
  setBusy(_phonePage, true);
  _sentPhone = phone;
  _api->instance().setUserPhone(_sentPhone);
  _request = _api->request(MTPauth_SendCode(
      MTP_string(_sentPhone), MTP_int(ApiId), MTP_string(ApiHash),
      MTP_codeSettings(MTP_flags(0), MTPVector<MTPbytes>(), MTPstring(), MTPBool())))
                 .done([=](const MTPauth_SentCode &result) {
                   _request = 0;
                   setBusy(_phonePage, false);
                   sentCode(result);
                 })
                 .fail([=](const MTP::Error &error) {
                   _request = 0;
                   const auto &type = error.type();
                   if (MTP::IsFloodError(error)) {
                     showError(_phonePage, tr::lng_flood_error(tr::now));
                   } else if (type == u"PHONE_NUMBER_INVALID"_q) {
                     showError(_phonePage, tr::lng_bad_phone(tr::now));
                   } else if (type == u"PHONE_NUMBER_BANNED"_q) {
                     showError(_phonePage, tr::lng_signin_banned_text(tr::now));
                   } else {
                     showError(_phonePage, type);
                   }
                 })
                 .handleFloodErrors()
                 .send();
}

void LoginView::sentCode(const MTPauth_SentCode &result) {
  result.match(
      [&](const MTPDauth_sentCode &data) {
        _phoneHash = qba(data.vphone_code_hash());
        auto description = tr::lng_code_desc(tr::now);
        auto setupEmail = false;
        data.vtype().match(
            [&](const MTPDauth_sentCodeTypeApp &) { description = tr::lng_code_from_telegram(tr::now); },
            [&](const MTPDauth_sentCodeTypeEmailCode &data) {
              description = tr::lng_intro_email_confirm_subtitle(tr::now, lt_email, qs(data.vemail_pattern()));
            },
            [&](const MTPDauth_sentCodeTypeSetUpEmailRequired &) { setupEmail = true; },
            [](const auto &) {});
        if (setupEmail) {
          // Email setup during sign-in isn't supported here: the official app does it.
          showError(_phonePage, tr::lng_intro_email_setup_title(tr::now));
          return;
        }
        buildCodePage();
        gtk_label_set_text(GTK_LABEL(_codePage.description), U8(StripMarkdown(description).replace('\n', ' ')).c_str());
        adw_navigation_view_push(_nav, _codePage.page);
        gtk_widget_grab_focus(_code);
      },
      [&](const MTPDauth_sentCodeSuccess &data) { finish(data.vauthorization()); },
      [&](const MTPDauth_sentCodePaymentRequired &) {
        showError(_phonePage, Q("Telegram requires a payment to sign in with this number."));
      });
}

// --- code --------------------------------------------------------------------

void LoginView::buildCodePage() {
  if (_codePage.page) {
    gtk_editable_set_text(GTK_EDITABLE(_code), "");
    gtk_widget_set_visible(_codePage.error, FALSE);
    return;
  }
  auto *group = adw_preferences_group_new();
  _code = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_code), U8(tr::lng_code_ph(tr::now)).c_str());
  adw_entry_row_set_input_purpose(ADW_ENTRY_ROW(_code), GTK_INPUT_PURPOSE_DIGITS);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), _code);
  _codePage = makePage(Q("+") + _sentPhone, tr::lng_code_desc(tr::now), group, [this] { submitCode(); });
  g_object_ref_sink(_codePage.page);
  Connect<void(AdwEntryRow *)>(_code, "entry-activated", [this](AdwEntryRow *) { submitCode(); });
}

void LoginView::submitCode() {
  if (_request || !_api) {
    return;
  }
  const auto code = DigitsOnly(EntryText(_code));
  if (code.isEmpty()) {
    showError(_codePage, tr::lng_bad_code(tr::now));
    return;
  }
  setBusy(_codePage, true);
  _request = _api->request(MTPauth_SignIn(MTP_flags(MTPauth_SignIn::Flag::f_phone_code), MTP_string(_sentPhone),
                                          MTP_bytes(_phoneHash), MTP_string(code), MTPEmailVerification()))
                 .done([=](const MTPauth_Authorization &result) {
                   _request = 0;
                   finish(result);
                 })
                 .fail([=](const MTP::Error &error) {
                   _request = 0;
                   const auto &type = error.type();
                   if (MTP::IsFloodError(error)) {
                     showError(_codePage, tr::lng_flood_error(tr::now));
                   } else if (type == u"PHONE_CODE_EMPTY"_q || type == u"PHONE_CODE_INVALID"_q) {
                     showError(_codePage, tr::lng_bad_code(tr::now));
                   } else if (type == u"SESSION_PASSWORD_NEEDED"_q) {
                     requestPassword();
                   } else if (type == u"PHONE_CODE_EXPIRED"_q || type == u"PHONE_NUMBER_INVALID"_q) {
                     setBusy(_codePage, false);
                     adw_navigation_view_pop(_nav);
                     showError(_phonePage, type == u"PHONE_CODE_EXPIRED"_q ? tr::lng_bad_code(tr::now)
                                                                         : tr::lng_bad_phone(tr::now));
                   } else {
                     showError(_codePage, type);
                   }
                 })
                 .handleFloodErrors()
                 .send();
}

// --- cloud password ----------------------------------------------------------

void LoginView::requestPassword() {
  _request = _api->request(MTPaccount_GetPassword())
                 .done([=](const MTPaccount_Password &result) {
                   _request = 0;
                   setBusy(_codePage, false);
                   result.match([&](const MTPDaccount_password &data) {
                     base::RandomAddSeed(bytes::make_span(data.vsecure_random().v));
                     _passwordState = Core::ParseCloudPasswordState(data);
                   });
                   buildPasswordPage();
                   adw_navigation_view_push(_nav, _passwordPage.page);
                   gtk_widget_grab_focus(_password);
                 })
                 .fail([=](const MTP::Error &error) {
                   _request = 0;
                   showError(_codePage, error.type());
                 })
                 .send();
}

void LoginView::buildPasswordPage() {
  auto description = tr::lng_signin_desc(tr::now);
  if (!_passwordState.hint.isEmpty()) {
    description += ' ' + tr::lng_signin_hint(tr::now, lt_password_hint, _passwordState.hint);
  }
  if (_passwordPage.page) {
    gtk_label_set_text(GTK_LABEL(_passwordPage.description), U8(description).c_str());
    gtk_editable_set_text(GTK_EDITABLE(_password), "");
    return;
  }
  auto *group = adw_preferences_group_new();
  _password = adw_password_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_password), U8(tr::lng_signin_password(tr::now)).c_str());
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), _password);
  _passwordPage = makePage(tr::lng_signin_title(tr::now), description, group, [this] { submitPassword(); });
  g_object_ref_sink(_passwordPage.page);
  Connect<void(AdwEntryRow *)>(_password, "entry-activated", [this](AdwEntryRow *) { submitPassword(); });
}

void LoginView::submitPassword() {
  if (_request || !_api) {
    return;
  }
  const auto password = Q(gtk_editable_get_text(GTK_EDITABLE(_password)));
  if (password.isEmpty()) {
    return;
  }
  if (!_passwordState.mtp.request) {
    showError(_passwordPage, Lang::Hard::ServerError());
    return;
  }
  setBusy(_passwordPage, true);
  const auto hash = Core::ComputeCloudPasswordHash(_passwordState.mtp.request.algo,
                                                   bytes::make_span(password.toUtf8()));
  const auto check = Core::ComputeCloudPasswordCheck(_passwordState.mtp.request, hash);
  if (!check) {
    showError(_passwordPage, Lang::Hard::ServerError());
    return;
  }
  _request = _api->request(MTPauth_CheckPassword(check.result))
                 .done([=](const MTPauth_Authorization &result) {
                   _request = 0;
                   finish(result);
                 })
                 .fail([=](const MTP::Error &error) {
                   _request = 0;
                   const auto &type = error.type();
                   if (MTP::IsFloodError(error)) {
                     showError(_passwordPage, tr::lng_flood_error(tr::now));
                   } else if (type == u"PASSWORD_HASH_INVALID"_q || type == u"SRP_PASSWORD_CHANGED"_q) {
                     showError(_passwordPage, tr::lng_signin_bad_password(tr::now));
                     // The SRP parameters are single-use: get fresh ones for the next try.
                     _api->request(MTPaccount_GetPassword())
                         .done([=](const MTPaccount_Password &result) {
                           result.match([&](const MTPDaccount_password &data) {
                             _passwordState = Core::ParseCloudPasswordState(data);
                           });
                         })
                         .send();
                   } else if (type == u"SRP_ID_INVALID"_q) {
                     showError(_passwordPage, Lang::Hard::ServerError());
                   } else {
                     showError(_passwordPage, type);
                   }
                 })
                 .handleFloodErrors()
                 .send();
}

// --- sign up (a number without an account) ----------------------------------

void LoginView::buildSignupPage() {
  if (_signupPage.page) {
    return;
  }
  auto *group = adw_preferences_group_new();
  _firstName = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_firstName), U8(tr::lng_signup_firstname(tr::now)).c_str());
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), _firstName);
  _lastName = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_lastName), U8(tr::lng_signup_lastname(tr::now)).c_str());
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), _lastName);
  _signupPage = makePage(tr::lng_signup_title(tr::now), tr::lng_signup_desc(tr::now), group, [this] { submitSignup(); });
  g_object_ref_sink(_signupPage.page);
}

void LoginView::submitSignup() {
  if (_request || !_api) {
    return;
  }
  const auto first = EntryText(_firstName);
  if (first.isEmpty()) {
    gtk_widget_grab_focus(_firstName);
    return;
  }
  setBusy(_signupPage, true);
  _request = _api->request(MTPauth_SignUp(MTP_flags(0), MTP_string(_sentPhone), MTP_bytes(_phoneHash),
                                          MTP_string(first), MTP_string(EntryText(_lastName))))
                 .done([=](const MTPauth_Authorization &result) {
                   _request = 0;
                   finish(result);
                 })
                 .fail([=](const MTP::Error &error) {
                   _request = 0;
                   showError(_signupPage,
                             MTP::IsFloodError(error) ? tr::lng_flood_error(tr::now) : error.type());
                 })
                 .handleFloodErrors()
                 .send();
}

// --- done --------------------------------------------------------------------

void LoginView::finish(const MTPauth_Authorization &auth) {
  auth.match([&](const MTPDauth_authorization &data) { finish(data.vuser()); },
             [&](const MTPDauth_authorizationSignUpRequired &) {
               buildSignupPage();
               adw_navigation_view_push(_nav, _signupPage.page);
               gtk_widget_grab_focus(_firstName);
             });
}

// Intro::details::Step::finish() and createSession().
void LoginView::finish(const MTPUser &user) {
  if (user.type() != mtpc_user || !user.c_user().is_self() || !user.c_user().vid().v) {
    ShowToast(Lang::Hard::ServerError());
    return;
  }
  // The same account is signed in already.
  for (const auto &[index, existing] : Core::App().domain().accounts()) {
    const auto raw = existing.get();
    if (const auto session = raw->maybeSession()) {
      if (raw->mtp().environment() == _account->mtp().environment()
          && UserId(user.c_user().vid()) == session->userId()) {
        _account->logOut();
        crl::on_main(raw, [=] {
          Core::App().domain().activate(raw);
          Local::sync();
        });
        return;
      }
    }
  }
  _api->request(MTPmessages_GetDialogFilters())
      .done([=](const MTPmessages_DialogFilters &result) {
        const auto &d = result.data();
        createSession(user, d.vfilters().v, d.is_tags_enabled());
      })
      .fail([=] { createSession(user, QVector<MTPDialogFilter>(), false); })
      .send();
}

void LoginView::createSession(const MTPUser &user, const QVector<MTPDialogFilter> &filters, bool tagsEnabled) {
  auto settings = std::make_unique<Main::SessionSettings>();
  const auto hasFilters = ranges::contains(filters, mtpc_dialogFilter, &MTPDialogFilter::type);
  settings->setDialogsFiltersEnabled(hasFilters);

  const auto account = _account;
  // Creating the session switches the main window to the chats and destroys this view.
  account->createSession(user, std::move(settings));

  account->local().enforceModernStorageIdBots();
  account->local().writeMtpData();
  auto &session = account->session();
  session.data().chatsFilters().setPreloaded(filters, tagsEnabled);
  if (hasFilters) {
    session.saveSettingsDelayed();
  }
  account->appConfig().refresh();
  Local::sync();
}

}  // namespace Contrail
