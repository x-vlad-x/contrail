#include "app.hpp"
#include "call_media.hpp"
#include "contrail_version.h"
#include "debug.hpp"
#include "local_state.hpp"
#include "main_window.hpp"
#include "notifications.hpp"
#include "td/scenario.hpp"
#include "td/session.hpp"
#include "td/strings.hpp"

#include <glib-unix.h>
#include <unistd.h>

#include <cerrno>
#include <optional>
#include <string>
#include <utility>

namespace Contrail {
namespace {

// Set by the process for the one it starts in its place (RestartWithoutWindow): it comes up without the window.
constexpr char kRestartedChat[] = "CONTRAIL_RESTARTED_CHAT";
constexpr char kRestartedNotifications[] = "CONTRAIL_RESTARTED_NOTIFICATIONS";

struct Program {
  AdwApplication *application = nullptr;
  std::unique_ptr<MainWindow> window;
  std::unique_ptr<Td::Session> session;
  bool quitting = false;
  bool restart = false;  // Starts again without the window once the application has quit.
  std::optional<std::string> restartedChat;  // This process was started so: the window's chat, possibly empty.
};

Program &Instance() {
  static Program program;
  return program;
}

void StartSession();

void SessionClosed(bool lost) {
  auto &program = Instance();
  if (program.quitting) return;
  // TDLib can't reopen a closed client, and this call comes from inside it: the new one starts from the main loop.
  g_idle_add_once(+[](gpointer data) {
    StartSession();
    if (data) Instance().window->showSessionLost();
  }, lost ? GINT_TO_POINTER(1) : nullptr);
}

void StartSession() {
  auto &program = Instance();
  // The language Contrail 0.3 used, if this one came after it; otherwise the system's.
  const auto chosen = ChosenLanguage();
  const auto language = chosen.empty() ? SystemLanguage() : chosen;
  // The window's store and open chat go with the client they talk to, while it is still there.
  program.window->detach();
  GetApp().session = nullptr;
  program.session.reset();
  program.session = std::make_unique<Td::Session>(
      g_getenv("CONTRAIL_SCENARIO") ? Td::MakeScenarioTransport() : Td::MakeTdTransport(), Td::SessionConfig{Td::DataDirectory(), language, kContrailVersion});
  GetApp().session = program.session.get();
  program.session->setClosedHandler(SessionClosed);
  program.window->attach(*program.session, language);
}

void Activate(GApplication *) {
  auto &program = Instance();
  if (!program.window) {
    LoadApplicationStyle();
    WatchSurfaces();
    Td::SetUpLog(Td::DataDirectory());
    program.window = std::make_unique<MainWindow>(GTK_APPLICATION(program.application));
    GetApp().window = program.window.get();
    if (g_getenv("CONTRAIL_DEBUG")) RegisterDebugActions(G_APPLICATION(program.application), *program.window);
    StartSession();
    if (const auto chat = std::exchange(program.restartedChat, std::nullopt)) {
      // Before GTK has put anything on the screen: the GPU's driver isn't set up at all.
      program.window->startWithoutWindow(*chat);
      return;
    }
  }
  program.window->present();
}

// tg:// and t.me links from the desktop entry (Exec=contrail -- %u) and other apps, as 0.3.1 took them.
void Open(GApplication *application, GFile **files, int count, const char *) {
  Activate(application);
  for (auto i = 0; i != count; ++i) {
    auto *uri = g_file_get_uri(files[i]);
    if (uri) Instance().window->openLinkWhenReady(uri);
    g_free(uri);
  }
}

void Shutdown(GApplication *) {
  auto &program = Instance();
  program.quitting = true;
  // Closes TDLib properly: its database must not be left in the middle of a write.
  if (program.window) program.window->detach();
  GetApp().session = nullptr;
  program.session.reset();
  GetApp().window = nullptr;
  program.window.reset();
  Calls::WaitForStoppedMedia();
}

gboolean Quit(gpointer) {
  // A signal to stop wins over a restart already asked for.
  Instance().restart = false;
  g_application_quit(G_APPLICATION(Instance().application));
  return G_SOURCE_REMOVE;
}

}  // namespace

void RestartWithoutWindow(const std::string &chat, const std::string &notifications) {
  auto &program = Instance();
  if (program.quitting || program.restart) return;
  // The same program, wherever it is: inside a Flatpak, an AppImage's mount or a package.
  if (access("/proc/self/exe", X_OK) != 0) {
    g_message("background: no restart, the program can't be started again: %s", g_strerror(errno));
    return;
  }
  g_setenv(kRestartedChat, chat.c_str(), TRUE);
  g_setenv(kRestartedNotifications, notifications.c_str(), TRUE);
  program.restart = true;
  g_message("background: starting again without the window");
  g_application_quit(G_APPLICATION(program.application));
}

}  // namespace Contrail

int main(int argc, char **argv) {
  using namespace Contrail;
  auto &program = Instance();
  // Started again in the background: what the last process left. Nothing it starts should take these along.
  if (const auto chat = g_getenv(kRestartedChat)) {
    program.restartedChat = chat;
    g_unsetenv(kRestartedChat);
  }
  if (const auto notifications = g_getenv(kRestartedNotifications)) {
    Notifier::TakeOver(notifications);
    g_unsetenv(kRestartedNotifications);
  }
  g_set_prgname(kAppId);
  g_set_application_name(kAppName);
  program.application = adw_application_new(kAppId, G_APPLICATION_HANDLES_OPEN);
  g_signal_connect(program.application, "activate", G_CALLBACK(Activate), nullptr);
  g_signal_connect(program.application, "open", G_CALLBACK(Open), nullptr);
  g_signal_connect(program.application, "shutdown", G_CALLBACK(Shutdown), nullptr);
  // Ctrl+C and SIGTERM close TDLib cleanly too.
  g_unix_signal_add(SIGTERM, Quit, nullptr);
  g_unix_signal_add(SIGINT, Quit, nullptr);
  const auto result = g_application_run(G_APPLICATION(program.application), argc, argv);
  g_object_unref(program.application);
  if (program.restart) {
    // TDLib has closed its database and the D-Bus name is free. Without the arguments: a link given to the first
    // start would open again, and with it the window.
    char *const arguments[] = {argv[0], nullptr};
    execv("/proc/self/exe", arguments);
    g_warning("background: starting again failed: %s", g_strerror(errno));
  }
  return result;
}
