// Remote control for testing on the phone without touching it (only with CONTRAIL_DEBUG=1):
//   gdbus call --session --dest io.github.x_vlad_x.Contrail --object-path /io/github/x_vlad_x/Contrail/Debug
//     --method org.gtk.Actions.Activate debug "[<'chat 0'>]" "{}"
// Commands: chat N | back | settings | contacts | archive | about | profile | search TEXT | row TEXT |
//           scroll-top | scroll-bottom

#include "debug.hpp"

#include "main_window.hpp"

#include "history/history.h"

#include <string>

namespace Contrail {
namespace {

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
    ActivateRow(w.widget(), arg);
  } else if (cmd == "scroll-top" || cmd == "scroll-bottom") {
    w.debugScroll(cmd == "scroll-top");
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
