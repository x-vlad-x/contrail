#pragma once

#include "app.hpp"
#include "td/client.hpp"

#include <cstddef>
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

}  // namespace Contrail
