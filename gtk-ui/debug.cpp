// Remote control for testing on the phone without touching it (only with CONTRAIL_DEBUG=1):
//   tools/phone.sh td-cmd 'tree'
// Commands: tree (what is on the screen, into the log) | login phone|code|password|name TEXT
#include "debug.hpp"

#include "main_window.hpp"

#include <string>

namespace Contrail {
namespace {

std::string Describe(GtkWidget *widget) {
  auto text = std::string();
  const auto add = [&](const char *value) {
    if (value && *value) text += std::string(" \"") + value + "\"";
  };
  if (GTK_IS_LABEL(widget)) add(gtk_label_get_text(GTK_LABEL(widget)));
  if (GTK_IS_BUTTON(widget)) add(gtk_button_get_label(GTK_BUTTON(widget)));
  if (ADW_IS_PREFERENCES_ROW(widget)) add(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(widget)));
  if (ADW_IS_ACTION_ROW(widget)) add(adw_action_row_get_subtitle(ADW_ACTION_ROW(widget)));
  if (GTK_IS_EDITABLE(widget) && !ADW_IS_PASSWORD_ENTRY_ROW(widget) && !GTK_IS_PASSWORD_ENTRY(widget)) {
    add(gtk_editable_get_text(GTK_EDITABLE(widget)));
  }
  return text;
}

// Only what carries text, indented by depth: the structure of a window is hundreds of plain boxes.
void Print(GtkWidget *widget, int depth, std::string &out) {
  if (!gtk_widget_get_mapped(widget)) return;
  if (const auto text = Describe(widget); !text.empty()) {
    out += std::string(size_t(depth), ' ') + G_OBJECT_TYPE_NAME(widget) + text + " "
        + std::to_string(gtk_widget_get_width(widget)) + "x" + std::to_string(gtk_widget_get_height(widget))
        + (gtk_widget_get_sensitive(widget) ? "" : " insensitive") + "\n";
  }
  for (auto *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    Print(child, depth + 1, out);
  }
}

void Run(MainWindow &window, const std::string &command) {
  const auto space = command.find(' ');
  const auto name = command.substr(0, space);
  const auto rest = space == std::string::npos ? std::string() : command.substr(space + 1);
  if (name == "tree") {
    auto out = std::string();
    Print(window.widget(), 0, out);
    g_message("tree\n%s", out.c_str());
  } else if (name == "login") {
    const auto gap = rest.find(' ');
    window.debugLogin(rest.substr(0, gap), gap == std::string::npos ? std::string() : rest.substr(gap + 1));
  } else {
    g_message("unknown debug command: %s", command.c_str());
  }
}

}  // namespace

void RegisterDebugActions(GApplication *application, MainWindow &window) {
  auto *action = g_simple_action_new("debug", G_VARIANT_TYPE_STRING);
  g_signal_connect(action, "activate", G_CALLBACK(+[](GSimpleAction *, GVariant *parameter, gpointer data) {
    Run(*static_cast<MainWindow *>(data), g_variant_get_string(parameter, nullptr));
  }), &window);
  g_action_map_add_action(G_ACTION_MAP(application), G_ACTION(action));
  g_object_unref(action);
}

}  // namespace Contrail
