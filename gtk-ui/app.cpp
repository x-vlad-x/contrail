#include "app.hpp"

#include "debug.hpp"
#include "main_window.hpp"
#include "qt_integration.hpp"
#include "style_css.h"

#include "core/application.h"
#include "core/sandbox.h"
#include "window/main_window.h"
#include "window/window_controller.h"
#include "lang/lang_instance.h"

#include <QtCore/QCoreApplication>
#include <QtWidgets/QApplication>

#include <glib-unix.h>

namespace Contrail {

App &GetApp() {
  static App app;
  return app;
}

std::string Escape(const QString &s) {
  auto *escaped = g_markup_escape_text(s.toUtf8().constData(), -1);
  std::string result = escaped;
  g_free(escaped);
  return result;
}

GdkTexture *ToTexture(const QImage &image) {
  if (image.isNull()) {
    return nullptr;
  }
  // ARGB32 premultiplied is BGRA in memory on little-endian machines.
  auto converted = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
  auto *bytes = g_bytes_new(converted.constBits(), converted.sizeInBytes());
  auto *texture = gdk_memory_texture_new(converted.width(), converted.height(), GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
                                         bytes, converted.bytesPerLine());
  g_bytes_unref(bytes);
  return texture;
}

GtkWidget *MakeIconButton(const char *iconName, const char *tooltip, const char *cssClass) {
  auto *button = gtk_button_new_from_icon_name(iconName);
  if (tooltip) {
    gtk_widget_set_tooltip_text(button, tooltip);
  }
  if (cssClass) {
    gtk_widget_add_css_class(button, cssClass);
  }
  return button;
}

void SetMarginAll(GtkWidget *w, int margin) {
  gtk_widget_set_margin_start(w, margin);
  gtk_widget_set_margin_end(w, margin);
  gtk_widget_set_margin_top(w, margin);
  gtk_widget_set_margin_bottom(w, margin);
}

void ClearBox(GtkWidget *box) {
  while (auto *child = gtk_widget_get_first_child(box)) {
    gtk_widget_unparent(child);
  }
}

void ShowToast(const QString &text) {
  if (auto *w = GetApp().window) {
    w->showToast(text);
  }
}

void OpenUri(const std::string &uri) {
  auto *launcher = gtk_uri_launcher_new(uri.c_str());
  auto *parent = GetApp().window ? GTK_WINDOW(GetApp().window->widget()) : nullptr;
  gtk_uri_launcher_launch(launcher, parent, nullptr, nullptr, nullptr);
  g_object_unref(launcher);
}

void CopyToClipboard(const QString &text) {
  gdk_clipboard_set_text(gdk_display_get_clipboard(gdk_display_get_default()), text.toUtf8().constData());
}

void TriggerFeedback(const char *event) {
  static auto *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
  if (!bus) {
    return;
  }
  g_dbus_connection_call(bus, "org.sigxcpu.Feedback", "/org/sigxcpu/Feedback", "org.sigxcpu.Feedback",
                         "TriggerFeedback", g_variant_new("(ss@a{sv}i)", kAppId, event,
                                                          g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0), -1),
                         nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr, nullptr);
}

void DestroyPopoverOnClose(GtkWidget *popover) {
  Connect<void(GtkPopover *)>(popover, "closed", [](GtkPopover *p) {
    // The message/chat row may disappear before the idle callback runs.
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, +[](gpointer data) -> gboolean {
      auto *widget = GTK_WIDGET(data);
      if (gtk_widget_get_parent(widget)) {
        gtk_widget_unparent(widget);
      }
      return G_SOURCE_REMOVE;
    }, g_object_ref(p), g_object_unref);
  });
}

namespace {

void LoadStyle() {
  auto *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, kStyleCss);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
#if GTK_CHECK_VERSION(4, 20, 0)
  // Since GTK 4.20 the "prefers-color-scheme" media queries of a stylesheet follow its provider's
  // own property, which libadwaita sets only on the stylesheets it loads itself: without this the
  // dark variants in style.css never apply.
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(provider), "prefers-color-scheme")) {
    const auto sync = +[](AdwStyleManager *manager, GParamSpec *, GtkCssProvider *provider) {
      g_object_set(provider, "prefers-color-scheme",
                   adw_style_manager_get_dark(manager) ? GTK_INTERFACE_COLOR_SCHEME_DARK
                                                       : GTK_INTERFACE_COLOR_SCHEME_LIGHT,
                   nullptr);
    };
    auto *manager = adw_style_manager_get_default();
    sync(manager, nullptr, provider);
    g_signal_connect_object(manager, "notify::dark", G_CALLBACK(sync), provider, GConnectFlags(0));
  }
#endif
  g_object_unref(provider);
}

}  // namespace

void HideCoreWindow() {
  // The Qt call panel is another top-level window. Keep it from painting or consuming shared video
  // frames behind GTK, just like the core's main window.
  for (auto *window : QApplication::topLevelWidgets()) {
    if (window->isVisible()) window->hide();
  }
}

// Called by the core when the app should come to the front: launched again, a link opened...
void Activate() {
  if (auto *w = GetApp().window) {
    w->present();
  }
}

// Called by Core::Application::run() once Telegram Desktop's core is up (see patches/).
void Start() {
  KeepGtkApplicationRunning();
  coreEventDispatcher = [](const std::function<void()> &fn) {
    Core::Sandbox::Instance().customEnterFromEventLoop(fn);
  };
  auto &app = GetApp();
  g_set_prgname(kAppId);
  g_set_application_name("Contrail");
  adw_init();
  // Icons the system theme doesn't have (installed next to the program).
  if (const auto dir = g_getenv("CONTRAIL_DIR")) {
    gtk_icon_theme_add_search_path(gtk_icon_theme_get_for_display(gdk_display_get_default()),
                                   (std::string(dir) + "/icons").c_str());
  }

  // The GApplication is only registered, never run(): the main loop is the one Qt already runs.
  // No GtkApplication: the core's own GApplication owns the application ID on D-Bus (see CONTRAIL_APP_ID in
  // the launcher) and handles activation and tg:// links. The window gets the ID from the program name.
  LoadStyle();
  gtk_window_set_default_icon_name(kAppId);

  app.window = new MainWindow();
  app.window->present();
  HideCoreWindow();
  if (g_getenv("CONTRAIL_DEBUG")) {
    RegisterDebugActions(*app.window);
  }

  // Ctrl+C / SIGTERM: quit cleanly so the core saves its state.
  g_unix_signal_add(SIGTERM, +[](gpointer) -> gboolean {
    RunCoreEvent([] { Core::Quit(); });
    return G_SOURCE_REMOVE;
  }, nullptr);
  g_unix_signal_add(SIGINT, +[](gpointer) -> gboolean {
    RunCoreEvent([] { Core::Quit(); });
    return G_SOURCE_REMOVE;
  }, nullptr);
}

}  // namespace Contrail
