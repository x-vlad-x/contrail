#pragma once

#include "app.hpp"

#include "ui/text/text_entity.h"

namespace Contrail {

// Telegram's formatted text -> Pango markup for a GtkLabel. Links become <a href>; mentions, hashtags
// and spoilers get internal hrefs ("tg-mention:", "tg-user:", "tg-hashtag:", "tg-command:", "spoiler:")
// that HandleLink() understands.
std::string ToMarkup(const TextWithEntities &text, bool revealSpoilers = false);

// A selectable, wrapping label for a message text. Opens links and reveals spoilers on click.
GtkWidget *MakeTextLabel(const TextWithEntities &text);
void SetLabelText(GtkWidget *label, const TextWithEntities &text);

// Opens a link from a message: web links in the browser, mentions and t.me links in the app.
void HandleLink(const QString &href);

QString FormatTime(TimeId date);       // 12:34
QString FormatListDate(TimeId date);   // chat list: time today, weekday this week, date otherwise
QString FormatDay(TimeId date);        // date separator in a chat: "October 13"

}  // namespace Contrail
