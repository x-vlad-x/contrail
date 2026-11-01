#pragma once

#include "app.hpp"
#include "td/store.hpp"

#include <cstdint>
#include <string>

namespace Contrail {

class MainWindow;

// The chat list (or the archive) in the sidebar, kept in sync with the store.
class ChatList final {
 public:
  ChatList(Td::Store &store, Td::ListKind kind, MainWindow &window);
  ~ChatList();

  [[nodiscard]] AdwNavigationPage *page() const { return _page; }
  void setCurrent(Td::ChatId chat);
  void startSearch(const std::string &query = {});
  void openRow(int index);

 private:
  Td::Store &_store;
  Td::ListKind _kind;
  MainWindow &_window;
  AdwNavigationPage *_page = nullptr;
};

}  // namespace Contrail
