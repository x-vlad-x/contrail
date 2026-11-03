#include "text.hpp"

#include "chat_view.hpp"
#include "layout.hpp"
#include "main_window.hpp"
#include "proxy.hpp"
#include "scroll_helpers.hpp"
#include "td/strings.hpp"

#include <algorithm>
#include <memory>
#include <set>
#include <vector>

namespace Contrail {
namespace {

namespace api = Td::api;

constexpr char kSpoilerHref[] = "spoiler:";

// In UTF-8 bytes of the text.
struct Tag {
  std::size_t start = 0;
  std::size_t end = 0;
  std::string open;
  std::string close;
  std::string href;  // Links, and spoilers to be tapped.
  bool spoiler = false;
};

// The tags of a text with spoilers hidden and links in place; what a label shows is chosen from these.
std::vector<Tag> CollectTags(const api::formattedText &text) {
  auto tags = std::vector<Tag>();
  for (const auto &entity : text.entities_) {
    if (!entity || !entity->type_ || entity->length_ <= 0 || entity->offset_ < 0) continue;
    const auto start = Utf8Offset(text.text_, entity->offset_);
    const auto end = Utf8Offset(text.text_, entity->offset_ + entity->length_);
    if (start >= end) continue;
    const auto part = text.text_.substr(start, end - start);
    const auto add = [&](std::string open, std::string close) {
      tags.push_back({start, end, std::move(open), std::move(close)});
    };
    const auto link = [&](std::string href) {
      add("<a href=\"" + Escape(href) + "\">", "</a>");
      tags.back().href = std::move(href);
    };
    switch (entity->type_->get_id()) {
      case api::textEntityTypeBold::ID: add("<b>", "</b>"); break;
      case api::textEntityTypeItalic::ID: add("<i>", "</i>"); break;
      case api::textEntityTypeUnderline::ID: add("<u>", "</u>"); break;
      case api::textEntityTypeStrikethrough::ID: add("<s>", "</s>"); break;
      case api::textEntityTypeCode::ID:
      case api::textEntityTypePre::ID:
      case api::textEntityTypePreCode::ID: add("<tt>", "</tt>"); break;
      case api::textEntityTypeBlockQuote::ID:
      case api::textEntityTypeExpandableBlockQuote::ID: add("<i>", "</i>"); break;
      case api::textEntityTypeUrl::ID:
        link(g_ascii_strncasecmp(part.c_str(), "http", 4) == 0 ? part : "https://" + part);
        break;
      case api::textEntityTypeTextUrl::ID: link(static_cast<const api::textEntityTypeTextUrl &>(*entity->type_).url_); break;
      case api::textEntityTypeEmailAddress::ID: link("mailto:" + part); break;
      case api::textEntityTypePhoneNumber::ID: link("tel:" + part); break;
      case api::textEntityTypeMention::ID: link("tg-mention:" + part); break;
      case api::textEntityTypeMentionName::ID:
        link("tg-user:" + std::to_string(static_cast<const api::textEntityTypeMentionName &>(*entity->type_).user_id_));
        break;
      case api::textEntityTypeHashtag::ID:
      case api::textEntityTypeCashtag::ID: link("tg-hashtag:" + part); break;
      case api::textEntityTypeBotCommand::ID: link("tg-command:" + part); break;
      case api::textEntityTypeSpoiler::ID:
        add("<span background=\"#808080\" foreground=\"#808080\">", "</span>");
        tags.back().spoiler = true;
        break;
      default: break;
    }
  }
  return tags;
}

std::vector<Tag> ShownTags(std::vector<Tag> tags, bool revealSpoilers, bool links) {
  std::erase_if(tags, [&](const Tag &tag) {
    return tag.spoiler ? revealSpoilers : (!links && !tag.href.empty());
  });
  if (links) {
    for (auto &tag : tags) {
      if (!tag.spoiler) continue;
      tag.open = "<a href=\"spoiler:\"><span background=\"#808080\" foreground=\"#808080\" underline=\"none\">";
      tag.close = "</span></a>";
      tag.href = kSpoilerHref;
    }
  }
  return tags;
}

std::string MarkupOf(const std::string &text, const std::vector<Tag> &tags) {
  if (tags.empty()) return Escape(text);
  // Split the text at every tag boundary and wrap each piece into the tags covering it: Pango needs
  // strictly nested tags, Telegram entities can overlap.
  auto bounds = std::set<std::size_t>{0, text.size()};
  for (const auto &tag : tags) {
    bounds.insert(tag.start);
    bounds.insert(tag.end);
  }
  auto result = std::string();
  for (auto it = bounds.begin(); it != bounds.end(); ++it) {
    const auto next = std::next(it);
    if (next == bounds.end()) break;
    const auto from = *it, till = *next;
    auto active = std::vector<const Tag *>();
    auto hasLink = false;
    for (const auto &tag : tags) {
      if (tag.start > from || tag.end < till) continue;
      // Links can't be nested in Pango: keep only the outermost one.
      if (tag.open.starts_with("<a ")) {
        if (hasLink) continue;
        hasLink = true;
      }
      active.push_back(&tag);
    }
    for (const auto *tag : active) result += tag->open;
    result += Escape(text.substr(from, till - from));
    for (auto i = active.rbegin(); i != active.rend(); ++i) result += (*i)->close;
  }
  return result;
}

struct LabelText {
  std::string text;
  std::vector<Tag> tags;
  bool revealed = false;
};

const Tag *LinkAt(GtkWidget *label, const LabelText &data, double x, double y) {
  auto offsetX = 0, offsetY = 0;
  gtk_label_get_layout_offsets(GTK_LABEL(label), &offsetX, &offsetY);
  auto index = 0, trailing = 0;
  if (!pango_layout_xy_to_index(gtk_label_get_layout(GTK_LABEL(label)), int((x - offsetX) * PANGO_SCALE),
                                int((y - offsetY) * PANGO_SCALE), &index, &trailing)) {
    return nullptr;
  }
  // A hidden spoiler over a link is a spoiler first.
  const Tag *found = nullptr;
  for (const auto &tag : data.tags) {
    if (std::size_t(index) < tag.start || std::size_t(index) >= tag.end) continue;
    if (tag.spoiler && !data.revealed) return &tag;
    if (!tag.href.empty() && !tag.spoiler && !found) found = &tag;
  }
  return found;
}

void ApplyLabelText(GtkWidget *label, LabelText *data) {
  gtk_label_set_markup(GTK_LABEL(label), MarkupOf(data->text, ShownTags(data->tags, data->revealed, true)).c_str());
  DisableLabelGestures(label);
}

// A tap on a spoiler reveals the label's spoilers, on a link opens it.
void TapTag(GtkWidget *label, LabelText *data, const Tag &tag) {
  if (tag.spoiler && !data->revealed) {
    data->revealed = true;
    ApplyLabelText(label, data);
    return;
  }
  const auto href = tag.href;  // HandleLink() may destroy the label (another chat opens).
  if (!href.empty() && !tag.spoiler) HandleLink(href);
}

[[nodiscard]] std::string Lowercase(std::string text) {
  for (auto &c : text) c = char(g_ascii_tolower(c));
  return text;
}

GtkWidget *MakeLabel(std::string text, std::vector<Tag> tags) {
  auto *label = gtk_label_new(nullptr);
  ConfigureMessageWrapping(GTK_LABEL(label));
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_selectable(GTK_LABEL(label), FALSE);
  gtk_widget_set_focusable(label, FALSE);
  gtk_widget_add_css_class(label, "message-text");
  auto data = std::make_unique<LabelText>();
  data->text = std::move(text);
  data->tags = std::move(tags);
  auto *raw = AttachData(label, "contrail-text", std::move(data));
  if (raw->tags.empty()) {
    gtk_label_set_text(GTK_LABEL(label), raw->text.c_str());
    return label;
  }
  // Doesn't claim the touch: the scrolled chat takes it as soon as the finger moves.
  auto *click = gtk_gesture_click_new();
  g_object_set_data(G_OBJECT(click), "contrail-own", GINT_TO_POINTER(1));
  gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(click));
  ApplyLabelText(label, raw);
  Connect<void(GtkGestureClick *, int, double, double)>(click, "released",
      [label, raw](GtkGestureClick *, int, double x, double y) {
        if (const auto *link = TapAllowed() ? LinkAt(label, *raw, x, y) : nullptr) TapTag(label, raw, *link);
      });
  return label;
}

}  // namespace

