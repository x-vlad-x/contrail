#pragma once

#include "app.hpp"
#include "td/store.hpp"

#include <cstdint>

namespace Contrail {

class MainWindow;
struct MessageItem;
struct ReplyPreview;

// An open chat: its messages and the message field.
class ChatView final {
 public:
  ChatView(Td::Store &store, Td::ChatId chat, MainWindow &window, std::int64_t showAt = 0);
  ~ChatView();

  [[nodiscard]] GtkWidget *widget() const { return _root; }
  [[nodiscard]] Td::ChatId chat() const { return _chat; }
  [[nodiscard]] Td::Store &store() const { return _store; }
  void showAt(std::int64_t message);
  // Cancels a reply or an edit; false if there was nothing to cancel.
  bool handleEscape();

  // What the messages ask for.
  void openChat(Td::ChatId chat);
  void openPhoto(const MessageItem &item, Td::FileId file);
  // A tap on a file: opens it if it is here, stops its download if it runs, starts one otherwise.
  void openDocument(const MessageItem &item, Td::FileId file);
  [[nodiscard]] bool opensWhenLoaded(Td::FileId file) const;
  void showMessageMenu(const MessageItem &item, GtkWidget *widget, double x, double y);
  void replyTo(const MessageItem &item);
  [[nodiscard]] ReplyPreview replyPreview(const MessageItem &item) const;
  void sendText(const std::string &text);  // Bot commands from links.

 private:
  Td::Store &_store;
  Td::ChatId _chat;
  MainWindow &_window;
  GtkWidget *_root = nullptr;
};

}  // namespace Contrail
