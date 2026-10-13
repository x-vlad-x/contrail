#include "text.hpp"

#include "main_window.hpp"

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
};

std::string Attr(const QString &s) {
  return Escape(s);
}

std::vector<Tag> CollectTags(const TextWithEntities &text, bool revealSpoilers) {
  std::vector<Tag> tags;
  const auto add = [&](const EntityInText &e, std::string open, std::string close) {
    tags.push_back({e.offset(), e.offset() + e.length(), std::move(open), std::move(close)});
  };
  const auto link = [&](const EntityInText &e, const QString &href) {
    add(e, "<a href=\"" + Attr(href) + "\">", "</a>");
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
        if (!revealSpoilers) {
          add(e, "<a href=\"spoiler:\"><span background=\"#808080\" foreground=\"#808080\" underline=\"none\">",
              "</span></a>");
        }
        break;
      default: break;
    }
  }
  return tags;
}

}  // namespace

std::string ToMarkup(const TextWithEntities &text, bool revealSpoilers) {
  const auto tags = CollectTags(text, revealSpoilers);
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
};

}  // namespace

void SetLabelText(GtkWidget *label, const TextWithEntities &text) {
  auto *data = static_cast<LabelText *>(g_object_get_data(G_OBJECT(label), "contrail-text"));
  if (!data) {
    data = AttachData(label, "contrail-text", std::make_unique<LabelText>());
  }
  data->text = text;
  data->revealed = false;
  gtk_label_set_markup(GTK_LABEL(label), ToMarkup(text).c_str());
}

GtkWidget *MakeTextLabel(const TextWithEntities &text) {
  auto *label = gtk_label_new(nullptr);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_selectable(GTK_LABEL(label), TRUE);
  gtk_widget_set_focusable(label, FALSE);
  gtk_widget_add_css_class(label, "message-text");
  SetLabelText(label, text);
  Connect<gboolean(GtkLabel *, const char *)>(label, "activate-link", [](GtkLabel *label, const char *uri) -> gboolean {
    const auto href = Q(uri);
    if (href == u"spoiler:"_q) {
      if (auto *data = static_cast<LabelText *>(g_object_get_data(G_OBJECT(label), "contrail-text"))) {
        data->revealed = true;
        gtk_label_set_markup(label, ToMarkup(data->text, true).c_str());
      }
      return TRUE;
    }
    HandleLink(href);
    return TRUE;
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
  const auto day = base::unixtime::parse(date).date();
  const auto today = QDate::currentDate();
  return (day.year() == today.year()) ? langDayOfMonthFull(day) : langDayOfMonthFull(day) + ' ' + QString::number(day.year());
}

}  // namespace Contrail