std::size_t Utf8Offset(std::string_view text, int utf16Offset) {
  auto units = 0;
  auto index = std::size_t(0);
  while (index < text.size() && units < utf16Offset) {
    const auto lead = static_cast<unsigned char>(text[index]);
    const auto length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    const auto width = length == 4 ? 2 : 1;  // Beyond the first plane: a surrogate pair.
    if (units + width > utf16Offset) break;
    units += width;
    index = std::min(index + length, text.size());
  }
  return index;
}

std::string ToMarkup(const api::formattedText &text, bool revealSpoilers, bool links) {
  return MarkupOf(text.text_, ShownTags(CollectTags(text), revealSpoilers, links));
}

GtkWidget *MakeTextLabel(const api::formattedText &text) {
  return MakeLabel(text.text_, CollectTags(text));
}

GtkWidget *MakeTextLabel(const std::string &text) {
  return MakeLabel(text, {});
}

void HandleLink(const std::string &href) {
  // Only until the next link: an answer for a link tapped before does nothing after it.
  static auto lifetime = Td::Lifetime();
  auto *window = GetApp().window;
  auto *store = window ? window->store() : nullptr;
  if (!store) return;
  const auto open = [window](Td::api::chat &chat) { window->openChat(chat.id_); };
  const auto failed = [](api::error &error) { ShowToast(error.message_); };
  if (href.starts_with("tg-mention:")) {
    auto name = href.substr(std::string_view("tg-mention:").size());
    if (name.starts_with('@')) name.erase(0, 1);
    lifetime.reset();
    store->client().request<api::chat>(api::make_object<api::searchPublicChat>(name), lifetime, open, failed);
  } else if (href.starts_with("tg-user:")) {
    const auto id = std::atoll(href.c_str() + std::string_view("tg-user:").size());
    lifetime.reset();
    store->client().request<api::chat>(api::make_object<api::createPrivateChat>(id, false), lifetime, open, failed);
  } else if (href.starts_with("tg-hashtag:")) {
    // As in 0.3.1: the chat list searches for it.
    window->searchChats(href.substr(std::string_view("tg-hashtag:").size()));
  } else if (href.starts_with("tg-command:")) {
    if (auto *chat = window->chatView()) chat->sendText(href.substr(std::string_view("tg-command:").size()));
  } else if (IsTelegramLink(href)) {
    OpenTelegramLink(href);
  } else {
    OpenUri(href);
  }
}

