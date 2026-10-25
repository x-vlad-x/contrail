// Remote control for testing on the phone without touching it (only with CONTRAIL_DEBUG=1):
//   gdbus call --session --dest io.github.x_vlad_x.Contrail --object-path /io/github/x_vlad_x/Contrail/Debug
//     --method org.gtk.Actions.Activate debug "[<'chat 0'>]" "{}"
// Commands: testmode | login phone|code|name TEXT | chat N | open NAME | topic N | saved | back | settings | contacts | archive | about | profile | search TEXT | row TEXT |
//           scroll-top | scroll-bottom | layout | call-preview [incoming|video] [NAME] |
//           close | present | state | memory [low|critical] | zoom | reconnect | core-open [ID] | background on|off | check-draft | demo (fictional chats for screenshots, see gtk-ui/demo.hpp) | demo-sponsored |
//           attach paste|files PATH|PATH|send|cancel|documents on|off|group on|off|clear-field|record-start|record-send|
//             record-cancel|state |
//           history-state [older|newer|end|id:ID|photo:ID|delete:ID (Saved Messages)|play:ID|speed:X|player|find:TEXT|
//             react:ID:EMOJI (Saved Messages, demo)|reactions:ID|custom-reactions|menu:ID|location|map:ID|fix:LAT,LON,ACCURACY|live|live-update|
//             live-start:SECONDS (Saved Messages)|live-map|live-bar|pinned|pinned-click|pinned-list|pin:ID|unpin:ID
//             (Saved Messages)|forward:ID (Saved Messages)|forward-picker:ID|forward-state|forward-menu|trim (rows far from the screen)] |
//           map | screenshot [call] PATH

#include "debug.hpp"

#include "demo.hpp"

#include "main_window.hpp"
#include "map_view.hpp"
#include "zoom_view.hpp"
#include "core/sandbox.h"

#include "history/history.h"
#include "main/main_session.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "core/application.h"
#include "window/window_session_controller.h"
#include "window/window_controller.h"
#include "core/core_settings.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_dc_options.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"

#include <string>

namespace Contrail {
namespace {

GtkWidget *FindWindow(const char *cssClass) {
  GtkWidget *result = nullptr;
  auto *list = gtk_window_list_toplevels();
  for (auto *i = list; i; i = i->next) {
    if (gtk_widget_get_visible(GTK_WIDGET(i->data)) && gtk_widget_has_css_class(GTK_WIDGET(i->data), cssClass)) {
      result = GTK_WIDGET(i->data);
    }
  }
  g_list_free(list);
  return result;
}

// Open popovers (menus) are surfaces of their own: drawn over the window where the compositor shows them.
void SnapshotPopovers(GtkWidget *root, GtkWidget *window, GtkSnapshot *snapshot) {
  for (auto *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child)) {
    if (GTK_IS_POPOVER(child) && gtk_widget_get_mapped(child)) {
      auto *surface = gtk_native_get_surface(GTK_NATIVE(child));
      if (surface && GDK_IS_POPUP(surface)) {
        double windowX = 0, windowY = 0, popoverX = 0, popoverY = 0;
        gtk_native_get_surface_transform(GTK_NATIVE(window), &windowX, &windowY);
        gtk_native_get_surface_transform(GTK_NATIVE(child), &popoverX, &popoverY);
        const auto x = gdk_popup_get_position_x(GDK_POPUP(surface)) + popoverX - windowX;
        const auto y = gdk_popup_get_position_y(GDK_POPUP(surface)) + popoverY - windowY;
        auto *paintable = gtk_widget_paintable_new(child);
        gtk_snapshot_save(snapshot);
        const auto origin = GRAPHENE_POINT_INIT(float(x), float(y));
        gtk_snapshot_translate(snapshot, &origin);
        gdk_paintable_snapshot(paintable, snapshot, gtk_widget_get_width(child), gtk_widget_get_height(child));
        gtk_snapshot_restore(snapshot);
        g_object_unref(paintable);
      }
    }
    SnapshotPopovers(child, window, snapshot);
  }
}

void SaveWindowPng(GtkWidget *window, const char *path, double scale) {
  if (!window || !gtk_widget_get_realized(window)) {
    g_warning("Screenshot: no window");
    return;
  }
  const auto width = gtk_widget_get_width(window), height = gtk_widget_get_height(window);
  // With the screen off the compositor doesn't ask for frames: lay out whatever changed since.
  gtk_widget_allocate(window, width, height, -1, nullptr);
  auto *paintable = gtk_widget_paintable_new(window);
  auto *snapshot = gtk_snapshot_new();
  gtk_snapshot_scale(snapshot, float(scale), float(scale));
  gdk_paintable_snapshot(paintable, snapshot, width, height);
  SnapshotPopovers(window, window, snapshot);
  auto *node = gtk_snapshot_free_to_node(snapshot);
  const auto bounds = GRAPHENE_RECT_INIT(0, 0, float(width * scale), float(height * scale));
  auto *texture = gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(window)), node, &bounds);
  g_message("Screenshot %s: %dx%d %s", path, int(width * scale), int(height * scale),
            gdk_texture_save_to_png(texture, path) ? "saved" : "FAILED");
  g_object_unref(texture);
  gsk_render_node_unref(node);
  g_object_unref(paintable);
}

