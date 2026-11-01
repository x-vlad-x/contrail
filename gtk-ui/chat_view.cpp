#include "chat_view.hpp"

namespace Contrail {

ChatView::ChatView(Td::Store &store, Td::ChatId chat, MainWindow &window, std::int64_t)
    : _store(store), _chat(chat), _window(window) {
  _root = gtk_label_new("");
  g_object_ref_sink(_root);
}

ChatView::~ChatView() {
  g_object_unref(_root);
}

void ChatView::showAt(std::int64_t) {}
bool ChatView::handleEscape() {
  return false;
}

}  // namespace Contrail
