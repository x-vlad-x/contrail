#include "app.hpp"
#include "call_media.hpp"
#include "contrail_version.h"
#include "debug.hpp"
#include "main_window.hpp"
#include "td/scenario.hpp"
#include "td/session.hpp"
#include "td/strings.hpp"

#include <glib-unix.h>

namespace Contrail {
namespace {

struct Program {
  AdwApplication *application = nullptr;
  std::unique_ptr<MainWindow> window;
  std::unique_ptr<Td::Session> session;
  bool quitting = false;
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
  const auto language = SystemLanguage();
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
    Td::SetUpLog(Td::DataDirectory());
    program.window = std::make_unique<MainWindow>(GTK_APPLICATION(program.application));
    GetApp().window = program.window.get();
    if (g_getenv("CONTRAIL_DEBUG")) RegisterDebugActions(G_APPLICATION(program.application), *program.window);
    StartSession();
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
  g_application_quit(G_APPLICATION(Instance().application));
  return G_SOURCE_REMOVE;
}

}  // namespace
}  // namespace Contrail

int main(int argc, char **argv) {
  using namespace Contrail;
  auto &program = Instance();
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
  return result;
}
