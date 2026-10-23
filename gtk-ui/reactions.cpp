#include "reactions.hpp"

#include "demo.hpp"
#include "main_window.hpp"
#include "scroll_helpers.hpp"
#include "text.hpp"
#include "userpics.hpp"

#include "apiwrap.h"
#include "data/data_document.h"
#include "data/data_message_reactions.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/stickers/data_custom_emoji.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/text/format_values.h"

#include <rpl/lifetime.h>

namespace Contrail {
namespace {

constexpr auto kMaxRecentUserpics = 3;  // Fewer reactions than this show who, not how many (as Telegram).
constexpr auto kStripCount = 6;         // Reactions at the top of the menu, before the button with all of them.
constexpr auto kWhoReactedLimit = 100;

using Data::ReactionId;

const char kStar[] = "\xE2\xAD\x90";  // A paid reaction: Telegram Stars.

GtkWidget *ReactionLabel(not_null<Main::Session *> session, const ReactionId &id, const char *cssClass) {
  auto *label = gtk_label_new(nullptr);
  gtk_widget_add_css_class(label, cssClass);
  SetReactionLabel(label, session, id);
  return label;
}

// The reactions offered for a message, as Telegram's reaction panel offers them.
std::vector<ReactionId> PossibleReactions(not_null<HistoryItem *> item) {
  const auto session = &item->history()->session();
  auto result = std::vector<ReactionId>();
  const auto possible = Data::LookupPossibleReactions(item);
  // Tags in Saved Messages are for Telegram Premium.
  if (possible.tags && !session->premium()) return result;
  for (const auto &reaction : possible.recent) {
    // Paid reactions need Telegram Stars, which this app doesn't handle.
    if (!reaction->id.paid()) result.push_back(reaction->id);
  }
  if (result.empty() && DemoActive() && item->canReact()) {
    // The offline demo never gets the server's list.
    for (const auto emoji : {"👍", "❤️", "🔥", "🥰", "👏", "😁", "🤔", "🤯", "😱", "🎉"}) {
      result.push_back(ReactionId{QString::fromUtf8(emoji)});
    }
  }
  return result;
}

GtkWidget *ChooserButton(not_null<HistoryItem *> item, const ReactionId &id, const char *labelClass,
                         Fn<void()> chosen) {
  const auto session = &item->history()->session();
  auto *button = gtk_button_new();
  gtk_button_set_child(GTK_BUTTON(button), ReactionLabel(session, id, labelClass));
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_add_css_class(button, "reaction-choice");
  const auto chosenIds = item->chosenReactions();
  if (std::find(chosenIds.begin(), chosenIds.end(), id) != chosenIds.end()) {
    gtk_widget_add_css_class(button, "chosen");
  }
  const auto itemId = item->fullId();
  OnClicked(button, [=] {
    if (const auto item = session->data().message(itemId)) {
      ToggleReaction(item, id);
    }
    if (chosen) chosen();
  });
  return button;
}

}  // namespace

void SetReactionLabel(GtkWidget *label, not_null<Main::Session *> session, const ReactionId &id) {
  if (id.paid()) {
    gtk_label_set_text(GTK_LABEL(label), kStar);
    return;
  } else if (const auto emoji = id.emoji(); !emoji.isEmpty()) {
    gtk_label_set_text(GTK_LABEL(label), U8(emoji).c_str());
    return;
  }
  // A custom emoji: the emoji it stands for, once its document is known.
  const auto alt = [](not_null<DocumentData *> document) {
    const auto sticker = document->sticker();
    return (sticker && !sticker->alt.isEmpty()) ? sticker->alt : QString::fromUtf8("\xE2\x9D\x94");
  };
  const auto document = session->data().document(id.custom());
  if (document->sticker()) {
    gtk_label_set_text(GTK_LABEL(label), U8(alt(document)).c_str());
    return;
  }
  gtk_label_set_text(GTK_LABEL(label), "\xE2\x80\xA6");
  auto *lifetime = AttachData(label, "contrail-custom-emoji", std::make_unique<rpl::lifetime>());
  session->data().customEmojiManager().resolve(id.custom()) | rpl::on_next([=](not_null<DocumentData *> document) {
    gtk_label_set_text(GTK_LABEL(label), U8(alt(document)).c_str());
  }, *lifetime);
}

int ReactionsCount(not_null<HistoryItem *> item) {
  auto result = 0;
  for (const auto &reaction : item->reactionsWithLocal()) result += reaction.count;
  return result;
}

void ToggleReaction(not_null<HistoryItem *> item, const ReactionId &id) {
  if (id.empty() || id.paid()) return;
  item->toggleReaction(id, HistoryReactionSource::Selector);
  TriggerFeedback("button-pressed");
}

void ToggleQuickReaction(not_null<HistoryItem *> item) {
  if (item->reactionsAreTags()) return;
  const auto possible = PossibleReactions(item);
  if (possible.empty()) return;
  const auto favorite = item->history()->owner().reactions().favoriteId();
  const auto id = std::find(possible.begin(), possible.end(), favorite) != possible.end() ? favorite : possible.front();
  item->toggleReaction(id, HistoryReactionSource::Quick);
  TriggerFeedback("button-pressed");
}

bool CanShowWhoReacted(not_null<HistoryItem *> item) {
  return item->canViewReactions() && !item->reactions().empty() && !item->history()->peer->isBroadcast();
}

// --- under the message -------------------------------------------------------

GtkWidget *MakeReactionsBar(not_null<HistoryItem *> item, not_null<MainWindow *> window) {
  const auto list = item->reactionsWithLocal();
  if (list.empty()) return nullptr;
  const auto session = &item->history()->session();
  const auto &recent = item->recentReactions();
  const auto showUserpics = !item->history()->peer->isBroadcast() && ReactionsCount(item) <= kMaxRecentUserpics;
  auto *flow = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), FALSE);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 20);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 4);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 4);
  gtk_widget_set_halign(flow, GTK_ALIGN_START);
  gtk_widget_add_css_class(flow, "reactions");
  const auto itemId = item->fullId();
  for (const auto &reaction : list) {
    const auto id = reaction.id;
    auto *chip = gtk_button_new();
    gtk_widget_add_css_class(chip, "reaction-chip");
    if (reaction.my) gtk_widget_add_css_class(chip, "chosen");
    if (id.paid()) gtk_widget_add_css_class(chip, "paid");
    if (item->reactionsAreTags()) gtk_widget_add_css_class(chip, "tag");
    auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(box), ReactionLabel(session, id, "reaction-emoji"));
    const auto i = recent.find(id);
    const auto who = (showUserpics && i != recent.end() && int(i->second.size()) == reaction.count)
                         ? &i->second
                         : nullptr;
    if (who) {
      auto *avatars = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_add_css_class(avatars, "reaction-userpics");
      for (const auto &one : *who) {
        gtk_box_append(GTK_BOX(avatars), MakeAvatar(one.peer, 18));
      }
      gtk_box_append(GTK_BOX(box), avatars);
    } else if (reaction.count > 0 && !(item->reactionsAreTags() && reaction.count == 1)) {
      auto *count = gtk_label_new(U8(Lang::FormatCountToShort(reaction.count).string).c_str());
      gtk_widget_add_css_class(count, "reaction-count");
      gtk_box_append(GTK_BOX(box), count);
    }
    gtk_button_set_child(GTK_BUTTON(chip), box);
    // Taps here are taps on the message: as for its links, not when they stop a scroll.
    OnClicked(chip, [=] {
      if (!TapAllowed()) return;
      if (const auto item = session->data().message(itemId)) ToggleReaction(item, id);
    });
    auto *longPress = gtk_gesture_long_press_new();
    gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(longPress), TRUE);
    Connect<void(GtkGestureLongPress *, double, double)>(longPress, "pressed", [=](GtkGestureLongPress *gesture, double, double) {
      const auto item = session->data().message(itemId);
      if (!item || !CanShowWhoReacted(item)) return;
      gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
      ShowWhoReacted(window, item, id);
    });
    gtk_widget_add_controller(chip, GTK_EVENT_CONTROLLER(longPress));
    gtk_flow_box_append(GTK_FLOW_BOX(flow), chip);
    auto *child = gtk_widget_get_parent(chip);
    gtk_widget_set_focusable(child, FALSE);
    gtk_widget_add_css_class(child, "reaction-cell");
  }
  return flow;
}