bool IsTelegramLink(const std::string &href) {
  auto link = Lowercase(href);
  if (link.starts_with("tg://") || link.starts_with("tg:")) return true;
  for (const auto *scheme : {"https://", "http://"}) {
    if (link.starts_with(scheme)) link.erase(0, std::string_view(scheme).size());
  }
  if (link.starts_with("www.")) link.erase(0, 4);
  for (const std::string_view host : {"t.me", "telegram.me", "telegram.dog"}) {
    if (link.starts_with(host) && (link.size() == host.size() || link[host.size()] == '/')) return true;
  }
  return false;
}

void OpenTelegramLink(const std::string &href) {
  // Only until the next link, as in HandleLink().
  static auto lifetime = Td::Lifetime();
  auto *window = GetApp().window;
  auto *store = window ? window->store() : nullptr;
  if (!store) return;
  lifetime.reset();
  auto &client = store->client();
  // What isn't routed here yet: a web link goes to the browser (which may hand it back), a tg:// one can't.
  const auto elsewhere = [href] {
    if (Lowercase(href).starts_with("tg:")) {
      ShowToast(Translated("This link can't be opened yet", "Эта ссылка пока не открывается",
                           "Це посилання поки не відкривається"));
    } else {
      OpenUri(href);
    }
  };
  const auto openChat = [window](api::chat &chat) { window->openChat(chat.id_); };
  const auto failed = [](api::error &error) { ShowToast(error.message_); };
  client.send(
      api::make_object<api::getInternalLinkType>(href),
      [window, &client, elsewhere, openChat, failed](Td::ObjectPtr object) {
        if (!object) return;
        switch (object->get_id()) {
          case api::internalLinkTypePublicChat::ID: {
            const auto &type = static_cast<api::internalLinkTypePublicChat &>(*object);
            client.request<api::chat>(api::make_object<api::searchPublicChat>(type.chat_username_), lifetime,
                                      openChat, failed);
            break;
          }
          case api::internalLinkTypeBotStart::ID: {
            // The bot's chat opens; its start button is the user's to press.
            const auto &type = static_cast<api::internalLinkTypeBotStart &>(*object);
            client.request<api::chat>(api::make_object<api::searchPublicChat>(type.bot_username_), lifetime,
                                      openChat, failed);
            break;
          }
          case api::internalLinkTypeMessage::ID: {
            const auto &type = static_cast<api::internalLinkTypeMessage &>(*object);
            client.request<api::messageLinkInfo>(
                api::make_object<api::getMessageLinkInfo>(type.url_), lifetime,
                [window](api::messageLinkInfo &info) {
                  const auto message = info.message_ ? info.message_->id_ : 0;
                  const auto forum = info.topic_id_ && info.topic_id_->get_id() == api::messageTopicForum::ID
                                         ? static_cast<const api::messageTopicForum *>(info.topic_id_.get())
                                         : nullptr;
                  if (forum && forum->forum_topic_id_) {
                    window->openTopic(info.chat_id_, forum->forum_topic_id_, message);
                  } else if (info.chat_id_) {
                    window->openChat(info.chat_id_, message);
                  }
                },
                failed);
            break;
          }
          case api::internalLinkTypeUserPhoneNumber::ID: {
            const auto &type = static_cast<api::internalLinkTypeUserPhoneNumber &>(*object);
            client.request<api::user>(
                api::make_object<api::searchUserByPhoneNumber>(type.phone_number_, false), lifetime,
                [&client, openChat, failed](api::user &user) {
                  client.request<api::chat>(api::make_object<api::createPrivateChat>(user.id_, false), lifetime,
                                            openChat, failed);
                },
                failed);
            break;
          }
          case api::internalLinkTypeProxy::ID:
            ConfirmAddProxy(*window, std::move(static_cast<api::internalLinkTypeProxy &>(*object).proxy_));
            break;
          default: elsewhere(); break;
        }
      },
      lifetime);
}

