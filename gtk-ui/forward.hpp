#pragma once

#include "app.hpp"
#include "td/store.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Contrail {

class MainWindow;

// Messages chosen to be forwarded. They wait above the message field of the chat (or topic) they go to, and go with
// whatever is sent there next, or alone. Kept for the session only, as Telegram Desktop keeps them.
struct ForwardDraft {
  Td::ChatId from = 0;
  std::vector<std::int64_t> ids;
  std::vector<std::string> senders;  // Who wrote them, each name once: what the bar says.
  std::string preview;               // Of a single message.
  bool names = true;                 // Without the senders' names they go as copies.
};

// "Forward", as the phone apps do it: the chats to choose from (in the chat list's order, Saved Messages first,
// with a search; a group with topics asks for the topic), then the chosen chat opens with the messages waiting
// above its message field. Only the chats and topics where one can write are offered.
class ForwardPicker final {
 public:
  ForwardPicker(Td::Store &store, MainWindow &window, ForwardDraft draft);
  ~ForwardPicker();

  [[nodiscard]] GtkWidget *dialog() const { return _dialog; }
  void present();

  // tools/phone.sh td-cmd: the row `index` of what is shown (the chats, or the topics of a group).
  void debugPick(int index, bool topics);
  void debugSearch(const std::string &text);
  void debugEnter();
  void debugState() const;

 private:
  struct Page {
    GtkWidget *view = nullptr;
    GtkWidget *search = nullptr;
    GtkWidget *list = nullptr;
    GtkWidget *scroll = nullptr;
    std::vector<std::string> words;
  };

  GtkWidget *makePage(Page &page);
  GtkWidget *makeRow(Td::ChatId chat, std::int32_t topic, bool forum);
  void fillChats();
  void showTopics(Td::ChatId chat);
  void fillTopics();
  void closeTopics();
  void activated(GtkListBoxRow *row);
  void chosen(Td::ChatId chat, std::int32_t topic);
  [[nodiscard]] bool canForwardTo(const Td::Chat &chat) const;
  [[nodiscard]] std::string chatName(const Td::Chat &chat) const;
  [[nodiscard]] Page &current();
  [[nodiscard]] static gboolean filterRow(GtkListBoxRow *row, gpointer data);

  Td::Store &_store;
  MainWindow &_window;
  ForwardDraft _draft;
  Td::Lifetime _lifetime;
  Td::Lifetime _forumLifetime;
  SignalConnections _signals;
  SignalConnections _topicSignals;
  GtkWidget *_dialog = nullptr;
  GtkWidget *_nav = nullptr;
  Page _chats;
  Page _topics;
  std::vector<Td::ChatId> _shown;  // The chats in the list now, to fill it again only when they change.
  Td::ChatId _self = 0;            // Saved Messages, once TDLib has given its chat.
  Td::ChatId _forum = 0;           // The group whose topics are shown.
  std::shared_ptr<void> _topicHold;
};

}  // namespace Contrail