// --- choosing ----------------------------------------------------------------

GtkWidget *MakeReactionsStrip(not_null<HistoryItem *> item, not_null<MainWindow *> window, Fn<void()> chosen) {
  const auto possible = PossibleReactions(item);
  if (possible.empty()) return nullptr;
  auto *strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(strip, "reactions-strip");
  const auto shown = std::min(int(possible.size()), kStripCount);
  for (auto i = 0; i != shown; ++i) {
    gtk_box_append(GTK_BOX(strip), ChooserButton(item, possible[i], "reaction-strip-emoji", chosen));
  }
  if (int(possible.size()) > shown) {
    auto *more = MakeIconButton("pan-down-symbolic", U8(tr::lng_manage_peer_reactions(tr::now)).c_str(), "flat");
    gtk_widget_add_css_class(more, "circular");
    gtk_widget_add_css_class(more, "reactions-more");
    gtk_widget_set_valign(more, GTK_ALIGN_CENTER);
    const auto session = &item->history()->session();
    const auto itemId = item->fullId();
    OnClicked(more, [=] {
      if (chosen) chosen();
      if (const auto item = session->data().message(itemId)) ShowReactionsChooser(window, item);
    });
    gtk_box_append(GTK_BOX(strip), more);
  }
  return strip;
}