bool DebugTapLabel(GtkWidget *label, const std::string &part) {
  auto *data = static_cast<LabelText *>(g_object_get_data(G_OBJECT(label), "contrail-text"));
  if (!data || part.empty()) return false;
  const Tag *found = nullptr;
  for (const auto &tag : data->tags) {
    const auto hidden = tag.spoiler && !data->revealed;
    if ((!hidden && (tag.href.empty() || tag.spoiler))
        || data->text.substr(tag.start, tag.end - tag.start).find(part) == std::string::npos) {
      continue;
    }
    // A hidden spoiler over a link is a spoiler first, as for a finger.
    if (!found || hidden) found = &tag;
    if (hidden) break;
  }
  if (!found) return false;
  g_message("chat tap: %s \"%s\"", (found->spoiler && !data->revealed) ? "spoiler" : found->href.c_str(),
            data->text.substr(found->start, found->end - found->start).c_str());
  TapTag(label, data, *found);
  return true;
}

std::string ShortCount(std::int64_t count) {
  const auto shorten = [](std::int64_t value, std::int64_t unit, const char *suffix) {
    const auto tenths = value * 10 / unit;
    auto result = std::to_string(tenths / 10);
    if (tenths % 10) result += "." + std::to_string(tenths % 10);
    return result + suffix;
  };
  if (count >= 1'000'000) return shorten(count, 1'000'000, "M");
  if (count >= 1'000) return shorten(count, 1'000, "K");
  return std::to_string(count);
}

}  // namespace Contrail
