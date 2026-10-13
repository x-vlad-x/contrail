#include "app.hpp"

#include "debug.hpp"
#include "main_window.hpp"
#include "style_css.h"

#include "core/application.h"
#include "core/sandbox.h"
#include "window/main_window.h"
#include "window/window_controller.h"
#include "lang/lang_instance.h"

#include <QtCore/QCoreApplication>

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

namespace {

void LoadStyle() {
  auto *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, kStyleCss);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

}  // namespace

void HideCoreWindow() {
  // Telegram Desktop's own window is "shown" on the offscreen platform: hidden, it isn't painted at all.
  if (const auto window = Core::App().activePrimaryWindow()) {
    window->widget()->hide();
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
    Core::Quit();
    return G_SOURCE_REMOVE;
  }, nullptr);
  g_unix_signal_add(SIGINT, +[](gpointer) -> gboolean {
    Core::Quit();
    return G_SOURCE_REMOVE;
  }, nullptr);
}

}  // namespace Contrail