void ShowReactionsChooser(not_null<MainWindow *> window, not_null<HistoryItem *> item) {
  const auto possible = PossibleReactions(item);
  if (possible.empty() || !window->widget()) return;
  auto *dialog = adw_dialog_new();
  adw_dialog_set_title(dialog, U8(tr::lng_manage_peer_reactions(tr::now)).c_str());
  adw_dialog_set_content_width(dialog, 420);
  adw_dialog_set_content_height(dialog, 460);
  auto *view = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());
  auto *flow = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 5);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 8);
  gtk_widget_set_valign(flow, GTK_ALIGN_START);
  SetMarginAll(flow, 8);
  for (const auto &id : possible) {
    auto *button = ChooserButton(item, id, "reaction-grid-emoji", [=] { adw_dialog_close(dialog); });
    gtk_flow_box_append(GTK_FLOW_BOX(flow), button);
    gtk_widget_set_focusable(gtk_widget_get_parent(button), FALSE);
  }
  auto *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), flow);
  gtk_widget_set_vexpand(scroll, TRUE);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroll);
  adw_dialog_set_child(dialog, view);
  adw_dialog_present(dialog, window->widget());
}

// --- who reacted -------------------------------------------------------------

void ShowWhoReacted(not_null<MainWindow *> window, not_null<HistoryItem *> item, ReactionId filter) {
  if (!CanShowWhoReacted(item) || !window->widget()) return;
  const auto session = &item->history()->session();
  auto *dialog = adw_dialog_new();
  adw_dialog_set_title(dialog, U8(tr::lng_context_seen_reacted(tr::now, lt_count, ReactionsCount(item))).c_str());
  adw_dialog_set_content_width(dialog, 420);
  adw_dialog_set_content_height(dialog, 520);
  auto *view = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());
  auto *stack = gtk_stack_new();
  auto *spinner = adw_spinner_new();
  gtk_widget_set_size_request(spinner, 32, 32);
  gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
  gtk_stack_add_named(GTK_STACK(stack), spinner, "loading");
  auto *list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(list, "boxed-list");
  gtk_widget_set_valign(list, GTK_ALIGN_START);
  SetMarginAll(list, 12);
  auto *clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(clamp), list);
  auto *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
  gtk_stack_add_named(GTK_STACK(stack), scroll, "list");
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), stack);
  adw_dialog_set_child(dialog, view);

  // The dialog may be closed before the answer: the request goes with it.
  struct Request {
    not_null<Main::Session *> session;
    mtpRequestId id = 0;
    ~Request() {
      if (id) session->api().request(id).cancel();
    }
  };
  auto *request = AttachData(dialog, "contrail-who-reacted", std::make_unique<Request>(Request{session}));
  using Flag = MTPmessages_GetMessageReactionsList::Flag;
  request->id =
      session->api()
          .request(MTPmessages_GetMessageReactionsList(MTP_flags(filter ? Flag::f_reaction : Flag(0)),
                                                       item->history()->peer->input(), MTP_int(item->id),
                                                       Data::ReactionToMTP(filter), MTPstring(), MTP_int(kWhoReactedLimit)))
          .done([=](const MTPmessages_MessageReactionsList &result) {
            request->id = 0;
            const auto &data = result.data();
            session->data().processUsers(data.vusers());
            session->data().processChats(data.vchats());
            for (const auto &reaction : data.vreactions().v) {
              const auto &fields = reaction.data();
              const auto peer = session->data().peer(peerFromMTP(fields.vpeer_id()));
              auto *row = adw_action_row_new();
              adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                            Escape(peer->isSelf() ? tr::lng_saved_messages(tr::now) : peer->name()).c_str());
              adw_action_row_add_prefix(ADW_ACTION_ROW(row), MakeAvatar(peer, 36));
              auto *emoji = ReactionLabel(session, Data::ReactionFromMTP(fields.vreaction()), "reaction-row-emoji");
              adw_action_row_add_suffix(ADW_ACTION_ROW(row), emoji);
              gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
              Connect<void(AdwActionRow *)>(row, "activated", [=](AdwActionRow *) {
                adw_dialog_close(dialog);
                window->openPeer(peer);
              });
              gtk_list_box_append(GTK_LIST_BOX(list), row);
            }
            gtk_stack_set_visible_child_name(GTK_STACK(stack), "list");
          })
          .fail([=](const MTP::Error &error) {
            request->id = 0;
            ShowToast(error.type());
            adw_dialog_close(dialog);
          })
          .send();
  adw_dialog_present(dialog, window->widget());
}

}  // namespace Contrail
