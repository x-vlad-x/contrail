// Remote control for testing on the phone without touching it (only with CONTRAIL_DEBUG=1):
//   tools/phone.sh td-cmd 'tree'
// Commands: tree (what is on the screen, into the log) | login phone|code|password|name TEXT | chat N | back
//   state (of the open chat) | scroll top|bottom|PIXELS | send TEXT | type TEXT (a draft) | reply | edit TEXT | delete
//   mem | trim | close | search TEXT | archive | list-state | list-scroll top|bottom|PIXELS | list-menu N
//   list-do N ACTION | archive-do N ACTION (ACTION: pin, mute, read, archive, delete, delete-now without the
//   question, saved)
//   topic N (opens topic N of the group's topics shown) | topic-state | topic-menu N | topic-do N mute|read
//   forward (the newest message of the open chat: the chats to choose from) | forward-pick N (row N of the chats
//   shown) | forward-pick-topic N (of the group's topics) | forward-search TEXT | forward-enter | forward-picker
//   (what it shows) | forward-mode names|no-names | forward-cancel | forward-menu (of the messages waiting)
//   menu [N] (of message N, its server number; the newest without it) | pin [N] | unpin [N] (the question comes)
//   pinned-state | pinned-bar (a tap on it) | pinned-close (its cross) | pinned-list (all of them) | pinned-list-state
//   pinned-list-pick N | pinned-list-action (unpin all, or hide)
//   dialog (the question shown: its texts and check box) | dialog-check 0|1 | dialog-answer RESPONSE (cancel, pin...)
//   react KEY [N] (on message N, the newest without it: a tap on its chip, or as the menu does; KEY an emoji or
//   custom:ID) | unreact [N] (the user's own taken back) | reacted [N] (who reacted) | react-quick [N] (a double tap)
//   reactions-more [N] (in its menu open, the button with all of them) | reactions-state [N] (its reactions and
//   chips, the dialogs and the menu strip open, rows built so far)
//   menu-state (the items of the message menu open) | menu-do ACTION (as a tap on one: copy, save, delete, retry,
//   reply, forward, edit, pin...) | media [N] (what message N shows: its media and downloads, the sizes of its bubble
//   and texts; N -1: the sponsored message) | tap N [K] (a tap on its media or button K, 0 without it)
//   tap-text N TEXT (a tap on the link or spoiler of its text with TEXT in it) | sponsored (the sponsored message)
//   edit-last (the up arrow in the empty field)
//   call-start|video USER | call-incoming|incoming-video | call-accept|hangup|mute|camera|switch-camera
//   call-state|hide|present|speaker | call-route ID | call-near|lock 0|1 | call-reconnecting|established|failed
//   call-remote-muted|remote-video-off|busy|end|rated | call-rate 1..5
#include "debug.hpp"

#include "chat_list.hpp"
#include "chat_view.hpp"
#include "forward.hpp"
#include "main_window.hpp"
#include "topic_list.hpp"
#include "userpics.hpp"

#include <malloc.h>

#include <cstdio>
#include <cstdlib>

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

// The question in front, if there is one.
AdwAlertDialog *ShownAlert(MainWindow &window) {
  auto *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window.widget()));
  return ADW_IS_ALERT_DIALOG(dialog) ? ADW_ALERT_DIALOG(dialog) : nullptr;
}

