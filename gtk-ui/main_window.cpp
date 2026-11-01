#include "main_window.hpp"

#include "login.hpp"
#include "td/session.hpp"
#include "td/strings.hpp"

namespace Contrail {
namespace {

namespace api = Td::api;

// The first start without a network gets no language pack: the built-in English comes up after this long.
constexpr auto kStringsTimeoutMs = 4000;

GtkWidget *MakeLoadingPage() {
  auto *page = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(page), adw_header_bar_new());
  auto *spinner = adw_spinner_new();
  gtk_widget_set_size_request(spinner, 48, 48);
  gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(page), spinner);
  return page;
}

void AlertInWindow(GtkWidget *window, const std::string &heading, const std::string &body) {
  auto *dialog = adw_alert_dialog_new(heading.c_str(), body.c_str());
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", Tr("lng_box_ok").c_str());
  adw_dialog_present(ADW_DIALOG(dialog), window);
}

}  // namespace

MainWindow::MainWindow(GtkApplication *application) {
  _window = adw_application_window_new(application);
  gtk_window_set_title(GTK_WINDOW(_window), "Contrail TD");
  gtk_window_set_default_size(GTK_WINDOW(_window), 900, 700);
  gtk_widget_set_size_request(_window, 320, 294);

  _toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
  _stack = GTK_STACK(gtk_stack_new());
  gtk_stack_set_transition_type(_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  adw_toast_overlay_set_child(_toasts, GTK_WIDGET(_stack));
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(_window), GTK_WIDGET(_toasts));
  gtk_stack_add_named(_stack, MakeLoadingPage(), "loading");
}

MainWindow::~MainWindow() {
  if (_stringsTimeout) g_source_remove(_stringsTimeout);
}

void MainWindow::present() {
  gtk_window_present(GTK_WINDOW(_window));
}

void MainWindow::showToast(const std::string &text) {
  adw_toast_overlay_add_toast(_toasts, adw_toast_new(text.c_str()));
}

void MainWindow::attach(Td::Session &session, const std::string &language) {
  _session = &session;
  _sessionLifetime.reset();
  _language = language;
  _stringsAsked = false;
  _stringsReady = false;
  _loginShown = false;
  _login = nullptr;
  gtk_stack_set_visible_child_name(_stack, "loading");
  session.setAuthorizationHandler([this](api::AuthorizationState &state) { authorization(state); });
}

void MainWindow::stringsReady() {
  if (_stringsTimeout) {
    g_source_remove(_stringsTimeout);
    _stringsTimeout = 0;
  }
  if (_stringsReady) return;
  _stringsReady = true;
  // The state that came in the meantime waited for this.
  _session->client().send(api::make_object<api::getAuthorizationState>(),
                          [this](Td::ObjectPtr object) {
                            if (object && object->get_id() != api::error::ID) {
                              authorization(static_cast<api::AuthorizationState &>(*object));
                            }
                          },
                          _sessionLifetime);
}

void MainWindow::authorization(api::AuthorizationState &state) {
  // The first state after the parameters: TDLib knows the language pack's options now.
  if (!_stringsAsked) {
    _stringsAsked = true;
    LoadStrings(_session->client(), _sessionLifetime, _language, [this] { stringsReady(); });
    if (!_stringsReady) {
      _stringsTimeout = g_timeout_add_once(kStringsTimeoutMs, +[](gpointer data) {
        auto *window = static_cast<MainWindow *>(data);
        window->_stringsTimeout = 0;
        window->stringsReady();
      }, this);
    }
  }
  if (!_stringsReady) return;
  switch (state.get_id()) {
    case api::authorizationStateWaitPhoneNumber::ID:
    case api::authorizationStateWaitCode::ID:
    case api::authorizationStateWaitEmailAddress::ID:
    case api::authorizationStateWaitEmailCode::ID:
    case api::authorizationStateWaitPassword::ID:
    case api::authorizationStateWaitRegistration::ID:
    case api::authorizationStateWaitPremiumPurchase::ID:
      showLogin(state);
      break;
    case api::authorizationStateReady::ID:
      showChats();
      break;
    case api::authorizationStateLoggingOut::ID:
    case api::authorizationStateClosing::ID:
    case api::authorizationStateClosed::ID:
      gtk_stack_set_visible_child_name(_stack, "loading");
      break;
    default: break;
  }
}

void MainWindow::showLogin(api::AuthorizationState &state) {
  if (!_login) {
    _login = std::make_unique<LoginView>(*_session);
    if (auto *old = gtk_stack_get_child_by_name(_stack, "login")) gtk_stack_remove(_stack, old);
    gtk_stack_add_named(_stack, _login->widget(), "login");
  }
  _loginShown = true;
  _login->show(state);
  gtk_stack_set_visible_child_name(_stack, "login");
}

void MainWindow::showChats() {
  if (!gtk_stack_get_child_by_name(_stack, "chats")) {
    auto *page = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(page), adw_header_bar_new());
    gtk_stack_add_named(_stack, page, "chats");
  }
  gtk_stack_set_visible_child_name(_stack, "chats");
  if (_login) {
    gtk_stack_remove(_stack, _login->widget());
    _login = nullptr;
  }
  if (_loginShown) {
    _loginShown = false;
    warnAboutConfirmation();
  }
}

void MainWindow::warnAboutConfirmation() {
  AlertInWindow(_window,
                Translated("Confirm this login", "Подтвердите вход", "Підтвердьте вхід"),
                Translated("Telegram may ask “Is it you?” in the app on your other device. Answer right away: "
                           "an unconfirmed login is closed within minutes.",
                           "На другом вашем устройстве Telegram может спросить «Это вы?». Ответьте сразу: "
                           "неподтверждённый вход закрывается уже через несколько минут.",
                           "На іншому вашому пристрої Telegram може запитати «Це ви?». Дайте відповідь одразу: "
                           "непідтверджений вхід закривається вже за кілька хвилин."));
}

void MainWindow::showSessionLost() {
  AlertInWindow(_window,
                Translated("You were logged out", "Вы вышли из аккаунта", "Ви вийшли з акаунта"),
                Translated("Telegram ended this session and its data on this device was deleted. This happens to a "
                           "new login that was not confirmed on another device, or when the session is terminated "
                           "from another device.",
                           "Telegram завершил этот сеанс, его данные на устройстве удалены. Так бывает с новым "
                           "входом, который не подтвердили на другом устройстве, или когда сеанс завершён с "
                           "другого устройства.",
                           "Telegram завершив цей сеанс, його дані на пристрої видалено. Так буває з новим входом, "
                           "який не підтвердили на іншому пристрої, або коли сеанс завершено з іншого пристрою."));
}

void MainWindow::debugLogin(const std::string &step, const std::string &value) {
  if (_login) _login->debugSubmit(step, value);
}

}  // namespace Contrail
