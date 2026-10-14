#include "text.hpp"

#include "main_window.hpp"
#include "layout.hpp"
#include "scroll_helpers.hpp"

#include "base/unixtime.h"
#include "core/application.h"
#include "core/local_url_handlers.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/text/format_values.h"
#include "ui/text/text_utilities.h"

#include <QtCore/QDateTime>
#include <QtCore/QLocale>

#include <algorithm>
#include <set>
#include <vector>

namespace Contrail {
namespace {

struct Tag {
  int start = 0;
  int end = 0;
  std::string open;
  std::string close;
  QString href;  // Links only.
};

std::string Attr(const QString &s) {
  return Escape(s);
}

std::vector<Tag> CollectTags(const TextWithEntities &text, bool revealSpoilers, bool links) {
  std::vector<Tag> tags;
  const auto add = [&](const EntityInText &e, std::string open, std::string close) {
    tags.push_back({e.offset(), e.offset() + e.length(), std::move(open), std::move(close)});
  };
  const auto link = [&](const EntityInText &e, const QString &href) {
    if (links) {
      add(e, "<a href=\"" + Attr(href) + "\">", "</a>");
      tags.back().href = href;
    }
  };
  for (const auto &e : text.entities) {
    if (e.length() <= 0 || e.offset() < 0 || e.offset() + e.length() > text.text.size()) {
      continue;
    }
    const auto part = text.text.mid(e.offset(), e.length());
    switch (e.type()) {
      case EntityType::Bold:
      case EntityType::Semibold: add(e, "<b>", "</b>"); break;
      case EntityType::Italic: add(e, "<i>", "</i>"); break;
      case EntityType::Underline: add(e, "<u>", "</u>"); break;
      case EntityType::StrikeOut: add(e, "<s>", "</s>"); break;
      case EntityType::Code:
      case EntityType::Pre: add(e, "<tt>", "</tt>"); break;
      case EntityType::Subscript: add(e, "<sub>", "</sub>"); break;
      case EntityType::Superscript: add(e, "<sup>", "</sup>"); break;
      case EntityType::Blockquote: add(e, "<i>", "</i>"); break;
      case EntityType::Colorized: add(e, "<span weight=\"600\">", "</span>"); break;
      case EntityType::Url: link(e, part.startsWith(u"http"_q, Qt::CaseInsensitive) ? part : (u"https://"_q + part)); break;
      case EntityType::CustomUrl: link(e, e.data()); break;
      case EntityType::Email: link(e, u"mailto:"_q + part); break;
      case EntityType::Phone: link(e, u"tel:"_q + part); break;
      case EntityType::Mention: link(e, u"tg-mention:"_q + part); break;
      case EntityType::MentionName: link(e, u"tg-user:"_q + e.data()); break;
      case EntityType::Hashtag:
      case EntityType::Cashtag: link(e, u"tg-hashtag:"_q + part); break;
      case EntityType::BotCommand: link(e, u"tg-command:"_q + part); break;
      case EntityType::Spoiler:
        if (!revealSpoilers && links) {
          add(e, "<a href=\"spoiler:\"><span background=\"#808080\" foreground=\"#808080\" underline=\"none\">",
              "</span></a>");
          tags.back().href = u"spoiler:"_q;
        } else if (!revealSpoilers) {
          add(e, "<span background=\"#808080\" foreground=\"#808080\">", "</span>");
        }
        break;
      default: break;
    }
  }
  return tags;
}

}  // namespace

std::string ToMarkup(const TextWithEntities &text, bool revealSpoilers, bool links) {
  const auto tags = CollectTags(text, revealSpoilers, links);
  if (tags.empty()) {
    return Escape(text.text);
  }
  // Split the text at every tag boundary and wrap each piece into the tags covering it: Pango needs
  // strictly nested tags, Telegram entities can overlap.
  std::set<int> bounds = {0, int(text.text.size())};
  for (const auto &tag : tags) {
    bounds.insert(tag.start);
    bounds.insert(tag.end);
  }
  std::string result;
  for (auto it = bounds.begin(); it != bounds.end(); ++it) {
    const auto next = std::next(it);
    if (next == bounds.end()) {
      break;
    }
    const auto from = *it, till = *next;
    std::vector<const Tag *> active;
    for (const auto &tag : tags) {
      if (tag.start <= from && tag.end >= till) {
        active.push_back(&tag);
      }
    }
    // Links can't be nested in Pango: keep only the outermost one.
    auto hasLink = false;
    std::erase_if(active, [&](const Tag *tag) {
      if (tag->open.rfind("<a ", 0) == 0) {
        if (hasLink) {
          return true;
        }
        hasLink = true;
      }
      return false;
    });
    for (const auto *tag : active) {
      result += tag->open;
    }
    result += Escape(text.text.mid(from, till - from));
    for (auto i = active.rbegin(); i != active.rend(); ++i) {
      result += (*i)->close;
    }
  }
  return result;
}

namespace {

struct LabelText {
  TextWithEntities text;
  bool revealed = false;
  // The links in the label's text, in UTF-8 bytes of the Pango layout.
  struct Link {
    int from = 0;
    int till = 0;
    QString href;
  };
  std::vector<Link> links;
};

const LabelText::Link *LinkAt(GtkWidget *label, const LabelText &data, double x, double y) {
  auto offsetX = 0, offsetY = 0;
  gtk_label_get_layout_offsets(GTK_LABEL(label), &offsetX, &offsetY);
  auto index = 0, trailing = 0;
  if (!pango_layout_xy_to_index(gtk_label_get_layout(GTK_LABEL(label)), int((x - offsetX) * PANGO_SCALE),
                                int((y - offsetY) * PANGO_SCALE), &index, &trailing)) {
    return nullptr;
  }
  for (const auto &link : data.links) {
    if (index >= link.from && index < link.till) {
      return &link;
    }
  }
  return nullptr;
}

void ApplyLabelText(GtkWidget *label, LabelText *data) {
  data->links.clear();
  const auto utf8Offset = [&](int offset) { return int(data->text.text.left(offset).toUtf8().size()); };
  for (const auto &tag : CollectTags(data->text, data->revealed, true)) {
    if (!tag.href.isEmpty()) {
      data->links.push_back({utf8Offset(tag.start), utf8Offset(tag.end), tag.href});
    }
  }
  gtk_label_set_markup(GTK_LABEL(label), ToMarkup(data->text, data->revealed).c_str());
  DisableLabelGestures(label);
}

}  // namespace

void SetLabelText(GtkWidget *label, const TextWithEntities &text) {
  auto *data = static_cast<LabelText *>(g_object_get_data(G_OBJECT(label), "contrail-text"));
  if (!data) {
    data = AttachData(label, "contrail-text", std::make_unique<LabelText>());
  }
  data->text = text;
  data->revealed = false;
  ApplyLabelText(label, data);
}

GtkWidget *MakeTextLabel(const TextWithEntities &text) {
  auto *label = gtk_label_new(nullptr);
  ConfigureMessageWrapping(GTK_LABEL(label));
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_selectable(GTK_LABEL(label), FALSE);
  gtk_widget_set_focusable(label, FALSE);
  gtk_widget_add_css_class(label, "message-text");
  // Doesn't claim the touch: the scrolled chat takes it as soon as the finger moves.
  auto *click = gtk_gesture_click_new();
  g_object_set_data(G_OBJECT(click), "contrail-own", GINT_TO_POINTER(1));
  gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(click));
  SetLabelText(label, text);
  Connect<void(GtkGestureClick *, int, double, double)>(click, "released", [label](GtkGestureClick *, int, double x, double y) {
    auto *data = static_cast<LabelText *>(g_object_get_data(G_OBJECT(label), "contrail-text"));
    const auto link = (data && TapAllowed()) ? LinkAt(label, *data, x, y) : nullptr;
    if (!link) {
      return;
    }
    if (link->href == u"spoiler:"_q) {
      data->revealed = true;
      ApplyLabelText(label, data);
      return;
    }
    const auto href = link->href;  // HandleLink() may destroy the label (another chat opens).
    HandleLink(href);
  });
  return label;
}

