// Contrail: an unofficial GTK4 + libadwaita Telegram client on TDLib.
#pragma once

#include "contrail_app.h"

#include <adwaita.h>
#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Contrail {

// kAppId (D-Bus name, desktop file and icon name) and kAppName come from the build (CMakeLists.txt): the version
// on TDLib runs next to the stable one under an ID of its own.
inline constexpr char kProjectUrl[] = "https://github.com/x-vlad-x/contrail";

class MainWindow;
namespace Td {
class Session;
}  // namespace Td

// Global application state. Everything here lives on the main thread.
struct App {
  MainWindow *window = nullptr;
  Td::Session *session = nullptr;  // Replaced when Telegram ends a session: TDLib can't reopen a closed client.
};

App &GetApp();

std::string Escape(const std::string &s);  // For Pango markup.

// Small helpers around the plain GTK C API.
GtkWidget *MakeIconButton(const char *iconName, const char *tooltip, const char *cssClass = nullptr);
void SetMarginAll(GtkWidget *w, int margin);
void ClearBox(GtkWidget *box);
void ShowToast(const std::string &text);
// A link (http, tg, geo...) in the program for it; a file on disk in the program for its kind.
void OpenUri(const std::string &uri);
void OpenPath(const std::string &path);
void CopyToClipboard(const std::string &text);
// Haptic/sound feedback through feedbackd (Phosh), if it runs: "button-pressed" and the like.
void TriggerFeedback(const char *event);
// The stylesheet and icons, once a display exists.
void LoadApplicationStyle();

// GTK sets up the GPU's driver for the first window, popover or dialog put on the screen, and keeps it, with its
// threads and memory, until the process ends. WatchSurfaces() notes when that happens (once a display exists).
void WatchSurfaces();
[[nodiscard]] bool SurfacesUsed();
// The process closes TDLib and starts itself again, without the window and so without the driver. `chat`: the
// place the window comes back to, `notifications`: those on the screen (Notifier::handOver).
void RestartWithoutWindow(const std::string &chat, const std::string &notifications);

// Connects a lambda to a GObject signal with no extra arguments besides the instance.
template <typename Fn>
void OnClicked(GtkWidget *button, Fn &&fn) {
  auto *heap = new std::function<void()>(std::forward<Fn>(fn));
  g_signal_connect_data(
      button, "clicked",
      G_CALLBACK(+[](GtkWidget *, gpointer data) {
        auto fn = *static_cast<std::function<void()> *>(data);
        fn();
      }), heap,
      [](gpointer data, GClosure *) { delete static_cast<std::function<void()> *>(data); }, GConnectFlags(0));
}

// Generic signal connection: Connect<void(GtkWidget *, int)>(obj, "signal", lambda).
// The std::function signature must match the C signal handler without the trailing user_data.
template <typename R, typename... Args>
struct SignalThunk {
  static R Call(Args... args, gpointer data) {
    // A handler can remove its own widget (opening another chat, for example).
    // Keep the callable alive until it returns, even if GTK destroys its closure.
    auto fn = *static_cast<std::function<R(Args...)> *>(data);
    return fn(args...);
  }
};

template <typename R, typename... Args>
gulong ConnectImpl(gpointer instance, const char *signal, std::function<R(Args...)> fn) {
  auto *heap = new std::function<R(Args...)>(std::move(fn));
  return g_signal_connect_data(
      instance, signal, G_CALLBACK(&(SignalThunk<R, Args...>::Call)), heap,
      [](gpointer data, GClosure *) { delete static_cast<std::function<R(Args...)> *>(data); }, GConnectFlags(0));
}

template <typename Sig, typename Fn>
gulong Connect(gpointer instance, const char *signal, Fn &&fn) {
  return ConnectImpl(instance, signal, std::function<Sig>(std::forward<Fn>(fn)));
}

// Views can outlive their C++ owner during GTK transitions. Disconnect handlers before teardown.
class SignalConnections final {
public:
  SignalConnections() = default;
  SignalConnections(const SignalConnections &) = delete;
  SignalConnections &operator=(const SignalConnections &) = delete;
  ~SignalConnections() { clear(); }

  template <typename Sig, typename Fn>
  void connect(gpointer instance, const char *signal, Fn &&fn) {
    const auto id = Connect<Sig>(instance, signal, std::forward<Fn>(fn));
    _handlers.emplace_back(G_OBJECT(g_object_ref(instance)), id);
  }

  template <typename Fn>
  void clicked(GtkWidget *button, Fn &&fn) {
    connect<void(GtkButton *)>(button, "clicked", [fn = std::forward<Fn>(fn)](GtkButton *) { fn(); });
  }

  void clear() {
    for (const auto &[object, id] : _handlers) {
      if (g_signal_handler_is_connected(object, id)) {
        g_signal_handler_disconnect(object, id);
      }
      g_object_unref(object);
    }
    _handlers.clear();
  }

private:
  std::vector<std::pair<GObject *, gulong>> _handlers;
};

void DestroyPopoverOnClose(GtkWidget *popover);


// Keeps a C++ object alive as long as a GObject lives.
template <typename T>
T *AttachData(gpointer object, const char *key, std::unique_ptr<T> data) {
  auto *raw = data.release();
  g_object_set_data_full(G_OBJECT(object), key, raw, [](gpointer p) { delete static_cast<T *>(p); });
  return raw;
}

}  // namespace Contrail