void DialogCommand(MainWindow &window, const std::string &name, const std::string &rest) {
  auto *alert = ShownAlert(window);
  if (!alert) {
    g_message("dialog: none");
    return;
  }
  auto *extra = adw_alert_dialog_get_extra_child(alert);
  auto *check = GTK_IS_CHECK_BUTTON(extra) ? GTK_CHECK_BUTTON(extra) : nullptr;
  if (name == "dialog-check") {
    if (check) gtk_check_button_set_active(check, std::atoi(rest.c_str()) != 0);
  } else if (name == "dialog-answer") {
    // As a tap on the button: the answer, then the dialog closes.
    g_signal_emit_by_name(alert, "response", rest.c_str());
    adw_dialog_force_close(ADW_DIALOG(alert));
  } else {
    const auto heading = adw_alert_dialog_get_heading(alert);
    const auto body = adw_alert_dialog_get_body(alert);
    const auto label = check ? gtk_check_button_get_label(check) : nullptr;
    g_message("dialog: heading=\"%s\" body=\"%s\" check=\"%s\" checked=%d", heading ? heading : "", body ? body : "",
              label ? label : "", check ? int(gtk_check_button_get_active(check)) : -1);
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
  if (name.starts_with("call-")) {
    window.debugCall(name.substr(5), rest);
  } else if (name == "mem") {
    // What malloc holds: given out and still in use (tcache included), or freed and kept by the process. The top
    // is the main arena's only.
    const auto info = mallinfo2();
    g_message("mem: in use %.1f MiB (arenas %.1f, mmap %.1f), free in the arenas %.1f MiB, releasable top %.1f MiB, "
              "avatars %.1f MiB",
              (info.uordblks + info.hblkhd) / 1048576., info.uordblks / 1048576., info.hblkhd / 1048576.,
              info.fordblks / 1048576., info.keepcost / 1048576., AvatarCacheBytes() / 1048576.);
  } else if (name == "mem-arenas") {
    // Each malloc arena: what it took from the system and how much of it is free. Arena 0 is the main thread's
    // (GTK and the interface), the others TDLib's threads and the decoders.
    char *xml = nullptr;
    size_t size = 0;
    if (auto *stream = open_memstream(&xml, &size)) {
      malloc_info(0, stream);
      std::fclose(stream);
      const auto text = std::string(xml, size);
      std::free(xml);
      const auto value = [&](std::size_t from, std::size_t to, const std::string &tag) {
        const auto at = text.find(tag, from);
        if (at == std::string::npos || at > to) return 0.;
        const auto quote = text.find("size=\"", at);
        return (quote == std::string::npos || quote > to) ? 0. : std::atof(text.c_str() + quote + 6) / 1048576.;
      };
      auto line = std::string("mem-arenas:");
      for (auto at = text.find("<heap nr="); at != std::string::npos;) {
        const auto next = text.find("<heap nr=", at + 1);
        const auto end = (next == std::string::npos) ? text.size() : next;
        const auto system = value(at, end, "<system type=\"current\"");
        const auto free = value(at, end, "<total type=\"fast\"") + value(at, end, "<total type=\"rest\"");
        char part[96];
        std::snprintf(part, sizeof(part), " [%d] %.1f/%.1f", std::atoi(text.c_str() + at + 10), system - free, system);
        line += part;
        at = next;
      }
      g_message("%s MiB used/taken", line.c_str());
    }
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
  } else if (name == "topic") {
    window.debugOpenTopic(std::atoi(rest.c_str()));
  } else if (name == "topic-state") {
    if (auto *topics = window.topicList()) topics->debugState();
  } else if (name == "topic-menu") {
    if (auto *topics = window.topicList()) topics->debugMenu(std::atoi(rest.c_str()));
  } else if (name == "topic-do") {
    const auto gap = rest.find(' ');
    if (auto *topics = window.topicList(); topics && gap != std::string::npos) {
      topics->debugAction(std::atoi(rest.c_str()), rest.substr(gap + 1));
    }
  } else if (name == "forward" && window.chatView()) {
    window.chatView()->debugForward();
  } else if (name == "forward-pick" && window.forwardPicker()) {
    window.forwardPicker()->debugPick(std::atoi(rest.c_str()), false);
  } else if (name == "forward-pick-topic" && window.forwardPicker()) {
    window.forwardPicker()->debugPick(std::atoi(rest.c_str()), true);
  } else if (name == "forward-search" && window.forwardPicker()) {
    window.forwardPicker()->debugSearch(rest);
  } else if (name == "forward-enter" && window.forwardPicker()) {
    window.forwardPicker()->debugEnter();
  } else if (name == "forward-picker") {
    if (auto *picker = window.forwardPicker()) picker->debugState();
    else g_message("forward picker: none");
  } else if (name == "forward-mode" && window.chatView()) {
    window.chatView()->debugForwardMode(rest != "no-names");
  } else if (name == "forward-cancel" && window.chatView()) {
    window.chatView()->debugForwardCancel();
  } else if (name == "forward-menu" && window.chatView()) {
    window.chatView()->debugForwardMenu();
  } else if (name == "menu" && window.chatView()) {
    window.chatView()->debugMessageMenu(std::atoll(rest.c_str()));
  } else if ((name == "pin" || name == "unpin") && window.chatView()) {
    window.chatView()->debugPin(std::atoll(rest.c_str()), name == "pin");
  } else if (name == "pinned-state" && window.chatView()) {
    window.chatView()->debugPinnedBar("state");
  } else if (name == "pinned-bar" && window.chatView()) {
    window.chatView()->debugPinnedBar("tap");
  } else if (name == "pinned-close" && window.chatView()) {
    window.chatView()->debugPinnedBar("close");
  } else if (name == "pinned-list" && window.chatView()) {
    window.chatView()->debugPinnedBar("list");
  } else if (name == "pinned-list-state" && window.chatView()) {
    window.chatView()->debugPinnedList("state");
  } else if (name == "pinned-list-pick" && window.chatView()) {
    window.chatView()->debugPinnedList("pick " + rest);
  } else if (name == "pinned-list-action" && window.chatView()) {
    window.chatView()->debugPinnedList("action");
  } else if (name == "react" && window.chatView()) {
    const auto gap = rest.find(' ');
    window.chatView()->debugReact(rest.substr(0, gap),
                                  gap == std::string::npos ? 0 : std::atoll(rest.c_str() + gap + 1));
  } else if (name == "unreact" && window.chatView()) {
    window.chatView()->debugUnreact(std::atoll(rest.c_str()));
  } else if (name == "reacted" && window.chatView()) {
    window.chatView()->debugReacted(std::atoll(rest.c_str()));
  } else if (name == "react-quick" && window.chatView()) {
    window.chatView()->debugReactQuick(std::atoll(rest.c_str()));
  } else if (name == "reactions-more" && window.chatView()) {
    window.chatView()->debugReactionsMore(std::atoll(rest.c_str()));
  } else if (name == "reactions-state" && window.chatView()) {
    window.chatView()->debugReactionsState(std::atoll(rest.c_str()));
  } else if (name == "menu-state" && window.chatView()) {
    window.chatView()->debugMenuState();
  } else if (name == "menu-do" && window.chatView()) {
    window.chatView()->debugMenuDo(rest);
  } else if (name == "media" && window.chatView()) {
    window.chatView()->debugMedia(std::atoll(rest.c_str()));
  } else if (name == "tap" && window.chatView()) {
    const auto gap = rest.find(' ');
    window.chatView()->debugTap(std::atoll(rest.c_str()), gap == std::string::npos ? 0 : std::atoi(rest.c_str() + gap + 1));
  } else if (name == "tap-text" && window.chatView()) {
    const auto gap = rest.find(' ');
    if (gap != std::string::npos) window.chatView()->debugTapText(std::atoll(rest.c_str()), rest.substr(gap + 1));
  } else if (name == "sponsored" && window.chatView()) {
    window.chatView()->debugSponsored();
  } else if (name == "dialog" || name == "dialog-check" || name == "dialog-answer") {
    DialogCommand(window, name, rest);
  } else if (name == "state" && window.chatView()) {
    window.chatView()->debugState();
  } else if (name == "scroll" && window.chatView()) {
    window.chatView()->debugScroll(rest);
  } else if (name == "send" && window.chatView()) {
    window.chatView()->debugSend(rest);
  } else if (name == "type" && window.chatView()) {
    window.chatView()->debugType(rest);
  } else if (name == "reply" && window.chatView()) {
    window.chatView()->debugReply();
  } else if (name == "edit" && window.chatView()) {
    window.chatView()->debugEdit(rest);
  } else if (name == "edit-last" && window.chatView()) {
    window.chatView()->debugEditLast();
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
