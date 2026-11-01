#include "td/message_text.hpp"

#include "td/strings.hpp"

#include <glib.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <utility>
#include <vector>

namespace Contrail::Td {
namespace {

// tdesktop's kPinnedMessageTextLimit.
constexpr auto kPinnedTextLimit = 16;
// Chat rows keep their preview, and a chat list can be thousands long; no row shows more than this.
constexpr auto kPreviewLimit = 200;
constexpr auto kNanoInOne = std::int64_t(1'000'000'000);
constexpr auto kNanoInGramCent = std::int64_t(10'000'000);
constexpr auto kGameEmoji = "\xF0\x9F\x8E\xAE";
constexpr auto kPollEmoji = "\xF0\x9F\x93\x8A";
constexpr auto kChecklistEmoji = "\xE2\x9C\x85";
constexpr auto kDiceEmoji = "\xF0\x9F\x8E\xB2";
constexpr auto kSpoilerMark = "\xE2\x96\x92";
constexpr auto kEllipsis = "\xE2\x80\xA6";

template <typename Type>
[[nodiscard]] const Type &To(const api::MessageContent &content) {
  return static_cast<const Type &>(content);
}

template <typename Type, typename Base>
[[nodiscard]] const Type *Cast(const Base *object) {
  return (object && object->get_id() == Type::ID) ? static_cast<const Type *>(object) : nullptr;
}

[[nodiscard]] std::size_t CharLength(unsigned char lead) {
  return lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
}

// Text can come with bytes cut short, the walk must not run past them.
[[nodiscard]] std::size_t CharLength(const std::string &text, std::size_t at) {
  return std::min(CharLength(static_cast<unsigned char>(text[at])), text.size() - at);
}

[[nodiscard]] std::string FirstChars(const std::string &text, int limit, bool *cut = nullptr) {
  auto at = std::size_t(0);
  for (; at < text.size() && limit > 0; --limit) at += CharLength(text, at);
  if (cut) *cut = (at < text.size());
  return text.substr(0, at);
}

[[nodiscard]] int CountChars(const std::string &text) {
  auto count = 0;
  for (auto at = std::size_t(0); at < text.size(); ++count) at += CharLength(text, at);
  return count;
}

// A preview is one line: line breaks, and the indents around them, become one space.
[[nodiscard]] std::string FlattenLines(const std::string &text) {
  auto result = std::string();
  result.reserve(text.size());
  auto space = false;
  for (auto at = std::size_t(0); at < text.size();) {
    const auto size = CharLength(text, at);
    const auto ascii = (size == 1) ? text[at] : '\0';
    // U+2028 and U+2029 are line breaks too.
    const auto separator = (size == 3 && text.compare(at, 2, "\xE2\x80") == 0
                            && (text[at + 2] == '\xA8' || text[at + 2] == '\xA9'));
    if (ascii == '\n' || ascii == '\r' || ascii == '\t' || ascii == ' ' || separator) {
      space = true;
    } else {
      if (space && !result.empty()) result += ' ';
      space = false;
      result.append(text, at, size);
    }
    at += size;
  }
  return result;
}

// The chat list has no way to hide a spoiler behind a tap, so its letters must not reach it at all. TDLib counts
// entities in UTF-16.
[[nodiscard]] std::string HideSpoilers(const api::formattedText &text) {
  auto ranges = std::vector<std::pair<std::int32_t, std::int32_t>>();
  for (const auto &entity : text.entities_) {
    if (entity && entity->type_ && entity->type_->get_id() == api::textEntityTypeSpoiler::ID) {
      ranges.emplace_back(entity->offset_, entity->offset_ + entity->length_);
    }
  }
  if (ranges.empty()) return text.text_;
  auto result = std::string();
  auto hiding = false;
  auto offset = std::int32_t(0);
  for (auto at = std::size_t(0); at < text.text_.size();) {
    const auto size = CharLength(text.text_, at);
    const auto hide = std::any_of(ranges.begin(), ranges.end(), [&](const auto &range) {
      return offset >= range.first && offset < range.second;
    });
    if (!hide) {
      result.append(text.text_, at, size);
    } else if (!hiding) {
      result += kSpoilerMark;
    }
    hiding = hide;
    offset += (size == 4) ? 2 : 1;
    at += size;
  }
  return result;
}

[[nodiscard]] std::string Plain(const api::object_ptr<api::formattedText> &text) {
  return text ? HideSpoilers(*text) : std::string();
}

[[nodiscard]] std::string WithCaption(const std::string &type, const std::string &caption) {
  if (caption.empty()) return type;
  return Tr("lng_dialogs_text_media",
            {{"media_part", Tr("lng_dialogs_text_media_wrapped", {{"media", type}})}, {"caption", caption}});
}

// "1.53" for 1'530'000'000 nano, without trailing zeros.
[[nodiscard]] std::string Decimal(std::int64_t nano) {
  auto result = std::to_string(std::llabs(nano) / kNanoInOne);
  auto fraction = std::to_string(std::llabs(nano) % kNanoInOne);
  fraction.insert(0, 9 - fraction.size(), '0');
  fraction.erase(fraction.find_last_not_of('0') + 1);
  if (!fraction.empty()) result += '.' + fraction;
  return result;
}

[[nodiscard]] std::string Stars(std::int64_t count) {
  return TrCount("lng_action_gift_for_stars", count);
}

[[nodiscard]] std::string Grams(std::int64_t nano) {
  // The plural forms are for whole numbers; a fraction takes "other", like in the rules of these languages.
  return (nano % kNanoInOne == 0)
             ? TrCount("lng_action_gram_transfer_amount", nano / kNanoInOne, {{"amount", Decimal(nano)}})
             : Tr("lng_action_gram_transfer_amount", {{"amount", Decimal(nano)}});
}

[[nodiscard]] int CurrencyExponent(const std::string &currency) {
  static constexpr auto kNone = std::array{"BIF", "CLP", "DJF", "GNF", "ISK", "JPY", "KMF", "KRW",
                                           "MGA", "PYG", "RWF", "UGX", "VND", "VUV", "XAF", "XOF", "XPF"};
  static constexpr auto kThree = std::array{"BHD", "IQD", "JOD", "KWD", "LYD", "OMR", "TND"};
  const auto listed = [&](const auto &list) {
    return std::any_of(list.begin(), list.end(), [&](const char *code) { return currency == code; });
  };
  return listed(kNone) ? 0 : listed(kThree) ? 3 : 2;
}

// Telegram Desktop keeps a rule for every currency, with its separators. A symbol for the usual ones and the ISO code
// for the others say the same, and the 150 rows of that table stay out of a phone's memory.
[[nodiscard]] std::string Money(std::int64_t amount, const std::string &currency) {
  static constexpr std::pair<const char *, const char *> kSymbols[] = {
      {"USD", "$"},           {"EUR", "\xE2\x82\xAC"}, {"GBP", "\xC2\xA3"},     {"JPY", "\xC2\xA5"},
      {"KRW", "\xE2\x82\xA9"}, {"INR", "\xE2\x82\xB9"}, {"CNY", "CN\xC2\xA5"}, {"AUD", "AU$"},
      {"CAD", "CA$"},         {"NZD", "NZ$"},          {"HKD", "HK$"},         {"TWD", "NT$"},
      {"MXN", "MX$"},         {"THB", "\xE0\xB8\xBF"}, {"VND", "\xE2\x82\xAB"}};
  if (currency == "XTR") return Stars(std::llabs(amount));
  auto power = std::int64_t(1);
  const auto exponent = CurrencyExponent(currency);
  for (auto i = 0; i != exponent; ++i) power *= 10;
  auto value = std::to_string(std::llabs(amount) / power);
  if (exponent) {
    auto fraction = std::to_string(std::llabs(amount) % power);
    fraction.insert(0, exponent - fraction.size(), '0');
    value += '.' + fraction;
  }
  const auto sign = std::string(amount < 0 ? "\xE2\x88\x92" : "");
  for (const auto &[code, symbol] : kSymbols) {
    if (currency == code) return sign + symbol + value;
  }
  return sign + value + ' ' + currency;
}

[[nodiscard]] std::string ResalePrice(const api::GiftResalePrice *price) {
  if (const auto stars = Cast<api::giftResalePriceStar>(price)) return Stars(stars->star_count_);
  if (const auto grams = Cast<api::giftResalePriceGram>(price)) return Grams(grams->gram_cent_count_ * kNanoInGramCent);
  return {};
}

[[nodiscard]] std::string CallDuration(std::int32_t seconds) {
  if (seconds <= 59) return TrCount("lng_seconds", seconds);
  return Tr("lng_duration_minutes_seconds",
            {{"minutes_count", TrCount("lng_duration_minsec_minutes", seconds / 60)},
             {"seconds_count", TrCount("lng_duration_minsec_seconds", seconds % 60)}});
}

[[nodiscard]] std::string WithDuration(const std::string &type, std::int32_t duration) {
  if (duration <= 0) return type;
  return Tr("lng_call_type_and_duration", {{"type", type}, {"duration", CallDuration(duration)}});
}

[[nodiscard]] std::string LongDuration(std::int32_t seconds) {
  const auto days = seconds / 86400;
  const auto hours = seconds / 3600;
  const auto minutes = seconds / 60;
  return days > 1      ? TrCount("lng_days", days)
         : hours > 1   ? TrCount("lng_hours", hours)
         : minutes > 1 ? TrCount("lng_minutes", minutes)
                       : TrCount("lng_seconds", seconds);
}

[[nodiscard]] std::string AutoDeleteTime(std::int32_t seconds) {
  constexpr auto kDay = 86400;
  if (seconds < 3600) return seconds < 60 ? TrCount("lng_seconds", seconds) : TrCount("lng_minutes", seconds / 60);
  if (seconds < kDay) return TrCount("lng_hours", seconds / 3600);
  if (seconds < kDay * 7) return TrCount("lng_days", seconds / kDay);
  if (seconds < kDay * 31) {
    const auto days = seconds / kDay;
    const auto weeks = TrCount("lng_weeks", days / 7);
    return (days % 7) ? weeks + ' ' + TrCount("lng_days", days % 7) : weeks;
  }
  if (seconds <= kDay * 31 * 11) return TrCount("lng_months", seconds / (kDay * 31));
  return TrCount("lng_years", (seconds + kDay * 182) / (kDay * 365));
}

[[nodiscard]] bool SameDay(GDateTime *left, GDateTime *right) {
  return g_date_time_get_year(left) == g_date_time_get_year(right)
         && g_date_time_get_day_of_year(left) == g_date_time_get_day_of_year(right);
}

// Worked out on each call: "today" goes stale as time passes, and the view asks again when it redraws.
[[nodiscard]] std::string ScheduledDate(std::int32_t date) {
  static constexpr const char *kMonths[] = {"lng_month1", "lng_month2", "lng_month3",  "lng_month4",
                                            "lng_month5", "lng_month6", "lng_month7",  "lng_month8",
                                            "lng_month9", "lng_month10", "lng_month11", "lng_month12"};
  auto *when = g_date_time_new_from_unix_local(date);
  auto *now = g_date_time_new_now_local();
  auto *tomorrow = g_date_time_add_days(now, 1);
  if (!when || !now || !tomorrow) {
    if (when) g_date_time_unref(when);
    if (now) g_date_time_unref(now);
    if (tomorrow) g_date_time_unref(tomorrow);
    return {};
  }
  auto *clock = g_date_time_format(when, "%H:%M");
  const auto time = std::string(clock ? clock : "");
  g_free(clock);
  const auto day = Tr("lng_month_day_year", {{"month", Tr(kMonths[g_date_time_get_month(when) - 1])},
                                             {"day", std::to_string(g_date_time_get_day_of_month(when))},
                                             {"year", std::to_string(g_date_time_get_year(when))}});
  const auto result = SameDay(when, now)        ? Tr("lng_group_call_starts_today", {{"time", time}})
                      : SameDay(when, tomorrow) ? Tr("lng_group_call_starts_tomorrow", {{"time", time}})
                                                : Tr("lng_group_call_starts_date", {{"date", day}, {"time", time}});
  g_date_time_unref(tomorrow);
  g_date_time_unref(now);
  g_date_time_unref(when);
  return result;
}

// "Anna, Boris and Clara". The argument names differ between the strings of a list of people and of tasks.
[[nodiscard]] std::string JoinList(const std::vector<std::string> &items, const char *one, const char *last,
                                   const char *accumulated, const char *next) {
  auto result = std::string();
  for (auto i = std::size_t(0); i != items.size(); ++i) {
    if (i == 0) {
      result = items[0];
    } else {
      result = Tr((i + 1 == items.size()) ? last : one, {{accumulated, result}, {next, items[i]}});
    }
  }
  return result;
}

[[nodiscard]] std::string JoinUsers(const std::vector<std::string> &names) {
  return JoinList(names, "lng_action_add_users_and_one", "lng_action_add_users_and_last", "accumulated", "user");
}

[[nodiscard]] std::string JoinInvited(const std::vector<std::string> &names) {
  return JoinList(names, "lng_action_invite_users_and_one", "lng_action_invite_users_and_last", "accumulated", "user");
}

[[nodiscard]] std::string JoinTasks(const std::vector<std::string> &tasks) {
  return JoinList(tasks, "lng_action_todo_tasks_and_one", "lng_action_todo_tasks_and_last", "tasks", "task");
}

[[nodiscard]] std::string UserName(const Store &store, UserId id) {
  const auto user = store.user(id);
  const auto name = user ? DisplayName(*user) : std::string();
  // TDLib tells of a user before mentioning one, so a missing name is an account that is gone.
  return name.empty() ? Tr("lng_deleted") : name;
}

[[nodiscard]] std::string UserShortName(const Store &store, UserId id) {
  const auto user = store.user(id);
  return (user && !user->firstName.empty()) ? user->firstName : UserName(store, id);
}

[[nodiscard]] std::string ChatTitle(const Store &store, ChatId id) {
  const auto chat = store.chat(id);
  return (chat && !chat->title.empty()) ? chat->title : Tr("lng_deleted");
}

[[nodiscard]] std::string SenderName(const Store &store, const api::MessageSender *sender) {
  if (const auto user = Cast<api::messageSenderUser>(sender)) return UserName(store, user->user_id_);
  if (const auto chat = Cast<api::messageSenderChat>(sender)) return ChatTitle(store, chat->chat_id_);
  return Tr("lng_deleted");
}

[[nodiscard]] std::string SenderShortName(const Store &store, const api::MessageSender *sender) {
  if (const auto user = Cast<api::messageSenderUser>(sender)) return UserShortName(store, user->user_id_);
  return SenderName(store, sender);
}

[[nodiscard]] bool SameSender(const api::MessageSender *left, const api::MessageSender *right) {
  if (!left || !right || left->get_id() != right->get_id()) return false;
  if (const auto user = Cast<api::messageSenderUser>(left)) {
    return user->user_id_ == static_cast<const api::messageSenderUser *>(right)->user_id_;
  }
  return Cast<api::messageSenderChat>(left)->chat_id_ == Cast<api::messageSenderChat>(right)->chat_id_;
}

[[nodiscard]] bool IsUser(const api::MessageSender *sender, UserId id) {
  const auto user = Cast<api::messageSenderUser>(sender);
  return user && user->user_id_ == id;
}

// The message being described. Empty when only the kind of a message is asked for: no text is built then.
struct Item {
  const api::message *message = nullptr;
  const Store *store = nullptr;
  const api::message *related = nullptr;

  [[nodiscard]] const api::message &m() const { return *message; }
  [[nodiscard]] const Store &s() const { return *store; }
  [[nodiscard]] const Chat *chat() const { return store->chat(message->chat_id_); }
  [[nodiscard]] bool out() const { return message->is_outgoing_; }
  [[nodiscard]] bool isSelf(const api::MessageSender *sender) const { return IsUser(sender, store->selfId()); }
  [[nodiscard]] bool self() const { return out() || isSelf(message->sender_id_.get()); }
  [[nodiscard]] bool post() const { return message->is_channel_post_; }
  [[nodiscard]] bool broadcast() const {
    const auto own = chat();
    return own && own->kind == ChatKind::Channel;
  }
  [[nodiscard]] bool savedMessages() const {
    const auto own = chat();
    return own && own->kind == ChatKind::Private && own->peerId == store->selfId();
  }
  [[nodiscard]] std::string from() const { return SenderName(*store, message->sender_id_.get()); }
  [[nodiscard]] std::string fromShort() const { return SenderShortName(*store, message->sender_id_.get()); }
  [[nodiscard]] std::string title() const { return ChatTitle(*store, message->chat_id_); }
  // The other side of a private chat, by first name; the title of any other.
  [[nodiscard]] std::string peerShort() const {
    const auto own = chat();
    if (own && own->kind == ChatKind::Private) return UserShortName(*store, own->peerId);
    return title();
  }
  // Someone did something in a chat of two: you to them, or they to you.
  [[nodiscard]] std::string otherShort() const { return self() ? peerShort() : fromShort(); }
  [[nodiscard]] std::string user(UserId id) const { return UserName(*store, id); }
  [[nodiscard]] std::string userShort(UserId id) const { return UserShortName(*store, id); }
  [[nodiscard]] std::string sender(const api::object_ptr<api::MessageSender> &id) const {
    return SenderName(*store, id.get());
  }
  [[nodiscard]] std::string senderShort(const api::object_ptr<api::MessageSender> &id) const {
    return SenderShortName(*store, id.get());
  }
};

[[nodiscard]] std::string DocumentName(const api::messageAudio &content) {
  const auto audio = content.audio_.get();
  if (audio && !audio->title_.empty() && !audio->performer_.empty()) {
    return audio->performer_ + " \xE2\x80\x93 " + audio->title_;
  }
  if (audio && !audio->title_.empty()) return audio->title_;
  if (audio && !audio->file_name_.empty()) return audio->file_name_;
  return Tr("lng_in_dlg_audio_file");
}

[[nodiscard]] std::string StickerText(const std::string &emoji) {
  return emoji.empty() ? Tr("lng_in_dlg_sticker") : Tr("lng_in_dlg_sticker_emoji", {{"emoji", emoji}});
}

[[nodiscard]] std::string CallText(const Item &item, const api::messageCall &call) {
  const auto missed = Cast<api::callDiscardReasonMissed>(call.discard_reason_.get()) != nullptr;
  const auto declined = Cast<api::callDiscardReasonDeclined>(call.discard_reason_.get()) != nullptr;
  const auto type = item.out() ? (missed ? (call.is_video_ ? Tr("lng_call_video_cancelled") : Tr("lng_call_cancelled"))
                                         : (call.is_video_ ? Tr("lng_call_video_outgoing") : Tr("lng_call_outgoing")))
                    : missed   ? (call.is_video_ ? Tr("lng_call_video_missed") : Tr("lng_call_missed"))
                    : declined ? (call.is_video_ ? Tr("lng_call_video_declined") : Tr("lng_call_declined"))
                               : (call.is_video_ ? Tr("lng_call_video_incoming") : Tr("lng_call_incoming"));
  return WithDuration(type, call.duration_);
}

[[nodiscard]] std::string GroupCallText(const Item &item, const api::messageGroupCall &call) {
  if (call.is_active_) return Tr("lng_call_ongoing");
  const auto type = item.out() ? (call.was_missed_ ? Tr("lng_call_group_declined") : Tr("lng_call_group_outgoing"))
                               : (call.was_missed_ ? Tr("lng_call_group_missed") : Tr("lng_call_group_incoming"));
  return WithDuration(type, call.duration_);
}

[[nodiscard]] std::string PaidMediaType(const api::PaidMedia *media) {
  return Cast<api::paidMediaVideo>(media) ? Tr("lng_in_dlg_video") : Tr("lng_in_dlg_photo");
}

[[nodiscard]] std::string InvoiceText(const api::messageInvoice &invoice) {
  if (const auto media = invoice.paid_media_.get()) {
    return WithCaption(PaidMediaType(media), Plain(invoice.paid_media_caption_));
  }
  return invoice.product_info_ ? invoice.product_info_->title_ : std::string();
}

// What a pin says of the message pinned; empty for a message with text only, which is quoted instead.
[[nodiscard]] std::string PinnedMedia(const api::MessageContent &content) {
  switch (content.get_id()) {
    case api::messagePhoto::ID: return Tr("lng_action_pinned_media_photo");
    case api::messageVideo::ID: return Tr("lng_action_pinned_media_video");
    case api::messageAudio::ID: return Tr("lng_action_pinned_media_audio");
    case api::messageVoiceNote::ID: return Tr("lng_action_pinned_media_voice");
    case api::messageVideoNote::ID: return Tr("lng_action_pinned_media_video_message");
    case api::messageDocument::ID: return Tr("lng_action_pinned_media_file");
    case api::messageAnimation::ID: return Tr("lng_action_pinned_media_gif");
    case api::messageContact::ID: return Tr("lng_action_pinned_media_contact");
    case api::messageLocation::ID:
    case api::messageLiveLocation::ID:
    case api::messageVenue::ID: return Tr("lng_action_pinned_media_location");
    case api::messageStory::ID: return Tr("lng_action_pinned_media_story");
    case api::messageSticker::ID: {
      const auto sticker = To<api::messageSticker>(content).sticker_.get();
      const auto emoji = sticker ? sticker->emoji_ : std::string();
      return emoji.empty() ? Tr("lng_action_pinned_media_sticker")
                           : Tr("lng_action_pinned_media_emoji_sticker", {{"emoji", emoji}});
    }
    case api::messageGame::ID: {
      const auto game = To<api::messageGame>(content).game_.get();
      return Tr("lng_action_pinned_media_game", {{"game", game ? game->title_ : std::string()}});
    }
    case api::messagePoll::ID: {
      const auto poll = To<api::messagePoll>(content).poll_.get();
      return "\xC2\xAB" + (poll ? Plain(poll->question_) : std::string()) + "\xC2\xBB";
    }
    case api::messageChecklist::ID: {
      const auto list = To<api::messageChecklist>(content).list_.get();
      return "\xC2\xAB" + (list ? Plain(list->title_) : std::string()) + "\xC2\xBB";
    }
    case api::messageDice::ID: return "\xC2\xAB" + To<api::messageDice>(content).emoji_ + "\xC2\xBB";
    default: return {};
  }
}

// Only the text of a pinned message is quoted: tdesktop cuts it to a few letters.
[[nodiscard]] std::string PinnedText(const api::message &pinned, const Store &store) {
  auto text = FlattenLines(MessagePreview(pinned, store));
  auto cut = false;
  auto start = FirstChars(text, kPinnedTextLimit, &cut);
  if (cut && CountChars(text) > kPinnedTextLimit + 5) return start + kEllipsis;
  return text;
}

[[nodiscard]] std::string PinText(const Item &item, const api::messagePinMessage &pin) {
  const auto pinned = [&](const std::string &media) {
    return Tr("lng_action_pinned_media", {{"from", item.from()}, {"media", media}});
  };
  if (!pin.message_id_) return pinned(Tr("lng_deleted_message"));
  const auto &related = item.related;
  if (related && related->id_ == pin.message_id_ && related->content_) {
    const auto media = PinnedMedia(*related->content_);
    if (!media.empty()) return pinned(media);
    const auto text = PinnedText(*related, item.s());
    if (!text.empty()) return Tr("lng_action_pinned_message", {{"from", item.from()}, {"text", text}});
  }
  return pinned(Translated("a message", "\xD1\x81\xD0\xBE\xD0\xBE\xD0\xB1\xD1\x89\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5",
                           "\xD0\xBF\xD0\xBE\xD0\xB2\xD1\x96\xD0\xB4\xD0\xBE\xD0\xBC\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xBD\xD1\x8F"));
}

[[nodiscard]] std::string GameScoreText(const Item &item, const api::messageGameScore &score) {
  const auto related = item.related;
  const auto content = (related && related->id_ == score.game_message_id_) ? related->content_.get() : nullptr;
  const auto game = Cast<api::messageGame>(content);
  const auto title = (game && game->game_) ? game->game_->title_ : std::string();
  if (item.self()) {
    return title.empty() ? TrCount("lng_action_game_you_scored_no_game", score.score_)
                         : TrCount("lng_action_game_you_scored", score.score_, {{"game", title}});
  }
  return title.empty() ? TrCount("lng_action_game_score_no_game", score.score_, {{"from", item.from()}})
                       : TrCount("lng_action_game_score", score.score_, {{"from", item.from()}, {"game", title}});
}

[[nodiscard]] std::string InvitedText(const Item &item, const std::vector<std::int64_t> &ids) {
  const auto chat = Tr("lng_action_invite_user_chat");
  if (ids.size() == 1) {
    return Tr("lng_action_invite_user", {{"from", item.from()}, {"user", item.user(ids[0])}, {"chat", chat}});
  }
  if (ids.empty()) {
    return Tr("lng_action_invite_user", {{"from", item.from()}, {"user", "somebody"}, {"chat", chat}});
  }
  auto names = std::vector<std::string>();
  for (const auto id : ids) names.push_back(item.user(id));
  return Tr("lng_action_invite_users_many", {{"from", item.from()}, {"users", JoinInvited(names)}, {"chat", chat}});
}

[[nodiscard]] std::string AddedText(const Item &item, const std::vector<std::int64_t> &ids) {
  if (ids.size() == 1) {
    if (item.m().sender_id_ && IsUser(item.m().sender_id_.get(), ids[0])) {
      return Tr("lng_action_user_joined", {{"from", item.from()}});
    }
    return Tr("lng_action_add_user", {{"from", item.from()}, {"user", item.user(ids[0])}});
  }
  if (ids.empty()) return Tr("lng_action_add_user", {{"from", item.from()}, {"user", "somebody"}});
  auto names = std::vector<std::string>();
  for (const auto id : ids) names.push_back(item.user(id));
  return Tr("lng_action_add_users_many", {{"from", item.from()}, {"users", JoinUsers(names)}});
}

[[nodiscard]] std::string AutoDeleteText(const Item &item, const api::messageChatSetMessageAutoDeleteTime &change) {
  const auto period = change.message_auto_delete_time_;
  const auto duration = AutoDeleteTime(period);
  if (change.from_user_id_ && period) {
    if (change.from_user_id_ == item.s().selfId()) return Tr("lng_action_ttl_global_me", {{"duration", duration}});
    return Tr("lng_action_ttl_global", {{"from", item.user(change.from_user_id_)}, {"duration", duration}});
  }
  if (item.post()) {
    return period ? Tr("lng_action_ttl_changed_channel", {{"duration", duration}})
                  : Tr("lng_action_ttl_removed_channel");
  }
  if (item.self()) {
    return period ? Tr("lng_action_ttl_changed_you", {{"duration", duration}}) : Tr("lng_action_ttl_removed_you");
  }
  return period ? Tr("lng_action_ttl_changed", {{"from", item.from()}, {"duration", duration}})
                : Tr("lng_action_ttl_removed", {{"from", item.from()}});
}

[[nodiscard]] std::string BackgroundText(const Item &item, const api::messageChatSetBackground &change) {
  const auto same = (change.old_background_message_id_ != 0);
  const auto name = item.otherShort();
  if (item.self()) {
    if (!same && !change.only_for_self_) return Tr("lng_action_set_wallpaper_both_me", {{"user", name}});
    return same ? Tr("lng_action_set_same_wallpaper_me") : Tr("lng_action_set_wallpaper_me");
  }
  return same ? Tr("lng_action_set_same_wallpaper", {{"user", name}}) : Tr("lng_action_set_wallpaper", {{"user", name}});
}

[[nodiscard]] std::string UpgradedGiftName(const api::upgradedGift *gift) {
  return gift ? gift->title_ + " #" + std::to_string(gift->number_) : std::string();
}

[[nodiscard]] std::string ThemeText(const Item &item, const api::messageChatSetTheme &change) {
  const auto self = item.self();
  if (const auto gift = Cast<api::chatThemeGift>(change.theme_.get())) {
    const auto name = gift->gift_theme_ ? UpgradedGiftName(gift->gift_theme_->gift_.get()) : std::string();
    return self ? Tr("lng_action_you_gift_theme_changed", {{"name", name}})
                : Tr("lng_action_gift_theme_changed", {{"from", item.from()}, {"name", name}});
  }
  const auto emoji = Cast<api::chatThemeEmoji>(change.theme_.get());
  if (emoji && !emoji->name_.empty()) {
    return self ? Tr("lng_action_you_theme_changed", {{"emoji", emoji->name_}})
                : Tr("lng_action_theme_changed", {{"from", item.from()}, {"emoji", emoji->name_}});
  }
  return self ? Tr("lng_action_you_theme_disabled") : Tr("lng_action_theme_disabled", {{"from", item.from()}});
}

[[nodiscard]] std::string TopicEditText(const Item &item, const api::messageForumTopicEdited &edit) {
  const auto bot = [&] {
    const auto chat = item.chat();
    const auto user = (chat && chat->kind == ChatKind::Private) ? item.s().user(chat->peerId) : nullptr;
    return user && user->bot;
  }();
  const auto link = bot ? Tr("lng_action_topic_bot_thread") : Tr("lng_action_topic_placeholder");
  if (!edit.name_.empty()) {
    return Tr("lng_action_topic_renamed", {{"from", item.from()}, {"link", link}, {"title", edit.name_}});
  }
  if (!edit.edit_icon_custom_emoji_id_) return Tr("lng_message_empty");
  if (!edit.icon_custom_emoji_id_) return Tr("lng_action_topic_icon_removed", {{"from", item.from()}, {"link", link}});
  // The icon is a custom emoji, which plain text can't draw; the speech balloon stands for it.
  return Tr("lng_action_topic_icon_changed",
            {{"from", item.from()}, {"link", link}, {"emoji", "\xF0\x9F\x92\xAC"}});
}

[[nodiscard]] std::string CommunityName(const Item &item, std::int64_t id) {
  // Communities are supergroups: TDLib's chat id of a supergroup is its id below this offset.
  constexpr auto kSupergroupOffset = std::int64_t(-1'000'000'000'000);
  auto chat = item.s().chat(id);
  if (!chat) chat = item.s().chat(kSupergroupOffset - id);
  return chat ? chat->title : std::string();
}

[[nodiscard]] std::string CommunityAddedText(const Item &item, std::int64_t id) {
  const auto community = CommunityName(item, id);
  const auto itself = Cast<api::messageSenderChat>(item.m().sender_id_.get())
                      && Cast<api::messageSenderChat>(item.m().sender_id_.get())->chat_id_ == item.m().chat_id_;
  if (!community.empty()) {
    if (!itself) return Tr("lng_action_community_added", {{"from", item.from()}, {"community", community}});
    return item.broadcast() ? Tr("lng_action_community_added_channel", {{"community", community}})
                            : Tr("lng_action_community_added_chat", {{"community", community}});
  }
  if (!itself) return Tr("lng_action_community_added_unknown", {{"from", item.from()}});
  return item.broadcast() ? Tr("lng_action_community_added_unknown_channel")
                          : Tr("lng_action_community_added_unknown_chat");
}

[[nodiscard]] std::string CommunityRemovedText(const Item &item) {
  const auto chat = Cast<api::messageSenderChat>(item.m().sender_id_.get());
  if (chat && chat->chat_id_ == item.m().chat_id_) {
    return item.broadcast() ? Tr("lng_action_community_removed_channel") : Tr("lng_action_community_removed_chat");
  }
  return Tr("lng_action_community_removed", {{"from", item.from()}});
}

[[nodiscard]] std::string GiftCostText(const Item &item, std::int64_t gifter, const std::string &cost) {
  if (item.out()) return Tr("lng_action_gift_sent", {{"cost", cost}});
  if (!gifter) return Tr("lng_action_gift_received_anonymous", {{"cost", cost}});
  return Tr("lng_action_gift_received", {{"user", item.userShort(gifter)}, {"cost", cost}});
}

[[nodiscard]] std::string GiftCodeText(const Item &item, const api::messagePremiumGiftCode &gift) {
  if (const auto chat = Cast<api::messageSenderChat>(gift.creator_id_.get())) {
    const auto channel = ChatTitle(item.s(), chat->chat_id_);
    return gift.is_unclaimed_       ? Tr("lng_prize_unclaimed_about", {{"channel", channel}})
           : gift.is_from_giveaway_ ? Tr("lng_prize_about", {{"channel", channel}})
                                    : Tr("lng_prize_gift_about", {{"channel", channel}});
  }
  const auto cost = gift.currency_.empty() ? TrCount("lng_action_gift_premium_months", gift.month_count_)
                                           : Money(gift.amount_, gift.currency_);
  if (item.self()) return Tr("lng_action_gift_sent", {{"cost", cost}});
  return Tr("lng_action_gift_received", {{"user", item.fromShort()}, {"cost", cost}});
}

[[nodiscard]] std::string GiveawayCompletedText(const api::messageGiveawayCompleted &done) {
  const auto winners = done.winner_count_;
  const auto unclaimed = done.unclaimed_prize_count_;
  const auto credits = done.is_star_giveaway_;
  return !winners               ? Tr("lng_action_giveaway_results_none")
         : credits && unclaimed ? Tr("lng_action_giveaway_results_credits_some")
         : unclaimed            ? Tr("lng_action_giveaway_results_some")
         : credits              ? TrCount("lng_action_giveaway_results_credits", winners)
                                : TrCount("lng_action_giveaway_results", winners);
}

[[nodiscard]] std::string GiveawayCreatedText(const Item &item, const api::messageGiveawayCreated &created) {
  const auto group = !item.broadcast();
  const auto from = item.from();
  if (!created.star_count_) {
    return group ? Tr("lng_action_giveaway_started_group", {{"from", from}})
                 : Tr("lng_action_giveaway_started", {{"from", from}});
  }
  const auto amount = TrCount("lng_action_giveaway_credits_started_amount", created.star_count_);
  return group ? Tr("lng_action_giveaway_credits_started_group", {{"from", from}, {"amount", amount}})
               : Tr("lng_action_giveaway_credits_started", {{"from", from}, {"amount", amount}});
}

[[nodiscard]] std::string GramTransferText(const Item &item, const api::messageTonWalletTransfer &transfer) {
  const auto out = item.out() || transfer.amount_ < 0;
  const auto amount = Grams(std::llabs(transfer.amount_));
  const auto id = out ? std::int64_t(0) : transfer.sender_user_id_;
  const auto known = id ? item.s().user(id) : nullptr;
  auto name = known ? UserShortName(item.s(), id) : out ? item.peerShort() : std::string();
  if (name.empty() || name == Tr("lng_deleted")) name = transfer.peer_address_;
  const auto text = name.empty()
                        ? (out ? Tr("lng_action_gram_transfer_sent_unknown", {{"amount", amount}})
                               : Tr("lng_action_gram_transfer_received_unknown", {{"amount", amount}}))
                    : out ? Tr("lng_action_gram_transfer_sent", {{"amount", amount}, {"user", name}})
                          : Tr("lng_action_gram_transfer_received", {{"user", name}, {"amount", amount}});
  const auto comment = transfer.is_comment_encrypted_ ? Tr("lng_action_gram_transfer_encrypted_comment")
                                                      : transfer.comment_;
  return comment.empty() ? text : Tr("lng_action_gram_transfer_comment", {{"text", text}, {"comment", comment}});
}

[[nodiscard]] std::string TonConnectPending(const api::messageTonConnectRequest &request) {
  const auto &name = request.dapp_name_;
  if (name.empty()) return Tr("lng_action_ton_connect_request_unknown");
  const auto &topic = request.topic_;
  return topic == "sendTransaction" ? Tr("lng_action_ton_connect_send_transaction", {{"app", name}})
         : topic == "signData"      ? Tr("lng_action_ton_connect_sign_data", {{"app", name}})
         : topic == "signMessage"   ? Tr("lng_action_ton_connect_sign_message", {{"app", name}})
         : topic == "disconnect"    ? Tr("lng_action_ton_connect_disconnect", {{"app", name}})
                                    : Tr("lng_action_ton_connect_request", {{"app", name}});
}

[[nodiscard]] std::string TonConnectText(const api::messageTonConnectRequest &request) {
  const auto &name = request.dapp_name_;
  const auto state = request.state_.get();
  if (Cast<api::tonConnectRequestStateAccepted>(state)) {
    return name.empty() ? Tr("lng_action_ton_connect_accepted_unknown")
                        : Tr("lng_action_ton_connect_accepted", {{"app", name}});
  }
  if (Cast<api::tonConnectRequestStateRejected>(state)) {
    return name.empty() ? Tr("lng_action_ton_connect_declined_unknown")
                        : Tr("lng_action_ton_connect_declined", {{"app", name}});
  }
  const auto pending = Cast<api::tonConnectRequestStatePending>(state);
  if (pending && pending->expiration_date_ > std::time(nullptr)) return TonConnectPending(request);
  return name.empty() ? Tr("lng_action_ton_connect_expired_unknown")
                      : Tr("lng_action_ton_connect_expired", {{"app", name}});
}

[[nodiscard]] std::string GiftText(const Item &item, const api::messageGift &gift) {
  const auto upgradeGifted = gift.is_prepaid_upgrade_;
  const auto base = gift.gift_ ? gift.gift_->star_count_ : std::int64_t(0);
  const auto cost = Stars(upgradeGifted            ? gift.prepaid_upgrade_star_count_
                          : gift.is_upgrade_separate_ ? base
                                                      : base + gift.prepaid_upgrade_star_count_);
  const auto self = item.self();
  const auto peer = item.otherShort();
  const auto from = gift.sender_id_.get();
  const auto fromSelf = !from || item.isSelf(from);
  const auto receiver = Cast<api::messageSenderChat>(gift.receiver_id_.get());
  const auto channel = receiver ? item.s().chat(receiver->chat_id_) : nullptr;
  if (channel && channel->kind == ChatKind::Channel) {
    const auto name = ChatTitle(item.s(), receiver->chat_id_);
    if (fromSelf) {
      return upgradeGifted ? Tr("lng_action_gift_sent_upgrade_self_channel", {{"cost", cost}, {"name", name}})
                           : Tr("lng_action_gift_sent_self_channel", {{"name", name}, {"cost", cost}});
    }
    const auto user = item.senderShort(gift.sender_id_);
    return upgradeGifted
               ? Tr("lng_action_gift_sent_upgrade_self_other", {{"cost", cost}, {"name", name}, {"user", user}})
               : Tr("lng_action_gift_sent_channel", {{"user", user}, {"name", name}, {"cost", cost}});
  }
  const auto anonymous = !from && !self;
  if (anonymous || item.savedMessages()) {
    return gift.is_from_auction_ ? Tr("lng_action_gift_auction_won", {{"cost", cost}})
           : anonymous           ? Tr("lng_action_gift_received_anonymous", {{"cost", cost}})
                                 : Tr("lng_action_gift_self_bought", {{"cost", cost}});
  }
  if (upgradeGifted) {
    if (self) {
      return fromSelf ? Tr("lng_action_gift_sent_upgrade_self", {{"cost", cost}})
                      : Tr("lng_action_gift_sent_upgrade_self_other",
                           {{"cost", cost}, {"name", peer}, {"user", item.senderShort(gift.sender_id_)}});
    }
    if (!fromSelf && !SameSender(from, item.m().sender_id_.get())) {
      return Tr("lng_action_gift_sent_upgrade_other",
                {{"from", peer}, {"cost", cost}, {"user", item.senderShort(gift.sender_id_)}});
    }
    return Tr("lng_action_gift_sent_upgrade", {{"from", peer}, {"cost", cost}});
  }
  return self ? Tr("lng_action_gift_sent", {{"cost", cost}})
              : Tr("lng_action_gift_received", {{"user", peer}, {"cost", cost}});
}

[[nodiscard]] std::string UpgradedChannelGiftText(const Item &item, const api::messageUpgradedGift &gift,
                                                  const std::string &channel, const std::string &cost, bool upgrade) {
  const auto from = gift.sender_id_.get();
  if (from && !item.isSelf(from)) {
    const auto user = item.senderShort(gift.sender_id_);
    if (!cost.empty()) return Tr("lng_action_gift_sent_channel", {{"user", user}, {"name", channel}, {"cost", cost}});
    return upgrade ? Tr("lng_action_gift_upgraded_channel", {{"user", user}, {"channel", channel}})
                   : Tr("lng_action_gift_transferred_channel", {{"user", user}, {"channel", channel}});
  }
  if (!cost.empty()) return Tr("lng_action_gift_sent_self_channel", {{"name", channel}, {"cost", cost}});
  return !from    ? Tr("lng_action_gift_transferred_unknown_channel", {{"channel", channel}})
         : upgrade ? Tr("lng_action_gift_upgraded_self_channel", {{"channel", channel}})
                   : Tr("lng_action_gift_transferred_self_channel", {{"channel", channel}});
}

[[nodiscard]] std::string UpgradedGiftText(const Item &item, const api::messageUpgradedGift &gift) {
  const auto origin = gift.origin_.get();
  const auto resale = Cast<api::upgradedGiftOriginResale>(origin);
  const auto offer = Cast<api::upgradedGiftOriginOffer>(origin);
  const auto upgrade = Cast<api::upgradedGiftOriginUpgrade>(origin) != nullptr;
  const auto cost = resale ? ResalePrice(resale->price_.get()) : offer ? ResalePrice(offer->price_.get()) : std::string();
  const auto self = item.self();
  const auto peer = item.otherShort();
  const auto from = gift.sender_id_.get();
  const auto user = from ? item.senderShort(gift.sender_id_) : peer;
  const auto receiver = Cast<api::messageSenderChat>(gift.receiver_id_.get());
  const auto channel = receiver ? item.s().chat(receiver->chat_id_) : nullptr;
  if (channel && channel->kind == ChatKind::Channel) {
    return UpgradedChannelGiftText(item, gift, ChatTitle(item.s(), receiver->chat_id_), cost, upgrade);
  }
  if (Cast<api::upgradedGiftOriginPrepaidUpgrade>(origin)) {
    return self ? Tr("lng_action_gift_upgraded_helped_self", {{"user", peer}})
                : Tr("lng_action_gift_upgraded_helped", {{"user", peer}});
  }
  if (Cast<api::upgradedGiftOriginCraft>(origin)) return Tr("lng_action_gift_crafted");
  if (from && !item.savedMessages()) {
    if (offer) {
      return self ? Tr("lng_action_gift_sent_sold", {{"cost", cost}})
                  : Tr("lng_action_gift_received_sold", {{"user", peer}, {"cost", cost}});
    }
    if (resale) {
      return self ? Tr("lng_action_gift_sent", {{"cost", cost}})
                  : Tr("lng_action_gift_received", {{"user", peer}, {"cost", cost}});
    }
    if (upgrade) {
      return self ? Tr("lng_action_gift_upgraded_mine", {{"user", user}})
                  : Tr("lng_action_gift_upgraded", {{"user", user}});
    }
    return self ? Tr("lng_action_gift_transferred_mine", {{"user", user}})
                : Tr("lng_action_gift_transferred", {{"user", user}});
  }
  if (resale) return Tr("lng_action_gift_self_bought", {{"cost", cost}});
  return !from    ? Tr("lng_action_gift_transferred_unknown")
         : upgrade ? Tr("lng_action_gift_upgraded_self")
                   : Tr("lng_action_gift_transferred_self");
}

[[nodiscard]] std::string OfferText(const Item &item, const api::messageUpgradedGiftPurchaseOffer &offer) {
  const auto cost = ResalePrice(offer.price_.get());
  if (item.self()) {
    return Tr("lng_action_gift_offer_you", {{"cost", cost}, {"name", UpgradedGiftName(offer.gift_.get())}});
  }
  return Tr("lng_action_gift_offer_incoming", {{"amount", cost}});
}

[[nodiscard]] std::string OfferRejectedText(const Item &item, const api::messageUpgradedGiftPurchaseOfferRejected &offer) {
  const auto cost = ResalePrice(offer.price_.get());
  const auto name = UpgradedGiftName(offer.gift_.get());
  if (item.self()) {
    const auto user = item.title();
    return offer.was_expired_
               ? Tr("lng_action_gift_offer_expired", {{"user", user}, {"name", name}, {"cost", cost}})
               : Tr("lng_action_gift_offer_declined_you", {{"user", user}, {"name", name}, {"cost", cost}});
  }
  return offer.was_expired_
             ? Tr("lng_action_gift_offer_expired_your", {{"name", name}, {"cost", cost}})
             : Tr("lng_action_gift_offer_declined", {{"user", item.from()}, {"name", name}, {"cost", cost}});
}

[[nodiscard]] std::string ChecklistTaskTitle(const api::checklist *list, std::int32_t id) {
  if (list) {
    for (const auto &task : list->tasks_) {
      if (task && task->id_ == id) return Plain(task->text_);
    }
  }
  return {};
}

// Task names live in the checklist message; without it there is only how many.
[[nodiscard]] std::string TasksText(const Item &item, std::int64_t listId, const std::vector<std::int32_t> &ids) {
  const auto related = item.related;
  const auto content = (related && related->id_ == listId) ? related->content_.get() : nullptr;
  const auto checklist = Cast<api::messageChecklist>(content);
  auto names = std::vector<std::string>();
  for (const auto id : ids) {
    auto name = ChecklistTaskTitle(checklist ? checklist->list_.get() : nullptr, id);
    if (name.empty()) return TrCount("lng_action_todo_tasks_fallback", std::int64_t(ids.size()));
    names.push_back("\"" + name + "\"");
  }
  return JoinTasks(names);
}

[[nodiscard]] std::string TasksDoneText(const Item &item, const api::messageChecklistTasksDone &done) {
  if (done.marked_as_done_task_ids_.empty()) {
    const auto tasks = TasksText(item, done.checklist_message_id_, done.marked_as_not_done_task_ids_);
    return item.self() ? Tr("lng_action_todo_marked_not_done_self", {{"tasks", tasks}})
                       : Tr("lng_action_todo_marked_not_done", {{"from", item.from()}, {"tasks", tasks}});
  }
  const auto tasks = TasksText(item, done.checklist_message_id_, done.marked_as_done_task_ids_);
  return item.self() ? Tr("lng_action_todo_marked_done_self", {{"tasks", tasks}})
                     : Tr("lng_action_todo_marked_done", {{"from", item.from()}, {"tasks", tasks}});
}

[[nodiscard]] std::string TasksAddedText(const Item &item, const api::messageChecklistTasksAdded &added) {
  auto names = std::vector<std::string>();
  for (const auto &task : added.tasks_) {
    if (task) names.push_back("\"" + Plain(task->text_) + "\"");
  }
  const auto tasks = JoinTasks(names);
  return item.self() ? Tr("lng_action_todo_added_self", {{"tasks", tasks}})
                     : Tr("lng_action_todo_added", {{"from", item.from()}, {"tasks", tasks}});
}

[[nodiscard]] std::string SuggestedPostPaidText(const Item &item, const api::messageSuggestedPostPaid &paid) {
  const auto from = item.fromShort();
  if (paid.star_amount_ && paid.star_amount_->star_count_) {
    return TrCount("lng_action_suggest_success_stars", paid.star_amount_->star_count_, {{"from", from}});
  }
  if (paid.gram_amount_ % kNanoInOne == 0) {
    return TrCount("lng_action_suggest_success_ton", paid.gram_amount_ / kNanoInOne, {{"from", from}});
  }
  return Tr("lng_action_suggest_success_ton", {{"from", from}, {"count", Decimal(paid.gram_amount_)}});
}

[[nodiscard]] std::string PassportDocument(const api::PassportElementType *type) {
  switch (type ? type->get_id() : 0) {
    case api::passportElementTypePersonalDetails::ID: return Tr("lng_action_secure_personal_details");
    case api::passportElementTypePassport::ID:
    case api::passportElementTypeDriverLicense::ID:
    case api::passportElementTypeIdentityCard::ID:
    case api::passportElementTypeInternalPassport::ID: return Tr("lng_action_secure_proof_of_identity");
    case api::passportElementTypeAddress::ID: return Tr("lng_action_secure_address");
    case api::passportElementTypeUtilityBill::ID:
    case api::passportElementTypeBankStatement::ID:
    case api::passportElementTypeRentalAgreement::ID:
    case api::passportElementTypePassportRegistration::ID:
    case api::passportElementTypeTemporaryRegistration::ID: return Tr("lng_action_secure_proof_of_address");
    case api::passportElementTypePhoneNumber::ID: return Tr("lng_action_secure_phone");
    case api::passportElementTypeEmailAddress::ID: return Tr("lng_action_secure_email");
    default: return {};
  }
}

[[nodiscard]] std::string PassportText(const Item &item, const api::messagePassportDataSent &sent) {
  auto documents = std::string();
  for (const auto &type : sent.types_) {
    const auto name = PassportDocument(type.get());
    if (name.empty()) continue;
    documents += (documents.empty() ? "" : ", ") + name;
  }
  return Tr("lng_action_secure_values_sent", {{"user", item.title()}, {"documents", documents}});
}

[[nodiscard]] std::string ProximityText(const Item &item, const api::messageProximityAlertTriggered &alert) {
  const auto meters = alert.distance_;
  const auto distance = (meters >= 1000)
                            // Tens of meters at most, as tdesktop shows.
                            ? Tr("lng_action_proximity_distance_km",
                                 {{"count", Decimal(std::int64_t(meters / 10) * 10 * 1'000'000)}})
                            : TrCount("lng_action_proximity_distance_m", meters);
  if (item.isSelf(alert.traveler_id_.get())) {
    return Tr("lng_action_you_proximity_reached", {{"distance", distance}, {"user", item.sender(alert.watcher_id_)}});
  }
  if (item.isSelf(alert.watcher_id_.get())) {
    return Tr("lng_action_proximity_reached_you", {{"from", item.sender(alert.traveler_id_)}, {"distance", distance}});
  }
  return Tr("lng_action_proximity_reached",
            {{"from", item.sender(alert.traveler_id_)}, {"distance", distance}, {"user", item.sender(alert.watcher_id_)}});
}

[[nodiscard]] std::string BotAccessText(const api::messageBotWriteAccessAllowed &access) {
  const auto reason = access.reason_.get();
  if (const auto site = Cast<api::botWriteAccessAllowReasonConnectedWebsite>(reason)) {
    return Tr("lng_action_bot_allowed_from_domain", {{"domain", site->domain_name_}});
  }
  if (Cast<api::botWriteAccessAllowReasonAddedToAttachmentMenu>(reason)) return Tr("lng_action_attach_menu_bot_allowed");
  if (const auto app = Cast<api::botWriteAccessAllowReasonLaunchedWebApp>(reason)) {
    const auto title = app->web_app_ ? app->web_app_->title_ : std::string();
    return Tr("lng_action_bot_allowed_from_app", {{"app", title.empty() ? std::string("App") : title}});
  }
  return Tr("lng_action_webapp_bot_allowed");
}

[[nodiscard]] std::string PaymentText(const Item &item, const api::messagePaymentSuccessful &payment) {
  const auto amount = Money(payment.total_amount_, payment.currency_);
  const auto user = item.title();
  const auto first = payment.is_first_recurring_;
  if (payment.invoice_name_.empty()) {
    if (payment.is_recurring_ && !first) return Tr("lng_action_payment_used_recurring", {{"amount", amount}});
    return first ? Tr("lng_action_payment_init_recurring", {{"amount", amount}, {"user", user}})
                 : Tr("lng_action_payment_done", {{"amount", amount}, {"user", user}});
  }
  const auto &invoice = payment.invoice_name_;
  return first ? Tr("lng_action_payment_init_recurring_for", {{"amount", amount}, {"user", user}, {"invoice", invoice}})
               : Tr("lng_action_payment_done_for", {{"amount", amount}, {"user", user}, {"invoice", invoice}});
}

struct Line {
  ContentKind kind = ContentKind::Unknown;
  std::string text;
};

// The one table of everything a message can contain. Text is built only when wanted: classifying has no message.
[[nodiscard]] Line Describe(const api::MessageContent &content, const Item &item, bool wanted) {
  const auto line = [&](ContentKind kind, auto text) {
    return Line{kind, wanted ? std::string(text()) : std::string()};
  };
  const auto shown = [&](auto text) { return line(ContentKind::Shown, text); };
  const auto service = [&](auto text) { return line(ContentKind::Service, text); };
  const auto fallback = [&](auto text) { return line(ContentKind::Fallback, text); };
  const auto secret = [&](bool flag) { return flag || (item.message && item.message->self_destruct_type_); };

  switch (content.get_id()) {
    // Shown as they are.
    case api::messageText::ID:
      return shown([&] { return Plain(To<api::messageText>(content).text_); });
    case api::messageAnimation::ID:
      return shown([&] { return WithCaption("GIF", Plain(To<api::messageAnimation>(content).caption_)); });
    case api::messageAudio::ID:
      return shown([&] {
        const auto &audio = To<api::messageAudio>(content);
        return WithCaption(DocumentName(audio), Plain(audio.caption_));
      });
    case api::messageDocument::ID:
      return shown([&] {
        const auto &file = To<api::messageDocument>(content);
        const auto document = file.document_.get();
        const auto name = (document && !document->file_name_.empty()) ? document->file_name_ : Tr("lng_in_dlg_file");
        return WithCaption(name, Plain(file.caption_));
      });
    case api::messagePaidMedia::ID:
      return shown([&] {
        const auto &paid = To<api::messagePaidMedia>(content);
        return WithCaption(PaidMediaType(paid.media_.empty() ? nullptr : paid.media_.front().get()),
                           Plain(paid.caption_));
      });
    case api::messagePhoto::ID:
      return shown([&] {
        const auto &photo = To<api::messagePhoto>(content);
        return secret(photo.is_secret_) ? Tr("lng_in_dlg_photo_ttl")
                                        : WithCaption(Tr("lng_in_dlg_photo"), Plain(photo.caption_));
      });
    case api::messageSticker::ID:
      return shown([&] {
        const auto sticker = To<api::messageSticker>(content).sticker_.get();
        return StickerText(sticker ? sticker->emoji_ : std::string());
      });
    case api::messageVideo::ID:
      return shown([&] {
        const auto &video = To<api::messageVideo>(content);
        return secret(video.is_secret_) ? Tr("lng_in_dlg_video_ttl")
                                        : WithCaption(Tr("lng_in_dlg_video"), Plain(video.caption_));
      });
    case api::messageVideoNote::ID:
      return shown([&] {
        return secret(To<api::messageVideoNote>(content).is_secret_) ? Tr("lng_in_dlg_video_message_ttl")
                                                                     : Tr("lng_in_dlg_video_message");
      });
    case api::messageVoiceNote::ID:
      return shown([&] {
        return secret(false) ? Tr("lng_in_dlg_voice_message_ttl")
                             : WithCaption(Tr("lng_in_dlg_audio"), Plain(To<api::messageVoiceNote>(content).caption_));
      });
    case api::messageLiveLocation::ID:
      return shown([&] { return Tr("lng_live_location"); });
    case api::messageLocation::ID:
      return shown([&] { return Tr("lng_maps_point"); });
    case api::messageVenue::ID:
      return shown([&] {
        const auto venue = To<api::messageVenue>(content).venue_.get();
        return WithCaption(Tr("lng_maps_point"), venue ? venue->title_ : std::string());
      });
    case api::messageContact::ID:
      return shown([&] { return Tr("lng_in_dlg_contact"); });
    case api::messageAnimatedEmoji::ID:
      return shown([&] { return To<api::messageAnimatedEmoji>(content).emoji_; });
    case api::messageDice::ID:
      return shown([&] { return To<api::messageDice>(content).emoji_; });
    case api::messageStakeDice::ID:
      return shown([&] { return kDiceEmoji; });
    case api::messageGame::ID:
      return shown([&] {
        const auto game = To<api::messageGame>(content).game_.get();
        return std::string(kGameEmoji) + ' ' + (game ? game->title_ : std::string());
      });
    case api::messagePoll::ID:
      return shown([&] {
        const auto poll = To<api::messagePoll>(content).poll_.get();
        return std::string(kPollEmoji) + ' ' + (poll ? Plain(poll->question_) : std::string());
      });
    case api::messageChecklist::ID:
      return shown([&] {
        const auto list = To<api::messageChecklist>(content).list_.get();
        return std::string(kChecklistEmoji) + ' ' + (list ? Plain(list->title_) : std::string());
      });
    case api::messageInvoice::ID:
      return shown([&] { return InvoiceText(To<api::messageInvoice>(content)); });
    case api::messageCall::ID:
      return shown([&] { return CallText(item, To<api::messageCall>(content)); });
    case api::messageGroupCall::ID:
      return shown([&] { return GroupCallText(item, To<api::messageGroupCall>(content)); });
    case api::messageGiveaway::ID:
      return shown([&] { return TrCount("lng_prizes_title", To<api::messageGiveaway>(content).winner_count_); });
    case api::messageGiveawayWinners::ID:
      return shown([&] {
        return To<api::messageGiveawayWinners>(content).winner_count_ == 1 ? Tr("lng_prizes_results_title_one")
                                                                           : Tr("lng_prizes_results_title");
      });
    case api::messageStory::ID: {
      const auto text = [&] {
        const auto user = item.peerShort();
        return item.out() ? Tr("lng_action_story_mention_me", {{"user", user}})
                          : Tr("lng_action_story_mention", {{"user", user}});
      };
      if (To<api::messageStory>(content).via_mention_) return service(text);
      return shown([&] { return Tr("lng_in_dlg_story"); });
    }

    // Lines in the middle of the chat.
    case api::messageExpiredPhoto::ID:
      return service([&] { return Tr("lng_ttl_photo_expired"); });
    case api::messageExpiredVideo::ID:
      return service([&] { return Tr("lng_ttl_video_expired"); });
    case api::messageExpiredVideoNote::ID:
      return service([&] { return Tr("lng_ttl_round_expired"); });
    case api::messageExpiredVoiceNote::ID:
      return service([&] { return Tr("lng_ttl_voice_expired"); });
    case api::messageVideoChatScheduled::ID:
      return service([&] {
        const auto date = ScheduledDate(To<api::messageVideoChatScheduled>(content).start_date_);
        return item.broadcast() ? Tr("lng_action_group_call_scheduled_channel", {{"date", date}})
                                : Tr("lng_action_group_call_scheduled_group", {{"from", item.from()}, {"date", date}});
      });
    case api::messageVideoChatStarted::ID:
      return service([&] {
        return item.broadcast() ? Tr("lng_action_group_call_started_channel")
                                : Tr("lng_action_group_call_started_group", {{"from", item.from()}});
      });
    case api::messageVideoChatEnded::ID:
      return service([&] {
        const auto duration = LongDuration(To<api::messageVideoChatEnded>(content).duration_);
        return item.broadcast()
                   ? Tr("lng_action_group_call_finished", {{"duration", duration}})
                   : Tr("lng_action_group_call_finished_group", {{"from", item.from()}, {"duration", duration}});
      });
    case api::messageInviteVideoChatParticipants::ID:
      return service([&] { return InvitedText(item, To<api::messageInviteVideoChatParticipants>(content).user_ids_); });
    case api::messagePollOptionAdded::ID:
      return service([&] {
        const auto option = Plain(To<api::messagePollOptionAdded>(content).text_);
        return item.self() ? Tr("lng_action_poll_added_answer_self", {{"option", option}})
                           : Tr("lng_action_poll_added_answer", {{"from", item.from()}, {"option", option}});
      });
    case api::messagePollOptionDeleted::ID:
      return service([&] {
        const auto option = Plain(To<api::messagePollOptionDeleted>(content).text_);
        return item.self() ? Tr("lng_action_poll_deleted_answer_self", {{"option", option}})
                           : Tr("lng_action_poll_deleted_answer", {{"from", item.from()}, {"option", option}});
      });
    case api::messageBasicGroupChatCreate::ID:
      return service([&] {
        return Tr("lng_action_created_chat",
                  {{"from", item.from()}, {"title", To<api::messageBasicGroupChatCreate>(content).title_}});
      });
    case api::messageSupergroupChatCreate::ID:
      return service([&] {
        return item.post() ? Tr("lng_action_created_channel")
                           : Tr("lng_action_created_chat", {{"from", item.from()},
                                                            {"title", To<api::messageSupergroupChatCreate>(content).title_}});
      });
    case api::messageChatChangeTitle::ID:
      return service([&] {
        const auto &title = To<api::messageChatChangeTitle>(content).title_;
        return item.post() ? Tr("lng_action_changed_title_channel", {{"title", title}})
                           : Tr("lng_action_changed_title", {{"from", item.from()}, {"title", title}});
      });
    case api::messageChatChangePhoto::ID:
      return service([&] {
        return item.post() ? Tr("lng_action_changed_photo_channel")
                           : Tr("lng_action_changed_photo", {{"from", item.from()}});
      });
    case api::messageChatDeletePhoto::ID:
      return service([&] {
        return item.post() ? Tr("lng_action_removed_photo_channel")
                           : Tr("lng_action_removed_photo", {{"from", item.from()}});
      });
    case api::messageChatOwnerLeft::ID:
      return service([&] {
        const auto id = To<api::messageChatOwnerLeft>(content).new_owner_user_id_;
        if (!id) return Tr("lng_action_user_left", {{"from", item.from()}});
        return Tr("lng_action_new_creator_pending", {{"user", item.user(id)}, {"from", item.from()}});
      });
    case api::messageChatOwnerChanged::ID:
      return service([&] {
        const auto id = To<api::messageChatOwnerChanged>(content).new_owner_user_id_;
        return Tr("lng_action_change_creator", {{"from", item.from()}, {"user", item.user(id)}});
      });
    case api::messageChatHasProtectedContentToggled::ID:
      return service([&] {
        const auto &toggled = To<api::messageChatHasProtectedContentToggled>(content);
        const auto enabled = toggled.new_has_protected_content_;
        if (enabled == toggled.old_has_protected_content_) return Tr("lng_action_no_forwards_still_disabled");
        if (item.out()) {
          return enabled ? Tr("lng_action_no_forwards_you_disabled") : Tr("lng_action_no_forwards_you_enabled");
        }
        return enabled ? Tr("lng_action_no_forwards_disabled", {{"from", item.from()}})
                       : Tr("lng_action_no_forwards_enabled", {{"from", item.from()}});
      });
    case api::messageChatHasProtectedContentDisableRequested::ID:
      return service([&] {
        if (To<api::messageChatHasProtectedContentDisableRequested>(content).is_expired_) {
          return Tr("lng_action_no_forwards_request_expired");
        }
        return item.self() ? Tr("lng_action_no_forwards_request_you")
                           : Tr("lng_action_no_forwards_request", {{"from", item.from()}});
      });
    case api::messageChatAddMembers::ID:
      return service([&] { return AddedText(item, To<api::messageChatAddMembers>(content).member_user_ids_); });
    case api::messageChatJoinByLink::ID:
      return service([&] { return Tr("lng_action_user_joined_by_link", {{"from", item.from()}}); });
    case api::messageChatJoinByRequest::ID:
      return service([&] { return Tr("lng_action_user_joined_by_request", {{"from", item.from()}}); });
    case api::messageChatJoinFromCommunity::ID:
      return service([&] {
        const auto community = CommunityName(item, To<api::messageChatJoinFromCommunity>(content).community_id_);
        return community.empty()
                   ? Tr("lng_action_user_joined_via_community_unknown", {{"from", item.from()}})
                   : Tr("lng_action_user_joined_via_community", {{"from", item.from()}, {"community", community}});
      });
    case api::messageChatDeleteMember::ID:
      return service([&] {
        const auto id = To<api::messageChatDeleteMember>(content).user_id_;
        if (IsUser(item.m().sender_id_.get(), id)) return Tr("lng_action_user_left", {{"from", item.from()}});
        return Tr("lng_action_kick_user", {{"from", item.from()}, {"user", item.user(id)}});
      });
    case api::messageChatAddedToCommunity::ID:
      return service([&] { return CommunityAddedText(item, To<api::messageChatAddedToCommunity>(content).community_id_); });
    case api::messageChatRemovedFromCommunity::ID:
      return service([&] { return CommunityRemovedText(item); });
    // Telegram Desktop has no line for a group becoming a supergroup.
    case api::messageChatUpgradeTo::ID:
    case api::messageChatUpgradeFrom::ID:
      return service([&] { return std::string(); });
    case api::messagePinMessage::ID:
      return service([&] { return PinText(item, To<api::messagePinMessage>(content)); });
    case api::messageScreenshotTaken::ID:
      return service([&] {
        return item.out() ? Tr("lng_action_you_took_screenshot")
                          : Tr("lng_action_took_screenshot", {{"from", item.from()}});
      });
    case api::messageChatSetBackground::ID:
      return service([&] { return BackgroundText(item, To<api::messageChatSetBackground>(content)); });
    case api::messageChatSetTheme::ID:
      return service([&] { return ThemeText(item, To<api::messageChatSetTheme>(content)); });
    case api::messageChatSetMessageAutoDeleteTime::ID:
      return service([&] { return AutoDeleteText(item, To<api::messageChatSetMessageAutoDeleteTime>(content)); });
    case api::messageChatBoost::ID:
      return service([&] {
        return item.self() ? Tr("lng_action_boost_apply_me")
                           : TrCount("lng_action_boost_apply", To<api::messageChatBoost>(content).boost_count_,
                                     {{"from", item.from()}});
      });
    case api::messageForumTopicCreated::ID:
      return service([&] { return Tr("lng_action_topic_created", {{"topic", To<api::messageForumTopicCreated>(content).name_}}); });
    case api::messageForumTopicEdited::ID:
      return service([&] { return TopicEditText(item, To<api::messageForumTopicEdited>(content)); });
    case api::messageForumTopicIsClosedToggled::ID:
      return service([&] {
        return To<api::messageForumTopicIsClosedToggled>(content).is_closed_
                   ? Tr("lng_action_topic_closed_inside_by", {{"from", item.from()}})
                   : Tr("lng_action_topic_reopened_inside_by", {{"from", item.from()}});
      });
    case api::messageForumTopicIsHiddenToggled::ID:
      return service([&] {
        return To<api::messageForumTopicIsHiddenToggled>(content).is_hidden_ ? Tr("lng_action_topic_hidden_inside")
                                                                             : Tr("lng_action_topic_unhidden_inside");
      });
    case api::messageSuggestProfilePhoto::ID:
      return service([&] {
        const auto photo = To<api::messageSuggestProfilePhoto>(content).photo_.get();
        const auto video = photo && photo->animation_;
        const auto user = item.otherShort();
        if (item.self()) {
          return video ? Tr("lng_action_suggested_video_me", {{"user", user}})
                       : Tr("lng_action_suggested_photo_me", {{"user", user}});
        }
        return video ? Tr("lng_action_suggested_video", {{"user", user}})
                     : Tr("lng_action_suggested_photo", {{"user", user}});
      });
    case api::messageSuggestBirthdate::ID:
      return service([&] {
        const auto user = item.otherShort();
        return item.self() ? Tr("lng_action_suggested_birthday_me", {{"user", user}})
                           : Tr("lng_action_suggested_birthday", {{"user", user}});
      });
    case api::messageCustomServiceAction::ID:
      return service([&] { return To<api::messageCustomServiceAction>(content).text_; });
    case api::messageGameScore::ID:
      return service([&] { return GameScoreText(item, To<api::messageGameScore>(content)); });
    case api::messageManagedBotCreated::ID:
      return service([&] {
        const auto bot = To<api::messageManagedBotCreated>(content).bot_user_id_;
        return Tr("lng_action_managed_bot_created", {{"from", item.from()}, {"bot", item.user(bot)}});
      });
    case api::messagePaymentSuccessful::ID:
      return service([&] { return PaymentText(item, To<api::messagePaymentSuccessful>(content)); });
    case api::messagePaymentSuccessfulBot::ID:
      return service([&] {
        const auto &payment = To<api::messagePaymentSuccessfulBot>(content);
        const auto amount = Money(payment.total_amount_, payment.currency_);
        return (payment.is_recurring_ && !payment.is_first_recurring_)
                   ? Tr("lng_action_payment_bot_recurring", {{"amount", amount}})
                   : Tr("lng_action_payment_bot_done", {{"amount", amount}});
      });
    case api::messagePaymentRefunded::ID:
      return service([&] {
        const auto &refund = To<api::messagePaymentRefunded>(content);
        return Tr("lng_action_payment_refunded",
                  {{"peer", item.sender(refund.owner_id_)}, {"amount", Money(refund.total_amount_, refund.currency_)}});
      });
    case api::messageGiftedPremium::ID:
      return service([&] {
        const auto &gift = To<api::messageGiftedPremium>(content);
        return GiftCostText(item, gift.gifter_user_id_,
                            gift.currency_.empty() ? TrCount("lng_action_gift_premium_months", gift.month_count_)
                                                   : Money(gift.amount_, gift.currency_));
      });
    case api::messagePremiumGiftCode::ID:
      return service([&] { return GiftCodeText(item, To<api::messagePremiumGiftCode>(content)); });
    case api::messageGiveawayCreated::ID:
      return service([&] { return GiveawayCreatedText(item, To<api::messageGiveawayCreated>(content)); });
    case api::messageGiveawayCompleted::ID:
      return service([&] { return GiveawayCompletedText(To<api::messageGiveawayCompleted>(content)); });
    case api::messageGiftedStars::ID:
      return service([&] {
        const auto &gift = To<api::messageGiftedStars>(content);
        return GiftCostText(item, gift.gifter_user_id_,
                            gift.currency_.empty() ? Stars(gift.star_count_) : Money(gift.amount_, gift.currency_));
      });
    case api::messageGiftedGrams::ID:
      return service([&] {
        const auto &gift = To<api::messageGiftedGrams>(content);
        return GiftCostText(item, gift.gifter_user_id_, Grams(gift.gram_amount_));
      });
    case api::messageGiveawayPrizeStars::ID:
      return service([&] {
        const auto &prize = To<api::messageGiveawayPrizeStars>(content);
        const auto channel = ChatTitle(item.s(), prize.boosted_chat_id_);
        return prize.is_unclaimed_ ? Tr("lng_prize_unclaimed_about", {{"channel", channel}})
                                   : Tr("lng_prize_about", {{"channel", channel}});
      });
    case api::messageTonWalletTransfer::ID:
      return service([&] { return GramTransferText(item, To<api::messageTonWalletTransfer>(content)); });
    case api::messageTonConnectRequest::ID:
      return service([&] { return TonConnectText(To<api::messageTonConnectRequest>(content)); });
    case api::messageGift::ID:
      return service([&] { return GiftText(item, To<api::messageGift>(content)); });
    case api::messageUpgradedGift::ID:
      return service([&] { return UpgradedGiftText(item, To<api::messageUpgradedGift>(content)); });
    case api::messageRefundedUpgradedGift::ID:
      return service([&] { return Tr("lng_action_gift_refunded"); });
    case api::messageUpgradedGiftPurchaseOffer::ID:
      return service([&] { return OfferText(item, To<api::messageUpgradedGiftPurchaseOffer>(content)); });
    case api::messageUpgradedGiftPurchaseOfferRejected::ID:
      return service([&] { return OfferRejectedText(item, To<api::messageUpgradedGiftPurchaseOfferRejected>(content)); });
    case api::messagePaidMessagesRefunded::ID:
      return service([&] {
        const auto stars = To<api::messagePaidMessagesRefunded>(content).star_count_;
        return item.self() ? TrCount("lng_action_paid_message_refund_self", stars, {{"name", item.peerShort()}})
                           : TrCount("lng_action_paid_message_refund", stars, {{"from", item.fromShort()}});
      });
    case api::messagePaidMessagePriceChanged::ID:
      return service([&] {
        const auto stars = To<api::messagePaidMessagePriceChanged>(content).paid_message_star_count_;
        return stars ? TrCount("lng_action_message_price_paid", stars) : Tr("lng_action_message_price_free");
      });
    case api::messageDirectMessagePriceChanged::ID:
      return service([&] {
        const auto &change = To<api::messageDirectMessagePriceChanged>(content);
        if (change.paid_message_star_count_ > 0) {
          return TrCount("lng_action_direct_messages_paid", change.paid_message_star_count_);
        }
        return change.is_enabled_ ? Tr("lng_action_direct_messages_enabled") : Tr("lng_action_direct_messages_disabled");
      });
    case api::messageUnpaidMessagesAllowedByLink::ID:
      return service([&] {
        return item.out() ? Tr("lng_action_you_no_paid_token_used")
                          : Tr("lng_action_no_paid_token_used", {{"from", item.from()}});
      });
    case api::messageChecklistTasksDone::ID:
      return service([&] { return TasksDoneText(item, To<api::messageChecklistTasksDone>(content)); });
    case api::messageChecklistTasksAdded::ID:
      return service([&] { return TasksAddedText(item, To<api::messageChecklistTasksAdded>(content)); });
    case api::messageSuggestedPostApprovalFailed::ID:
      return service([&] { return Tr("lng_action_not_enough_funds"); });
    case api::messageSuggestedPostApproved::ID:
      return service([&] { return Tr("lng_suggest_action_agreement"); });
    case api::messageSuggestedPostDeclined::ID:
      return service([&] { return Tr("lng_action_post_rejected"); });
    case api::messageSuggestedPostPaid::ID:
      return service([&] { return SuggestedPostPaidText(item, To<api::messageSuggestedPostPaid>(content)); });
    case api::messageSuggestedPostRefunded::ID:
      return service([&] {
        return Cast<api::suggestedPostRefundReasonPaymentRefunded>(To<api::messageSuggestedPostRefunded>(content).reason_.get())
                   ? Tr("lng_action_suggest_refund_user")
                   : Tr("lng_action_suggest_refund_admin");
      });
    case api::messageContactRegistered::ID:
      return service([&] { return Tr("lng_action_user_registered", {{"from", item.from()}}); });
    case api::messageUsersShared::ID:
      return service([&] {
        auto names = std::vector<std::string>();
        for (const auto &user : To<api::messageUsersShared>(content).users_) {
          if (user) names.push_back(item.user(user->user_id_));
        }
        return Tr("lng_action_shared_chat_with_bot", {{"chat", JoinUsers(names)}, {"bot", item.title()}});
      });
    case api::messageChatShared::ID:
      return service([&] {
        const auto chat = To<api::messageChatShared>(content).chat_.get();
        const auto title = !chat ? Tr("lng_deleted") : chat->title_.empty() ? ChatTitle(item.s(), chat->chat_id_) : chat->title_;
        return Tr("lng_action_shared_chat_with_bot", {{"chat", title}, {"bot", item.title()}});
      });
    case api::messageBotWriteAccessAllowed::ID:
      return service([&] { return BotAccessText(To<api::messageBotWriteAccessAllowed>(content)); });
    case api::messageWebAppDataSent::ID:
      return service([&] {
        return Tr("lng_action_webview_data_done", {{"text", To<api::messageWebAppDataSent>(content).button_text_}});
      });
    case api::messagePassportDataSent::ID:
      return service([&] { return PassportText(item, To<api::messagePassportDataSent>(content)); });
    case api::messageProximityAlertTriggered::ID:
      return service([&] { return ProximityText(item, To<api::messageProximityAlertTriggered>(content)); });

    // Nothing to show of these: a rich message needs a page view, and the two others reach bots only.
    case api::messageRichMessage::ID:
    case api::messagePassportDataReceived::ID:
    case api::messageWebAppDataReceived::ID:
    case api::messageUnsupported::ID:
      return fallback([&] { return Tr("lng_unsupported_message_title"); });

    default:
      return line(ContentKind::Unknown, [&] { return Tr("lng_unsupported_message_title"); });
  }
}

[[nodiscard]] std::string Preview(std::string text) {
  auto cut = false;
  auto result = FirstChars(FlattenLines(text), kPreviewLimit, &cut);
  return cut ? result + kEllipsis : result;
}

[[nodiscard]] std::string ForwardedFrom(const api::message &message, const Store &store) {
  const auto info = message.forward_info_.get();
  const auto origin = info ? info->origin_.get() : nullptr;
  if (const auto user = Cast<api::messageOriginUser>(origin)) {
    return (user->sender_user_id_ == store.selfId()) ? Tr("lng_from_you") : UserShortName(store, user->sender_user_id_);
  }
  if (const auto chat = Cast<api::messageOriginChat>(origin)) return ChatTitle(store, chat->sender_chat_id_);
  if (const auto channel = Cast<api::messageOriginChannel>(origin)) return ChatTitle(store, channel->chat_id_);
  if (const auto hidden = Cast<api::messageOriginHiddenUser>(origin)) return hidden->sender_name_;
  return {};
}

}  // namespace

ContentKind ClassifyContent(const api::MessageContent &content) {
  return Describe(content, Item(), false).kind;
}

bool IsServiceMessage(const api::MessageContent &content) {
  return ClassifyContent(content) == ContentKind::Service;
}

std::string MessagePreview(const api::message &message, const Store &store, const api::message *related) {
  if (!message.content_) return {};
  return Preview(Describe(*message.content_, Item{&message, &store, related}, true).text);
}

std::string ServiceText(const api::message &message, const Store &store, const api::message *related) {
  if (!message.content_ || !IsServiceMessage(*message.content_)) return {};
  return Describe(*message.content_, Item{&message, &store, related}, true).text;
}

std::string SenderPrefix(const api::message &message, const Store &store) {
  if (!message.content_ || message.is_channel_post_ || IsServiceMessage(*message.content_)) return {};
  const auto chat = store.chat(message.chat_id_);
  if (!chat) return {};
  if (chat->kind == ChatKind::Private || chat->kind == ChatKind::Secret) {
    return (chat->peerId == store.selfId()) ? ForwardedFrom(message, store) : std::string();
  }
  if (chat->kind == ChatKind::Channel) return {};
  const auto sender = message.sender_id_.get();
  return (message.is_outgoing_ || IsUser(sender, store.selfId())) ? Tr("lng_from_you") : SenderShortName(store, sender);
}

}  // namespace Contrail::Td