void HandleLink(const QString &href) {
  auto *window = GetApp().window;
  if (href.startsWith(u"tg-mention:"_q)) {
    const auto name = href.mid(11).mid(href.mid(11).startsWith('@') ? 1 : 0);
    Core::App().openLocalUrl(u"tg://resolve?domain="_q + name, QVariant());
  } else if (href.startsWith(u"tg-user:"_q)) {
    const auto fields = TextUtilities::MentionNameDataToFields(href.mid(8));
    if (window && window->session() && fields.userId) {
      if (const auto user = window->session()->data().userLoaded(UserId(fields.userId))) {
        window->openPeer(user);
      }
    }
  } else if (href.startsWith(u"tg-hashtag:"_q)) {
    if (window) {
      window->searchChats(href.mid(11));
    }
  } else if (href.startsWith(u"tg-command:"_q)) {
    if (window) {
      window->sendBotCommand(href.mid(11));
    }
  } else if (const auto local = Core::TryConvertUrlToLocal(href); local.startsWith(u"tg://"_q, Qt::CaseInsensitive)) {
    // t.me links: Telegram Desktop resolves them and opens the chat, which the GTK window follows.
    Core::App().openLocalUrl(local, QVariant());
  } else {
    OpenUri(U8(href));
  }
}

QString FormatTime(TimeId date) {
  return QLocale().toString(base::unixtime::parse(date).time(), QLocale::ShortFormat);
}

QString FormatListDate(TimeId date) {
  return Ui::FormatDialogsDate(base::unixtime::parse(date));
}

QString FormatDay(TimeId date) {
  // Adds the year itself for the past years.
  return langDayOfMonthFull(base::unixtime::parse(date).date());
}

}  // namespace Contrail
