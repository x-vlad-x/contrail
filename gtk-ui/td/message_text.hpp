// Telegram Desktop's core wrote these texts; TDLib gives only the data. One place turns a message into text.
#pragma once

#include "td/store.hpp"

#include <string>

namespace Contrail::Td {

// A line for the chat list, a reply or a pinned bar: "Photo", "Voice message", the text, a service message.
[[nodiscard]] std::string MessagePreview(const api::message &message, const Store &store);

}  // namespace Contrail::Td
