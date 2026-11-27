#include "td/chat_row.hpp"

#include "td/strings.hpp"

#include <glib.h>

namespace Contrail::Td {
namespace {

std::string Folded(const std::string &text) {
  auto *folded = g_utf8_casefold(text.c_str(), -1);
  auto result = std::string(folded);
  g_free(folded);
  return result;
}

std::string FirstName(const Store &store, UserId id) {
  const auto user = store.user(id);
  if (!user) return {};
  return user->firstName.empty() ? DisplayName(*user) : user->firstName;
}

// Choosing a location or a contact reads as typing, as in Telegram Desktop.
std::string ActionText(std::int32_t action, const std::string &name) {
  const auto pick = [&](const char *alone, const char *named) {
    return name.empty() ? Tr(alone) : Tr(named, {{"user", name}});
  };
  switch (action) {
    case api::chatActionRecordingVideo::ID: return pick("lng_send_action_record_video", "lng_user_action_record_video");
    case api::chatActionUploadingVideo::ID: return pick("lng_send_action_upload_video", "lng_user_action_upload_video");
    case api::chatActionRecordingVoiceNote::ID:
      return pick("lng_send_action_record_audio", "lng_user_action_record_audio");
    case api::chatActionUploadingVoiceNote::ID:
      return pick("lng_send_action_upload_audio", "lng_user_action_upload_audio");
    case api::chatActionRecordingVideoNote::ID:
      return pick("lng_send_action_record_round", "lng_user_action_record_round");
    case api::chatActionUploadingVideoNote::ID:
      return pick("lng_send_action_upload_round", "lng_user_action_upload_round");
    case api::chatActionUploadingPhoto::ID: return pick("lng_send_action_upload_photo", "lng_user_action_upload_photo");
    case api::chatActionUploadingDocument::ID: return pick("lng_send_action_upload_file", "lng_user_action_upload_file");
    case api::chatActionChoosingSticker::ID:
      return pick("lng_send_action_choose_sticker", "lng_user_action_choose_sticker");
    case api::chatActionChoosingLocation::ID:
    case api::chatActionChoosingContact::ID: return pick("lng_typing", "lng_user_typing");
    default: return {};
  }
}

}  // namespace

std::vector<std::string> SearchWords(const std::string &query) {
  auto result = std::vector<std::string>();
  const auto folded = Folded(query);
  auto start = std::size_t(0);
  while (start < folded.size()) {
    const auto end = std::min(folded.find(' ', start), folded.size());
    if (end > start) result.push_back(folded.substr(start, end - start));
    start = end + 1;
  }
  return result;
}

bool MatchesSearch(const std::string &title, const std::string &username, const std::vector<std::string> &words) {
  const auto name = Folded(title);
  const auto nick = Folded(username);
  return std::ranges::all_of(words, [&](const std::string &word) {
    return name.find(word) != std::string::npos || nick.starts_with(word);
  });
}

std::string OneLine(const std::string &text, std::size_t limit) {
  auto end = std::min(limit, text.size());
  // The first byte left out must start a character, or the last one kept would be cut in half.
  while (end > 0 && end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
  auto result = text.substr(0, end);
  std::ranges::replace(result, '\n', ' ');
  std::ranges::replace(result, '\r', ' ');
  return result;
}

bool ChatIsUnread(const Chat &chat) {
  return chat.unread > 0 || chat.markedUnread || chat.unreadMentions > 0 || chat.unreadReactions > 0;
}

Badge ChatBadge(const Chat &chat) {
  if (!ChatIsUnread(chat)) return {};
  auto badge = Badge{.visible = true, .muted = chat.muted && chat.unreadMentions == 0};
  // A new reaction to the user's message: a heart, as in the phone apps.
  badge.text = chat.unreadMentions > 0 ? "@" : chat.unreadReactions > 0 ? "\xE2\x9D\xA4" : "";
  if (chat.unread > 0) {
    badge.text += (badge.text.empty() ? "" : " ") + std::to_string(chat.unread);
  } else if (badge.text.empty()) {
    badge.text = " ";  // Marked as unread: an empty dot.
  }
  return badge;
}

const char *ChatStatusIcon(const Chat &chat, UserId self) {
  if (!chat.lastMessageId || !chat.lastOutgoing || chat.lastService) return nullptr;
  if (chat.kind == ChatKind::Private && chat.peerId == self) return nullptr;
  if (chat.lastFailed) return "dialog-error-symbolic";
  if (chat.lastSending) return "contrail-clock-symbolic";
  return chat.lastMessageId > chat.readOutbox ? "contrail-check-symbolic" : "contrail-check-double-symbolic";
}

std::string TypingText(const Chat &chat, const Store &store) {
  const auto actions = store.actions(chat.id);
  if (!actions) return {};
  const auto named = chat.kind != ChatKind::Private && chat.kind != ChatKind::Secret;
  auto typing = std::vector<UserId>();
  for (const auto &action : *actions) {
    if (action.kind == api::chatActionTyping::ID) typing.push_back(action.user);
  }
  if (typing.size() > 2) return TrCount("lng_many_typing", std::int64_t(typing.size()));
  if (typing.size() == 2) {
    return Tr("lng_users_typing", {{"user", FirstName(store, typing[0])}, {"second_user", FirstName(store, typing[1])}});
  }
  if (typing.size() == 1) return named ? Tr("lng_user_typing", {{"user", FirstName(store, typing[0])}}) : Tr("lng_typing");
  for (const auto &action : *actions) {
    const auto text = ActionText(action.kind, named ? FirstName(store, action.user) : std::string());
    if (!text.empty()) return text;
  }
  return {};
}

std::string PreviewSender(const Chat &chat, const Store &store) {
  if (!chat.lastMessageId || chat.lastService || chat.kind == ChatKind::Channel) return {};
  if (chat.kind == ChatKind::Private && chat.peerId == store.selfId()) return {};
  if (chat.lastOutgoing) return Tr("lng_from_you");
  if (chat.kind == ChatKind::Group) {
    if (const auto name = FirstName(store, chat.lastSenderUser); !name.empty()) return name;
    if (const auto from = store.chat(chat.lastSenderChat)) return from->title;
  }
  return {};
}

}  // namespace Contrail::Td
