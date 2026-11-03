#include "app.hpp"

#include "css_split.hpp"
#include "main_window.hpp"
#include "style_css.h"

#include <cerrno>
#include <malloc.h>
#include <sys/prctl.h>


namespace Contrail {
namespace {

// Decoded pictures (a photo is up to 2560 px, 26 MB) come and go all the time. glibc's malloc raises
// its mmap threshold as large blocks are freed, after which they come from the heap and stay with the
// process when freed. A fixed threshold keeps every large block in its own mapping, given back to the
// system as soon as it is freed; fewer arenas keep the threads' freed memory from piling up. Set before
// main(), so TDLib's threads get these too. 64 KiB, not 256: blocks in between left holes in the heap that
// nothing reused. A chat with photos scrolled through and closed, by tools/mem-scenario.sh: 77 MiB at the peak
// instead of 91, 61 after it instead of 69; 32 KiB gave no more.
//
// Transparent huge pages off: with them on "always" (as FuriOS has them) the first touch of a
// thread's stack, a malloc arena or the end of the heap takes 2 MiB where a few KiB are used. On the FLX1s
// 30 MiB of the process were huge pages right after the start. Programs it starts inherit this.
[[maybe_unused]] const auto kMallocTuning = [] {
  if (prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0) != 0) g_debug("Memory: huge pages stay on: %s", g_strerror(errno));
  mallopt(M_MMAP_THRESHOLD, 64 * 1024);
  mallopt(M_TRIM_THRESHOLD, 4 * 1024 * 1024);
  mallopt(M_ARENA_MAX, 4);
  return 0;
}();

}  // namespace

App &GetApp() {
  static App app;
  return app;
}

std::string Escape(const std::string &s) {
  auto *escaped = g_markup_escape_text(s.c_str(), -1);
  std::string result = escaped;
  g_free(escaped);
  return result;
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

void ShowToast(const std::string &text) {
  if (auto *w = GetApp().window) {
    w->showToast(text);
  }
}

void OpenUri(const std::string &uri) {
  if (g_getenv("CONTRAIL_DEBUG")) g_message("open: %s", uri.c_str());
  // The scenario has no account and no network: what would open is only told.
  if (g_getenv("CONTRAIL_SCENARIO")) return;
  auto *launcher = gtk_uri_launcher_new(uri.c_str());
  auto *parent = GetApp().window ? GTK_WINDOW(GetApp().window->widget()) : nullptr;
  gtk_uri_launcher_launch(launcher, parent, nullptr, nullptr, nullptr);
  g_object_unref(launcher);
}

void CopyToClipboard(const std::string &text) {
  gdk_clipboard_set_text(gdk_display_get_clipboard(gdk_display_get_default()), text.c_str());
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
  auto *manager = adw_style_manager_get_default();
  auto *display = gdk_display_get_default();
#if GTK_CHECK_VERSION(4, 20, 0)
  // Since GTK 4.20 the "prefers-color-scheme" media queries of a stylesheet follow its provider's
  // own property, which libadwaita sets only on the stylesheets it loads itself: without this the
  // dark variants in style.css never apply.
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(provider), "prefers-color-scheme")) {
    gtk_css_provider_load_from_string(provider, kStyleCss);
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    const auto sync = +[](AdwStyleManager *manager, GParamSpec *, GtkCssProvider *provider) {
      g_object_set(provider, "prefers-color-scheme",
                   adw_style_manager_get_dark(manager) ? GTK_INTERFACE_COLOR_SCHEME_DARK
                                                       : GTK_INTERFACE_COLOR_SCHEME_LIGHT,
                   nullptr);
    };
    sync(manager, nullptr, provider);
    g_signal_connect_object(manager, "notify::dark", G_CALLBACK(sync), provider, GConnectFlags(0));
    g_object_unref(provider);
    return;
  }
#endif
  const auto [base, dark] = SplitDarkRules(kStyleCss);
  gtk_css_provider_load_from_string(provider, base.c_str());
  gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
  auto *darkProvider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(darkProvider, dark.c_str());
  const auto sync = +[](AdwStyleManager *manager, GParamSpec *, GtkCssProvider *provider) {
    auto *display = gdk_display_get_default();
    gtk_style_context_remove_provider_for_display(display, GTK_STYLE_PROVIDER(provider));
    if (adw_style_manager_get_dark(manager)) {
      gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider),
                                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }
  };
  sync(manager, nullptr, darkProvider);
  // The manager lives as long as the program: the provider stays with the handler.
  g_signal_connect(manager, "notify::dark", G_CALLBACK(sync), darkProvider);
}

}  // namespace

void LoadApplicationStyle() {
  LoadStyle();
  gtk_window_set_default_icon_name(kAppId);
  // Icons the system theme doesn't have (installed next to the program).
  if (const auto dir = g_getenv("CONTRAIL_DIR")) {
    gtk_icon_theme_add_search_path(gtk_icon_theme_get_for_display(gdk_display_get_default()),
                                   (std::string(dir) + "/icons").c_str());
  }
}

}  // namespace Contrail