GtkWidget *FindByClass(GtkWidget *root, const char *cssClass) {
  for (auto *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child)) {
    if (gtk_widget_get_mapped(child) && gtk_widget_has_css_class(child, cssClass)) return child;
    if (auto *found = FindByClass(child, cssClass)) return found;
  }
  return nullptr;
}

// Activates the first visible row (AdwPreferencesRow or a list row with a label) containing |text|.
bool ActivateRow(GtkWidget *root, const std::string &text) {
  for (auto *c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c)) {
    if (!gtk_widget_get_mapped(c)) {
      continue;
    }
    if (ADW_IS_PREFERENCES_ROW(c)) {
      const auto *title = adw_preferences_row_get_title(ADW_PREFERENCES_ROW(c));
      if (title && std::string(title).find(text) != std::string::npos) {
        if (ADW_IS_ACTION_ROW(c) || ADW_IS_BUTTON_ROW(c)) {
          g_signal_emit_by_name(c, "activated");
        } else {
          gtk_widget_activate(c);
        }
        return true;
      }
    }
    if (ActivateRow(c, text)) {
      return true;
    }
  }
  return false;
}

void Run(MainWindow &w, const std::string &cmdline) {
  g_message("Debug command: %s", cmdline.c_str());
  const auto space = cmdline.find(' ');
  const auto cmd = cmdline.substr(0, space);
  const auto arg = (space == std::string::npos) ? std::string() : cmdline.substr(space + 1);
  if (cmd == "chat") {
    w.debugOpenChat(std::atoi(arg.c_str()));
  } else if (cmd == "topic") {
    w.debugOpenTopic(std::atoi(arg.c_str()));
  } else if (cmd == "open") {
    // A chat of the main list by (a part of) its name.
    if (const auto session = w.session()) {
      for (const auto &row : session->data().chatsList()->indexed()->all()) {
        if (const auto history = row->history(); history && history->peer->name().contains(Q(arg.c_str()))) {
          w.openHistory(history);
          break;
        }
      }
    }
  } else if (cmd == "testmode") {
    // Telegram's test servers for a new account, as the "testmode" code on Telegram Desktop's intro.
    auto &domain = Core::App().domain();
    if (domain.started() && domain.accounts().size() == 1 && !domain.active().sessionExists()
        && domain.active().mtp().environment() == MTP::Environment::Production) {
      domain.addActivated(MTP::Environment::Test);
    }
  } else if (cmd == "login") {
    const auto value = Q(arg.c_str());
    const auto space = value.indexOf(' ');
    w.debugLogin(value.left(space), space < 0 ? QString() : value.mid(space + 1));
  } else if (cmd == "saved") {
    if (const auto session = w.session()) {
      w.openPeer(session->user());
    }
  } else if (cmd == "back") {
    w.debugBack();
  } else if (cmd == "settings") {
    w.openSettings();
  } else if (cmd == "contacts") {
    w.openContacts();
  } else if (cmd == "archive") {
    w.openArchive();
  } else if (cmd == "about") {
    w.showAbout();
  } else if (cmd == "profile") {
    if (const auto history = w.currentHistory()) {
      w.openProfile(history->peer);
    }
  } else if (cmd == "search") {
    w.searchChats(Q(arg.c_str()));
  } else if (cmd == "row") {
    if (w.widget()) ActivateRow(w.widget(), arg);
  } else if (cmd == "scroll-top" || cmd == "scroll-bottom") {
    w.debugScroll(cmd == "scroll-top");
  } else if (cmd == "call-preview") {
    // call-preview [incoming|video|outgoing] [name]
    const auto value = Q(arg.c_str());
    const auto mode = value.section(' ', 0, 0);
    const auto name = value.section(' ', 1);
    w.debugCallPreview(mode == u"incoming"_q, mode == u"video"_q, name.isEmpty() ? u"Contrail"_q : name);
  } else if (cmd == "demo") {
    StartDemo();
  } else if (cmd == "demo-sponsored") {
    AddDemoSponsored();
  } else if (cmd == "map") {
    // The open map (a location's, the location picker's): where it is, its zoom and rotation.
#ifdef CONTRAIL_SHUMATE
    if (auto *map = w.widget() ? FindByClass(w.widget(), "contrail-map") : nullptr) {
      auto lat = 0., lon = 0.;
      MapCenter(map, &lat, &lon);
      g_message("Map: center=%.6f,%.6f zoom=%.3f rotation=%.3f", lat, lon, MapZoom(map), MapRotation(map));
    } else {
      g_message("Map: none");
    }
#endif  // CONTRAIL_SHUMATE
  } else if (cmd == "screenshot") {
    // screenshot [call] PATH: the window drawn by GTK itself, 3x, nothing of the phone around it.
    const auto value = Q(arg.c_str());
    const auto call = value.startsWith(u"call "_q);
    if (auto *window = call ? FindWindow("call-window") : w.widget()) {
      SaveWindowPng(window, U8(call ? value.mid(5) : value).c_str(), 3.);
    }
  } else if (cmd == "close") {
    if (w.widget()) gtk_window_close(GTK_WINDOW(w.widget()));
  } else if (cmd == "present") {
    w.present();
  } else if (cmd == "state") {
    w.debugState();
  } else if (cmd == "memory") {
    // "memory": the system's memory as Contrail sees it; "memory low|critical": free memory as if it were short.
    w.debugMemory(Q(arg.c_str()));
  } else if (cmd == "core-open") {
    // Saved Messages opened by the core, as a notification's click or a link does: "core-open [ID]".
    const auto window = Core::App().activePrimaryWindow();
    const auto controller = window ? window->sessionController() : nullptr;
    if (controller) {
      controller->showPeerHistory(controller->session().user()->id, Window::SectionShow::Way::ClearStack,
                                  MsgId(std::atoll(arg.c_str())));
    }
  } else if (cmd == "reconnect") {
    // Drops the connections as a network change would: what was being sent or uploaded is sent again.
    if (const auto session = w.session()) session->account().mtp().restart();
  } else if (cmd == "zoom") {
    // The photo viewer's scale and position (history-state photo:ID opens a message's photo).
    auto *view = ZoomViewFind(w.widget());
    g_message("Zoom state: %s", view ? ZoomViewState(view).c_str() : "no viewer");
  } else if (cmd == "background") {
    Core::App().settings().setCloseBehavior(arg == "on" ? Core::Settings::CloseBehavior::RunInBackground
                                                        : Core::Settings::CloseBehavior::Quit);
    w.updateBackgroundMode();
  } else if (cmd == "call-local-test") {
    w.debugLocalCall(Q(arg.c_str()));
  } else if (cmd == "attach") {
    w.debugAttach(Q(arg.c_str()));
  } else if (cmd == "check-draft") {
    w.debugCheckDraft();
  } else if (cmd == "history-state") {
    w.debugHistory(Q(arg.c_str()));
  } else if (cmd == "check-core-event") {
    Core::Sandbox::Instance().postponeCall([] {
      g_message("GTK/core event regression: postponed update completed");
    });
  } else if (cmd == "layout" && w.widget()) {
    const auto width = gtk_widget_get_width(w.widget());
    const auto check = [&](const auto &self, GtkWidget *widget) -> void {
      if (!gtk_widget_get_mapped(widget)) return;
      int minimum = 0, natural = 0;
      gtk_widget_measure(widget, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, &natural, nullptr, nullptr);
      // Header bars extend one pixel beyond each edge to draw their separators.
      if (minimum > width || gtk_widget_get_width(widget) > width + 2) {
        g_warning("Layout overflow: %s allocated=%d min=%d natural=%d window=%d",
            G_OBJECT_TYPE_NAME(widget), gtk_widget_get_width(widget), minimum, natural, width);
      }
      for (auto *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
        self(self, child);
      }
    };
    g_message("Layout: %dx%d, chat=%s", width, gtk_widget_get_height(w.widget()),
        w.currentHistory() ? "open" : "closed");
    check(check, w.widget());
  }
}

}  // namespace

void RegisterDebugActions(MainWindow &window) {
  auto *group = g_simple_action_group_new();
  auto *action = g_simple_action_new("debug", G_VARIANT_TYPE_STRING);
  Connect<void(GSimpleAction *, GVariant *)>(action, "activate", [&window](GSimpleAction *, GVariant *param) {
    Run(window, g_variant_get_string(param, nullptr));
  });
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
  g_object_unref(action);
  if (auto *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr)) {
    g_dbus_connection_export_action_group(bus, "/io/github/x_vlad_x/Contrail/Debug", G_ACTION_GROUP(group), nullptr);
  }
}

}  // namespace Contrail
