#pragma once

#include "app.hpp"
#include "td/store.hpp"

#include <cstdint>

namespace Contrail {

class MainWindow;

// An open chat: its messages and the message field.
class ChatView final {
 public:
  ChatView(Td::Store &store, Td::ChatId chat, MainWindow &window, std::int64_t showAt = 0);
  ~ChatView();

  [[nodiscard]] GtkWidget *widget() const { return _root; }
  [[nodiscard]] Td::ChatId chat() const { return _chat; }
  void showAt(std::int64_t message);
  // Cancels a reply or an edit; false if there was nothing to cancel.
  bool handleEscape();

 private:
  Td::Store &_store;
  Td::ChatId _chat;
  MainWindow &_window;
  GtkWidget *_root = nullptr;
};

}  // namespace Contrail
