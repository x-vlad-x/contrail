// Telegram Desktop's core wrote these texts; TDLib gives only the data. One place turns a message into text.
#pragma once

#include "td/store.hpp"

#include <string>

namespace Contrail::Td {

// How a chat shows a message. Fallback is drawn like a normal message, with its preview as text: there is nothing
// better to show for it. Unknown is a type this table doesn't have; a test keeps it from happening.
enum class ContentKind { Unknown, Shown, Service, Fallback };

[[nodiscard]] ContentKind ClassifyContent(const api::MessageContent &content);
[[nodiscard]] bool IsServiceMessage(const api::MessageContent &content);

// Some service messages speak about another message: a pin, a game score, a checklist. The store doesn't hold
// messages, so whoever has that message passes it as `related`; without it the text is more general.

// A line for the chat list, a reply or a pinned bar: "Photo, caption", "Voice message", the text, a service message.
[[nodiscard]] std::string MessagePreview(const api::message &message, const Store &store,
                                         const api::message *related = nullptr);

// The centered text of a service message, empty for any other. A service message can have none too (a group became
// a supergroup): Telegram Desktop shows no line for those, so check IsServiceMessage() first and hide them.
[[nodiscard]] std::string ServiceText(const api::message &message, const Store &store,
                                      const api::message *related = nullptr);

// Who to name before a preview, without the colon: "You", "Anna". Empty where Telegram Desktop names nobody: private
// chats, channels, service messages. In Saved Messages it is the author of what was forwarded.
[[nodiscard]] std::string SenderPrefix(const api::message &message, const Store &store);

}  // namespace Contrail::Td
