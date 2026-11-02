// Remote control for testing on the phone without touching it (only with CONTRAIL_DEBUG=1):
//   tools/phone.sh td-cmd 'tree'
// Commands: tree (what is on the screen, into the log) | login phone|code|password|name TEXT | chat N | back
//   state (of the open chat) | scroll top|bottom|PIXELS | send TEXT | reply | edit TEXT | delete
//   mem | trim | close | search TEXT | archive | list-state | list-scroll top|bottom|PIXELS | list-menu N
//   list-do N ACTION | archive-do N ACTION (ACTION: pin, mute, read, archive, delete, delete-now without the
//   question, saved)
#include "debug.hpp"

#include "chat_list.hpp"
#include "chat_view.hpp"
#include "main_window.hpp"

#include <malloc.h>

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

// "N ACTION" for a row of a list.
void ListAction(ChatList *list, const std::string &rest) {
  const auto gap = rest.find(' ');
  if (list && gap != std::string::npos) list->debugAction(std::atoi(rest.c_str()), rest.substr(gap + 1));
}

void Run(MainWindow &window, const std::string &command) {
  const auto space = command.find(' ');
  const auto name = command.substr(0, space);
  const auto rest = space == std::string::npos ? std::string() : command.substr(space + 1);
  if (name == "mem") {
    // What malloc holds: given out and still in use (tcache included), or freed and kept by the process. The top
    // is the main arena's only.
    const auto info = mallinfo2();
    g_message("mem: in use %.1f MiB (arenas %.1f, mmap %.1f), free in the arenas %.1f MiB, releasable top %.1f MiB",
              (info.uordblks + info.hblkhd) / 1048576., info.uordblks / 1048576., info.hblkhd / 1048576.,
              info.fordblks / 1048576., info.keepcost / 1048576.);
  } else if (name == "trim") {
    malloc_trim(0);
    g_message("trim: done");
  } else if (name == "close") {
    // As the close button does: the window goes first, the program quits after it.
    gtk_window_close(GTK_WINDOW(window.widget()));
  } else if (name == "tree") {
    auto out = std::string();
    Print(window.widget(), 0, out);
    g_message("tree\n%s", out.c_str());
  } else if (name == "chat") {
    window.debugOpenChat(std::atoi(rest.c_str()));
  } else if (name == "back") {
    window.debugBack();
  } else if (name == "search") {
    if (auto *list = window.chatList()) list->startSearch(rest);
  } else if (name == "archive") {
    window.openArchive();
  } else if (name == "list-state") {
    if (auto *list = window.chatList()) list->debugState();
  } else if (name == "list-scroll") {
    if (auto *list = window.chatList()) list->debugScroll(rest);
  } else if (name == "list-menu") {
    if (auto *list = window.chatList()) list->debugMenu(std::atoi(rest.c_str()));
  } else if (name == "list-do") {
    ListAction(window.chatList(), rest);
  } else if (name == "archive-do") {
    ListAction(window.chatList(true), rest);
  } else if (name == "state" && window.chatView()) {
    window.chatView()->debugState();
  } else if (name == "scroll" && window.chatView()) {
    window.chatView()->debugScroll(rest);
  } else if (name == "send" && window.chatView()) {
    window.chatView()->debugSend(rest);
  } else if (name == "reply" && window.chatView()) {
    window.chatView()->debugReply();
  } else if (name == "edit" && window.chatView()) {
    window.chatView()->debugEdit(rest);
  } else if (name == "delete" && window.chatView()) {
    window.chatView()->debugDelete();
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
