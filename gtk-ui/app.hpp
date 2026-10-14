// Contrail: an unofficial GTK4 + libadwaita interface for Telegram Desktop.
#pragma once

#include <adwaita.h>
#include <gtk/gtk.h>

#include <QtCore/QString>
#include <QtGui/QImage>

#include <functional>
#include <memory>
#include <type_traits>
#include <string>
#include <vector>

namespace Main {
class Session;
class Account;
} // namespace Main

namespace Contrail {

// Application ID: D-Bus name, desktop file and icon name.
inline constexpr char kAppId[] = "io.github.x_vlad_x.Contrail";
inline constexpr char kProjectUrl[] = "https://github.com/x-vlad-x/contrail";

class MainWindow;

// Global application state. Everything here lives on the main thread, which runs both the
// GLib main loop (GTK) and Qt's event loop (Telegram Desktop's core): Qt uses GLib's dispatcher.
struct App {
  MainWindow *window = nullptr;
};

App &GetApp();

// Hides Telegram Desktop's own (offscreen) window, so it doesn't paint in the background.
void HideCoreWindow();

// QString <-> UTF-8.
inline std::string U8(const QString &s) {
  return s.toStdString();
}
inline QString Q(const char *s) {
  return QString::fromUtf8(s);
}
std::string Escape(const QString &s);  // For Pango markup.

// QImage -> GdkTexture (nullptr for a null image). The caller owns the reference.
GdkTexture *ToTexture(const QImage &image);

// Small helpers around the plain GTK C API.
GtkWidget *MakeIconButton(const char *iconName, const char *tooltip, const char *cssClass = nullptr);
void SetMarginAll(GtkWidget *w, int margin);
void ClearBox(GtkWidget *box);
void ShowToast(const QString &text);
void OpenUri(const std::string &uri);
void CopyToClipboard(const QString &text);
// Haptic/sound feedback through feedbackd (Phosh), if it runs: "button-pressed" and the like.
void TriggerFeedback(const char *event);

// GTK uses GLib's native dispatch, outside QApplication::notify(). The Telegram core requires
// a registered event context before it can postpone widget/media updates.
using CoreEventDispatcher = void (*)(const std::function<void()> &);
inline CoreEventDispatcher coreEventDispatcher = nullptr;
inline void RunCoreEvent(const std::function<void()> &fn) {
  if (coreEventDispatcher) coreEventDispatcher(fn);
  else fn(); // Standalone GTK tests, before the core is started.
}

// Connects a lambda to a GObject signal with no extra arguments besides the instance.
template <typename Fn>
void OnClicked(GtkWidget *button, Fn &&fn) {
  auto *heap = new std::function<void()>(std::forward<Fn>(fn));
  g_signal_connect_data(
      button, "clicked",
      G_CALLBACK(+[](GtkWidget *, gpointer data) {
        auto fn = *static_cast<std::function<void()> *>(data);
        RunCoreEvent(fn);
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
    if constexpr (std::is_void_v<R>) {
      RunCoreEvent([&] { fn(args...); });
    } else {
      R result{};
      RunCoreEvent([&] { result = fn(args...); });
      return result;
    }
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
