#include "chat_view.hpp"

#include "main_window.hpp"
#include "message.hpp"

namespace Contrail {
namespace {

void OpenPath(const std::string &path) {
  if (auto *uri = g_filename_to_uri(path.c_str(), nullptr, nullptr)) {
    OpenUri(uri);
    g_free(uri);
  }
}

}  // namespace

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

void ChatView::openChat(Td::ChatId chat) {
  _window.openChat(chat);
}

// There is no photo viewer yet: the photo opens outside, once the row has it.
void ChatView::openPhoto(const MessageItem &, Td::FileId file) {
  if (const auto known = _store.file(file); known && known->complete) OpenPath(known->path);
}

void ChatView::openDocument(const MessageItem &, Td::FileId file) {
  const auto known = _store.file(file);
  if (known && known->complete) {
    OpenPath(known->path);
  } else if (known && known->downloading) {
    _store.cancelDownload(file);
  } else {
    _store.download(file, 32);
  }
}

bool ChatView::opensWhenLoaded(Td::FileId) const {
  return false;
}

void ChatView::showMessageMenu(const MessageItem &, GtkWidget *, double, double) {}
void ChatView::replyTo(const MessageItem &) {}

// Not loaded, until this view holds messages.
ReplyPreview ChatView::replyPreview(const MessageItem &) const {
  return {};
}

void ChatView::sendText(const std::string &) {}

}  // namespace Contrail
