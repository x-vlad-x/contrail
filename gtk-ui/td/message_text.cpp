#include "td/message_text.hpp"

namespace Contrail::Td {

std::string MessagePreview(const api::message &message, const Store &store) {
  if (message.content_ && message.content_->get_id() == api::messageText::ID) {
    return static_cast<const api::messageText &>(*message.content_).text_->text_;
  }
  return {};
}

}  // namespace Contrail::Td
