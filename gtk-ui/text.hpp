#pragma once

#include "app.hpp"

#include "ui/text/text_entity.h"

namespace Contrail {

// Telegram's formatted text -> Pango markup for a GtkLabel. Links become <a href>; mentions, hashtags
// and spoilers get internal hrefs ("tg-mention:", "tg-user:", "tg-hashtag:", "tg-command:", "spoiler:")
// that HandleLink() understands. Without `links` (one-line previews, nothing to tap) they stay plain text:
// a GtkLabel with any link claims every touch on it and the list under it can't be scrolled there.
std::string ToMarkup(const TextWithEntities &text, bool revealSpoilers = false, bool links = true);

// A wrapping label for a message text. Opens links and reveals spoilers on tap.
GtkWidget *MakeTextLabel(const TextWithEntities &text);
void SetLabelText(GtkWidget *label, const TextWithEntities &text);

// Opens a link from a message: web links in the browser, mentions and t.me links in the app.
void HandleLink(const QString &href);

QString FormatTime(TimeId date);       // 12:34
QString FormatListDate(TimeId date);   // chat list: time today, weekday this week, date otherwise
QString FormatDay(TimeId date);        // date separator in a chat: "October 13"

// For the few texts Telegram Desktop has no strings for (features of the phone apps): English, or Russian or
// Ukrainian when that is the interface's language.
QString Translated(const char *english, const char *russian, const char *ukrainian);

}  // namespace Contrail
