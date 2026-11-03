#pragma once

#include "app.hpp"
#include "td/client.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Contrail {

// TDLib counts entities in UTF-16 units, Pango in UTF-8 bytes. The byte offset in `text` of the UTF-16 offset:
// past the end it is the end, inside a surrogate pair the start of that character.
[[nodiscard]] std::size_t Utf8Offset(std::string_view text, int utf16Offset);

// Telegram's formatted text -> Pango markup for a GtkLabel. Links become <a href>; mentions, hashtags
// and spoilers get internal hrefs ("tg-mention:", "tg-user:", "tg-hashtag:", "tg-command:", "spoiler:")
// that HandleLink() understands. Without `links` (one-line previews, nothing to tap) they stay plain text:
// a GtkLabel with any link claims every touch on it and the list under it can't be scrolled there.
[[nodiscard]] std::string ToMarkup(const Td::api::formattedText &text, bool revealSpoilers = false, bool links = true);

// A wrapping label for a message text. Opens links and reveals spoilers on tap.
GtkWidget *MakeTextLabel(const Td::api::formattedText &text);
GtkWidget *MakeTextLabel(const std::string &text);

// Opens a link from a message: web links in the browser, mentions in the app.
void HandleLink(const std::string &href);

// Telegram's own links (t.me, tg://) that a message has: the chats and messages they lead to open here, the rest in
// the browser. The whole set of Telegram's links (invites, stickers, proxies, settings...) comes with their routing
// later: this is where it goes.
[[nodiscard]] bool IsTelegramLink(const std::string &href);
void OpenTelegramLink(const std::string &href);

// For the debug commands: a tap on the link or the spoiler of a message's label whose text has `part`, as a finger
// does it. False when this label has none.
bool DebugTapLabel(GtkWidget *label, const std::string &part);

// A count as Telegram shortens it: 999, 1.2K, 3M.
[[nodiscard]] std::string ShortCount(std::int64_t count);

}  // namespace Contrail
