#include "chat_list.hpp"

namespace Contrail {

ChatList::ChatList(Td::Store &store, Td::ListKind kind, MainWindow &window)
    : _store(store), _kind(kind), _window(window) {
  _page = adw_navigation_page_new(gtk_label_new(""), "Contrail");
  g_object_ref_sink(_page);
}

ChatList::~ChatList() {
  g_object_unref(_page);
}

void ChatList::setCurrent(Td::ChatId) {}
void ChatList::startSearch(const std::string &) {}
void ChatList::openRow(int) {}

}  // namespace Contrail
