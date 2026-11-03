#include "reactions.hpp"

#include "chat_view.hpp"
#include "device_scale.hpp"
#include "main_window.hpp"
#include "picture_decode.hpp"
#include "scroll_helpers.hpp"
#include "td/strings.hpp"
#include "text.hpp"
#include "userpics.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Contrail {
namespace {

namespace api = Td::api;

constexpr auto kMaxRecentUserpics = 3;  // Fewer reactions than this show who, not how many (as Telegram).
constexpr auto kStripCount = 6;         // Reactions at the top of the menu, before the button with all of them.
constexpr auto kWhoReactedLimit = 100;
constexpr auto kAvailableRowSize = 8;
constexpr auto kCustomPrefix = "custom:";
constexpr auto kPaidKey = "paid";
constexpr auto kCountKey = "contrail-reaction-count";
constexpr auto kCustomIdKey = "contrail-custom-emoji";
constexpr auto kCustomSizeKey = "contrail-custom-size";
// Custom emoji pictures kept decoded, each the size it is shown at (a few KB): the ones on the screen and around.
constexpr auto kCustomTextures = 96;
constexpr auto kCustomDownloadPriority = 8;
constexpr auto kCustomAskLimit = 200;  // getCustomEmojiStickers takes this many at once.

const char kStar[] = "\xE2\xAD\x90";  // A paid reaction: Telegram Stars.

template <typename Type, typename Base>
[[nodiscard]] const Type *Cast(const Base *object) {
  return (object && object->get_id() == Type::ID) ? static_cast<const Type *>(object) : nullptr;
}

[[nodiscard]] const api::messageReactions *ReactionsOf(const api::message &message) {
  const auto info = message.interaction_info_.get();
  return info ? info->reactions_.get() : nullptr;
}

[[nodiscard]] std::int64_t CustomId(const std::string &key) {
  return key.starts_with(kCustomPrefix) ? std::atoll(key.c_str() + std::strlen(kCustomPrefix)) : 0;
}

// Telegram's heart is "❤" alone, the dove "🕊" too: fonts draw such characters as plain text in the label's color
// unless U+FE0F asks for the emoji.
[[nodiscard]] std::string EmojiText(const std::string &key) {
  if (key == kPaidKey) return kStar;
  if (g_utf8_strlen(key.c_str(), -1) == 1) return key + "\xEF\xB8\x8F";
  return key;
}

[[nodiscard]] std::string SenderKey(const api::MessageSender *sender) {
  if (const auto user = Cast<api::messageSenderUser>(sender)) return "u" + std::to_string(user->user_id_);
  if (const auto chat = Cast<api::messageSenderChat>(sender)) return "c" + std::to_string(chat->chat_id_);
  return "?";
}

[[nodiscard]] GtkWidget *SenderAvatar(Td::Store &store, const api::MessageSender *sender, int size) {
  if (const auto user = Cast<api::messageSenderUser>(sender)) return MakeUserAvatar(store, user->user_id_, size);
  if (const auto chat = Cast<api::messageSenderChat>(sender)) return MakeAvatar(store, chat->chat_id_, size);
  return adw_avatar_new(size, nullptr, TRUE);
}

[[nodiscard]] bool IsChannel(const ChatView &chat) {
  const auto known = chat.store().chat(chat.chat());
  return known && known->kind == Td::ChatKind::Channel;
}

[[nodiscard]] bool IsPremium(const Td::Store &store) {
  const auto self = store.user(store.selfId());
  return self && self->premium;
}

[[nodiscard]] bool Chosen(const api::message &message, const std::string &key) {
  if (const auto reactions = ReactionsOf(message)) {
    for (const auto &reaction : reactions->reactions_) {
      if (reaction && reaction->is_chosen_ && ReactionKey(reaction->type_.get()) == key) return true;
    }
  }
  return false;
}

// --- custom emoji pictures ---------------------------------------------------

void FreeWeak(GWeakRef *ref) {
  g_weak_ref_clear(ref);
  g_free(ref);
}

// The pictures of custom emoji, by their ids: asked from TDLib once, a few at a time, then their files. A picture
// is drawn once, as a still: dozens of animated ones on the screen would cost the battery (as Telegram's power
// saving, which stops them); an animated one shows its thumbnail, or the emoji it stands for when the thumbnail is
// animated too. The views waiting for one are weak: a view gone before its picture comes leaves nothing behind.
class CustomEmojiCache final {
 public:
  explicit CustomEmojiCache(Td::Store &store) : _store(store) {
    _store.onFile([this](const Td::File &file) { fileChanged(file); }, _lifetime);
  }

  ~CustomEmojiCache() {
    if (_askSource) g_source_remove(_askSource);
    for (auto &[id, entry] : _entries) {
      for (auto &sized : entry.sizes) g_clear_object(&sized.texture);
      std::ranges::for_each(entry.waiting, FreeWeak);
    }
  }

  [[nodiscard]] const Td::Store *store() const { return &_store; }

  // `view` is the box ReactionView makes: its picture, and the emoji in text if there is no picture.
  void bind(GtkWidget *view, std::int64_t id) {
    auto &entry = _entries[id];
    if (show(entry, view)) return;
    wait(entry, view);
    if (!entry.asked) {
      entry.asked = true;
      _toAsk.push_back(id);
      // After the views of this pass are made: one request for all of them.
      if (!_askSource) {
        _askSource = g_idle_add_once(+[](gpointer data) {
          auto *self = static_cast<CustomEmojiCache *>(data);
          self->_askSource = 0;
          self->ask();
        }, this);
      }
    }
  }

  [[nodiscard]] std::string state() const {
    auto known = 0, pictures = 0, textures = 0;
    for (const auto &[id, entry] : _entries) {
      known += entry.known;
      pictures += !entry.path.empty();
      textures += int(entry.sizes.size());
    }
    return "custom=" + std::to_string(_entries.size()) + " known=" + std::to_string(known)
           + " pictures=" + std::to_string(pictures) + " textures=" + std::to_string(textures);
  }

 private:
  struct Sized {
    int pixels = 0;
    GdkTexture *texture = nullptr;
    std::uint64_t used = 0;
  };
  struct Entry {
    bool asked = false;
    bool known = false;   // TDLib told about it.
    bool broken = false;  // No still picture: the emoji in text.
    Td::FileId file = 0;
    std::string path;  // Of the file, once it is here.
    std::string emoji;
    std::vector<Sized> sizes;
    std::vector<GWeakRef *> waiting;
  };

  // The picture, or the emoji in text: false while neither is known.
  bool show(Entry &entry, GtkWidget *view) {
    auto *picture = gtk_widget_get_first_child(view);
    if (entry.broken || (entry.known && !entry.file)) {
      if (!entry.known) return false;
      auto *label = gtk_widget_get_next_sibling(picture);
      if (!label) {
        label = gtk_label_new(nullptr);
        gtk_widget_add_css_class(label, "reaction-custom-alt");
        gtk_box_append(GTK_BOX(view), label);
      }
      gtk_label_set_text(GTK_LABEL(label), EmojiText(entry.emoji.empty() ? "\xE2\x9D\x94" : entry.emoji).c_str());
      gtk_widget_set_visible(label, TRUE);
      gtk_widget_set_visible(picture, FALSE);
      return true;
    }
    if (entry.path.empty()) return false;
    const auto size = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), kCustomSizeKey));
    auto *texture = textureFor(entry, DevicePixels(view, size));
    if (!texture) {
      entry.broken = true;
      return show(entry, view);
    }
    gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(texture));
    // The emoji in text stood here while the file couldn't be read.
    gtk_widget_set_visible(picture, TRUE);
    if (auto *label = gtk_widget_get_next_sibling(picture)) gtk_widget_set_visible(label, FALSE);
    return true;
  }

  static void wait(Entry &entry, GtkWidget *view) {
    auto present = false;
    std::erase_if(entry.waiting, [&](GWeakRef *ref) {
      auto *object = g_weak_ref_get(ref);
      if (!object) {
        FreeWeak(ref);
        return true;
      }
      present = present || object == static_cast<gpointer>(view);
      g_object_unref(object);
      return false;
    });
    if (present) return;
    auto *ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, view);
    entry.waiting.push_back(ref);
  }

  void ask() {
    auto ids = std::vector<std::int64_t>();
    while (!_toAsk.empty() && int(ids.size()) < kCustomAskLimit) {
      ids.push_back(_toAsk.back());
      _toAsk.pop_back();
    }
    if (!_toAsk.empty()) {
      _askSource = g_idle_add_once(+[](gpointer data) {
        auto *self = static_cast<CustomEmojiCache *>(data);
        self->_askSource = 0;
        self->ask();
      }, this);
    }
    if (ids.empty()) return;
    _store.client().request<api::stickers>(
        api::make_object<api::getCustomEmojiStickers>(std::vector<std::int64_t>(ids)), _lifetime,
        [this, ids](api::stickers &stickers) { answered(ids, stickers); },
        [this, ids](api::error &) {
          for (const auto id : ids) {
            auto &entry = _entries[id];
            entry.known = entry.broken = true;
            showWaiting(entry);
          }
        });
  }

  // A still picture of the sticker, if it has one: a static sticker itself, else a thumbnail that isn't animated.
  static const api::file *StillFile(const api::sticker &sticker) {
    if (Cast<api::stickerFormatWebp>(sticker.format_.get()) && sticker.sticker_) return sticker.sticker_.get();
    if (const auto thumbnail = sticker.thumbnail_.get(); thumbnail && thumbnail->file_ && thumbnail->format_) {
      switch (thumbnail->format_->get_id()) {
        case api::thumbnailFormatWebp::ID:
        case api::thumbnailFormatJpeg::ID:
        case api::thumbnailFormatPng::ID: return thumbnail->file_.get();
        default: break;
      }
    }
    return nullptr;
  }

  void answered(const std::vector<std::int64_t> &ids, api::stickers &stickers) {
    auto downloads = std::vector<Td::FileId>();
    for (const auto &sticker : stickers.stickers_) {
      const auto type = sticker ? Cast<api::stickerFullTypeCustomEmoji>(sticker->full_type_.get()) : nullptr;
      if (!type) continue;
      auto &entry = _entries[type->custom_emoji_id_];
      entry.known = true;
      entry.emoji = sticker->emoji_;
      const auto file = StillFile(*sticker);
      if (!file) continue;
      entry.file = file->id_;
      _byFile[file->id_] = type->custom_emoji_id_;
      if (file->local_ && file->local_->is_downloading_completed_) {
        entry.path = file->local_->path_;
      } else if (const auto known = _store.file(file->id_); known && known->complete) {
        entry.path = known->path;
      } else {
        downloads.push_back(file->id_);
      }
    }
    for (const auto id : ids) {
      auto &entry = _entries[id];
      if (!entry.known) entry.known = entry.broken = true;  // TDLib doesn't know of it.
      if (entry.broken || !entry.file || !entry.path.empty()) showWaiting(entry);
    }
    // Last: a download can answer before it returns.
    for (const auto file : downloads) _store.download(file, kCustomDownloadPriority);
  }

  void fileChanged(const Td::File &file) {
    const auto it = _byFile.find(file.id);
    if (it == _byFile.end() || !file.complete) return;
    auto &entry = _entries[it->second];
    entry.path = file.path;
    entry.broken = false;
    showWaiting(entry);
  }

  void showWaiting(Entry &entry) {
    auto waiting = std::exchange(entry.waiting, {});
    for (auto *ref : waiting) {
      if (auto *view = g_weak_ref_get(ref)) {
        show(entry, GTK_WIDGET(view));
        g_object_unref(view);
      }
      FreeWeak(ref);
    }
    trim();
  }

  GdkTexture *textureFor(Entry &entry, int pixels) {
    for (auto &sized : entry.sizes) {
      if (sized.pixels == pixels) {
        sized.used = ++_clock;
        return sized.texture;
      }
    }
    // A few KB, read right here as the pinned bar's picture is: never waits for a large photo being decoded.
    const auto options = DecodeOptions{.pixbufFallback = true, .waitForLarge = false};
    GdkTexture *texture = nullptr;
    if (auto *pixbuf = DecodePictureFile(entry.path.c_str(), pixels, pixels, options)) {
      texture = TextureFromPixbuf(pixbuf);
    }
    if (!texture) return nullptr;
    entry.sizes.push_back({pixels, texture, ++_clock});
    ++_textures;
    return texture;
  }

  // The textures used longest ago go; views still showing one keep it until they go too.
  void trim() {
    while (_textures > kCustomTextures) {
      Entry *oldest = nullptr;
      auto index = std::size_t(0);
      for (auto &[id, entry] : _entries) {
        for (auto k = std::size_t(0); k < entry.sizes.size(); ++k) {
          if (!oldest || entry.sizes[k].used < oldest->sizes[index].used) {
            oldest = &entry;
            index = k;
          }
        }
      }
      if (!oldest) break;
      g_object_unref(oldest->sizes[index].texture);
      oldest->sizes.erase(oldest->sizes.begin() + std::ptrdiff_t(index));
      --_textures;
    }
  }

  Td::Store &_store;
  Td::Lifetime _lifetime;
  std::unordered_map<std::int64_t, Entry> _entries;
  std::unordered_map<Td::FileId, std::int64_t> _byFile;
  std::vector<std::int64_t> _toAsk;
  guint _askSource = 0;
  int _textures = 0;
  std::uint64_t _clock = 0;
};

std::unique_ptr<CustomEmojiCache> &CurrentCache() {
  static std::unique_ptr<CustomEmojiCache> cache;
  return cache;
}

CustomEmojiCache &CacheFor(Td::Store &store) {
  auto &cache = CurrentCache();
  if (!cache || cache->store() != &store) cache = std::make_unique<CustomEmojiCache>(store);
  return *cache;
}

// --- views -------------------------------------------------------------------

// The size of a reaction's emoji, as large as the emoji text next to it.
int ReactionSize(const char *cssClass) {
  const auto is = [&](const char *name) { return !std::strcmp(cssClass, name); };
  return is("reaction-strip-emoji") ? 26 : is("reaction-grid-emoji") ? 32 : is("reaction-row-emoji") ? 22 : 18;
}

// The emoji of a reaction: a label, or the picture of a custom emoji (Telegram Premium).
GtkWidget *ReactionView(Td::Store &store, const std::string &key, const char *cssClass) {
  if (const auto custom = CustomId(key)) {
    const auto size = ReactionSize(cssClass);
    auto *view = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_valign(view, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(view, cssClass);
    auto *picture = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_size_request(picture, size, size);
    gtk_widget_add_css_class(picture, "reaction-custom");
    gtk_box_append(GTK_BOX(view), picture);
    g_object_set_data_full(G_OBJECT(view), kCustomIdKey, new std::int64_t(custom),
                           [](gpointer data) { delete static_cast<std::int64_t *>(data); });
    g_object_set_data(G_OBJECT(view), kCustomSizeKey, GINT_TO_POINTER(size));
    CacheFor(store).bind(view, custom);
    return view;
  }
  auto *label = gtk_label_new(EmojiText(key).c_str());
  gtk_widget_add_css_class(label, cssClass);
  return label;
}

// The reactions offered for a message, as Telegram's reaction panel offers them: its top ones, the recent, the
// popular. Paid reactions need Telegram Stars, which this app doesn't handle; tags in Saved Messages and custom
// emoji are for Telegram Premium.
std::vector<std::string> PossibleReactions(const Td::Store &store, const api::availableReactions *available) {
  auto result = std::vector<std::string>();
  if (!available || available->unavailability_reason_) return result;
  const auto premium = IsPremium(store);
  if (available->are_tags_ && !premium) return result;
  for (const auto *list : {&available->top_reactions_, &available->recent_reactions_, &available->popular_reactions_}) {
    for (const auto &reaction : *list) {
      if (!reaction || (reaction->needs_premium_ && !premium)) continue;
      auto key = ReactionKey(reaction->type_.get());
      if (key.empty() || key == kPaidKey || std::ranges::count(result, key)) continue;
      result.push_back(std::move(key));
    }
  }
  return result;
}

GtkWidget *ChooserButton(ChatView &chat, const api::message &message, const std::string &key, const char *labelClass,
                         std::function<void()> chosen) {
  auto *button = gtk_button_new();
  gtk_button_set_child(GTK_BUTTON(button), ReactionView(chat.store(), key, labelClass));
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_add_css_class(button, "reaction-choice");
  if (Chosen(message, key)) gtk_widget_add_css_class(button, "chosen");
  g_object_set_data_full(G_OBJECT(button), "contrail-reaction", g_strdup(key.c_str()), g_free);
  const auto id = message.id_;
  const auto token = chat.lifetime().token();
  auto *view = &chat;
  OnClicked(button, [=] {
    if (!token.expired()) {
      if (const auto message = view->heldMessage(id)) ToggleReaction(*view, *message, key);
    }
    if (chosen) chosen();
  });
  return button;
}

GtkWidget *MakeDialog(const std::string &title, int height, GtkWidget *content) {
  auto *dialog = adw_dialog_new();
  adw_dialog_set_title(dialog, title.c_str());
  adw_dialog_set_content_width(dialog, 420);
  adw_dialog_set_content_height(dialog, height);
  auto *view = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), content);
  adw_dialog_set_child(dialog, view);
  return GTK_WIDGET(dialog);
}

}  // namespace

std::string ReactionKey(const api::ReactionType *type) {
  if (const auto emoji = Cast<api::reactionTypeEmoji>(type)) return emoji->emoji_;
  if (const auto custom = Cast<api::reactionTypeCustomEmoji>(type)) {
    return kCustomPrefix + std::to_string(custom->custom_emoji_id_);
  }
  if (Cast<api::reactionTypePaid>(type)) return kPaidKey;
  return {};
}

api::object_ptr<api::ReactionType> ReactionFromKey(const std::string &key) {
  if (key.empty()) return nullptr;
  if (key == kPaidKey) return api::make_object<api::reactionTypePaid>();
  if (const auto custom = CustomId(key)) return api::make_object<api::reactionTypeCustomEmoji>(custom);
  return api::make_object<api::reactionTypeEmoji>(key);
}

int ReactionsCount(const api::message &message) {
  auto result = 0;
  if (const auto reactions = ReactionsOf(message)) {
    for (const auto &reaction : reactions->reactions_) result += reaction ? reaction->total_count_ : 0;
  }
  return result;
}

void ToggleReaction(ChatView &chat, const api::message &message, const std::string &key) {
  if (key.empty() || key == kPaidKey || message.sending_state_) return;
  auto function = Chosen(message, key)
      ? Td::FunctionPtr(api::make_object<api::removeMessageReaction>(message.chat_id_, message.id_, ReactionFromKey(key)))
      : Td::FunctionPtr(api::make_object<api::addMessageReaction>(message.chat_id_, message.id_, ReactionFromKey(key),
                                                                  false, true));
  // TDLib shows the change at once (updateMessageInteractionInfo); an error goes to the toast.
  chat.store().client().request<api::ok>(std::move(function), chat.lifetime(), [](api::ok &) {});
  TriggerFeedback("button-pressed");
}

void ToggleQuickReaction(ChatView &chat, const api::message &message) {
  if (message.sending_state_) return;
  const auto id = message.id_;
  auto *view = &chat;
  chat.store().client().request<api::availableReactions>(
      api::make_object<api::getMessageAvailableReactions>(message.chat_id_, id, kAvailableRowSize), chat.lifetime(),
      [view, id](api::availableReactions &available) {
        const auto message = view->heldMessage(id);
        if (!message || available.are_tags_) return;
        const auto possible = PossibleReactions(view->store(), &available);
        if (possible.empty()) return;
        const auto quick = ReactionKey(view->store().defaultReaction());
        ToggleReaction(*view, *message, std::ranges::count(possible, quick) ? quick : possible.front());
      },
      [](api::error &) {});
}

bool CanShowWhoReacted(const ChatView &chat, const api::message &message) {
  const auto reactions = ReactionsOf(message);
  return reactions && !reactions->reactions_.empty() && reactions->can_get_added_reactions_ && !IsChannel(chat);
}

// --- under the message -------------------------------------------------------

namespace {

// Who reacted, in place of the number: in a chat that tells, when everybody who did is among the recent ones.
const api::messageReaction *WithUserpics(const ChatView &chat, const api::message &message,
                                         const api::messageReaction &reaction) {
  if (IsChannel(chat) || ReactionsCount(message) > kMaxRecentUserpics) return nullptr;
  return int(reaction.recent_sender_ids_.size()) == reaction.total_count_ ? &reaction : nullptr;
}

bool CountShown(const api::messageReactions &reactions, const api::messageReaction &reaction) {
  return reaction.total_count_ > 0 && !(reactions.are_tags_ && reaction.total_count_ == 1);
}

}  // namespace

std::string ReactionsLayoutKey(const ChatView &chat, const api::message &message) {
  const auto reactions = ReactionsOf(message);
  if (!reactions) return {};
  auto result = std::string(reactions->are_tags_ ? "t" : "");
  for (const auto &reaction : reactions->reactions_) {
    if (!reaction) continue;
    result += ReactionKey(reaction->type_.get()) + (reaction->is_chosen_ ? "+" : ";");
    if (WithUserpics(chat, message, *reaction)) {
      for (const auto &sender : reaction->recent_sender_ids_) result += SenderKey(sender.get()) + ',';
    } else {
      result += CountShown(*reactions, *reaction) ? "#" : "-";
    }
    result += '|';
  }
  return result;
}

std::string ReactionsCountKey(const api::message &message) {
  auto result = std::string();
  if (const auto reactions = ReactionsOf(message)) {
    for (const auto &reaction : reactions->reactions_) {
      if (reaction) result += std::to_string(reaction->total_count_) + ',';
    }
  }
  return result;
}

GtkWidget *MakeReactionsBar(ChatView &chat, const api::message &message) {
  const auto reactions = ReactionsOf(message);
  if (!reactions || reactions->reactions_.empty()) return nullptr;
  auto &store = chat.store();
  auto *flow = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), FALSE);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 20);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 4);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 4);
  gtk_widget_set_halign(flow, GTK_ALIGN_START);
  gtk_widget_add_css_class(flow, "reactions");
  const auto id = message.id_;
  const auto token = chat.lifetime().token();
  auto *view = &chat;
  for (const auto &reaction : reactions->reactions_) {
    if (!reaction) continue;
    const auto key = ReactionKey(reaction->type_.get());
    auto *chip = gtk_button_new();
    gtk_widget_add_css_class(chip, "reaction-chip");
    if (reaction->is_chosen_) gtk_widget_add_css_class(chip, "chosen");
    if (key == kPaidKey) gtk_widget_add_css_class(chip, "paid");
    if (reactions->are_tags_) gtk_widget_add_css_class(chip, "tag");
    g_object_set_data_full(G_OBJECT(chip), "contrail-reaction", g_strdup(key.c_str()), g_free);
    auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(box), ReactionView(store, key, "reaction-emoji"));
    if (const auto who = WithUserpics(chat, message, *reaction)) {
      auto *avatars = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_add_css_class(avatars, "reaction-userpics");
      for (const auto &sender : who->recent_sender_ids_) {
        gtk_box_append(GTK_BOX(avatars), SenderAvatar(store, sender.get(), 18));
      }
      gtk_box_append(GTK_BOX(box), avatars);
    } else if (CountShown(*reactions, *reaction)) {
      auto *count = gtk_label_new(ShortCount(reaction->total_count_).c_str());
      gtk_widget_add_css_class(count, "reaction-count");
      gtk_box_append(GTK_BOX(box), count);
      g_object_set_data(G_OBJECT(chip), kCountKey, count);
    }
    gtk_button_set_child(GTK_BUTTON(chip), box);
    // Taps here are taps on the message: as for its links, not when they stop a scroll.
    OnClicked(chip, [=] {
      if (!TapAllowed() || token.expired()) return;
      if (const auto message = view->heldMessage(id)) ToggleReaction(*view, *message, key);
    });
    auto *longPress = gtk_gesture_long_press_new();
    gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(longPress), TRUE);
    Connect<void(GtkGestureLongPress *, double, double)>(
        longPress, "pressed", [=](GtkGestureLongPress *gesture, double, double) {
          if (token.expired()) return;
          const auto message = view->heldMessage(id);
          if (!message || !CanShowWhoReacted(*view, *message)) return;
          gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
          ShowWhoReacted(*view, *message, key);
        });
    gtk_widget_add_controller(chip, GTK_EVENT_CONTROLLER(longPress));
    gtk_flow_box_append(GTK_FLOW_BOX(flow), chip);
    auto *child = gtk_widget_get_parent(chip);
    gtk_widget_set_focusable(child, FALSE);
    gtk_widget_add_css_class(child, "reaction-cell");
  }
  return flow;
}

// The same chips in the same order (ReactionsLayoutKey): only their numbers.
void UpdateReactionsBar(GtkWidget *bar, const api::message &message) {
  const auto reactions = ReactionsOf(message);
  if (!reactions || !bar) return;
  auto *child = gtk_widget_get_first_child(bar);
  for (const auto &reaction : reactions->reactions_) {
    if (!reaction) continue;
    if (!child) break;
    auto *chip = gtk_flow_box_child_get_child(GTK_FLOW_BOX_CHILD(child));
    if (auto *count = chip ? static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(chip), kCountKey)) : nullptr) {
      gtk_label_set_text(GTK_LABEL(count), ShortCount(reaction->total_count_).c_str());
    }
    child = gtk_widget_get_next_sibling(child);
  }
}

// --- choosing ----------------------------------------------------------------

GtkWidget *MakeReactionsStrip(ChatView &chat, const api::message &message,
                              std::shared_ptr<api::availableReactions> available, std::function<void()> chosen) {
  const auto possible = PossibleReactions(chat.store(), available.get());
  if (possible.empty()) return nullptr;
  auto *strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(strip, "reactions-strip");
  const auto shown = std::min(int(possible.size()), kStripCount);
  for (auto i = 0; i != shown; ++i) {
    gtk_box_append(GTK_BOX(strip), ChooserButton(chat, message, possible[i], "reaction-strip-emoji", chosen));
  }
  if (int(possible.size()) > shown) {
    auto *more = MakeIconButton("pan-down-symbolic", Tr("lng_manage_peer_reactions").c_str(), "flat");
    gtk_widget_add_css_class(more, "circular");
    gtk_widget_add_css_class(more, "reactions-more");
    gtk_widget_set_valign(more, GTK_ALIGN_CENTER);
    const auto id = message.id_;
    const auto token = chat.lifetime().token();
    auto *view = &chat;
    OnClicked(more, [=] {
      if (chosen) chosen();
      if (!token.expired()) ShowReactionsChooser(*view, id, available);
    });
    gtk_box_append(GTK_BOX(strip), more);
  }
  return strip;
}

void ShowReactionsChooser(ChatView &chat, std::int64_t id, std::shared_ptr<api::availableReactions> available) {
  const auto message = chat.heldMessage(id);
  const auto possible = PossibleReactions(chat.store(), available.get());
  if (!message || possible.empty()) return;
  auto *flow = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 5);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 8);
  gtk_widget_set_valign(flow, GTK_ALIGN_START);
  SetMarginAll(flow, 8);
  auto *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), flow);
  gtk_widget_set_vexpand(scroll, TRUE);
  auto *dialog = MakeDialog(Tr("lng_manage_peer_reactions"), 460, scroll);
  g_object_set_data(G_OBJECT(dialog), "contrail-reactions-grid", flow);
  for (const auto &key : possible) {
    auto *button = ChooserButton(chat, *message, key, "reaction-grid-emoji", [=] { adw_dialog_close(ADW_DIALOG(dialog)); });
    gtk_flow_box_append(GTK_FLOW_BOX(flow), button);
    gtk_widget_set_focusable(gtk_widget_get_parent(button), FALSE);
  }
  chat.holdDialog(ADW_DIALOG(dialog));
  adw_dialog_present(ADW_DIALOG(dialog), chat.window().widget());
}

// --- who reacted -------------------------------------------------------------

void ShowWhoReacted(ChatView &chat, const api::message &message, const std::string &filter) {
  if (!CanShowWhoReacted(chat, message)) return;
  auto *stack = gtk_stack_new();
  auto *spinner = adw_spinner_new();
  gtk_widget_set_size_request(spinner, 32, 32);
  gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
  gtk_stack_add_named(GTK_STACK(stack), spinner, "loading");
  auto *list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(list, "boxed-list");
  gtk_widget_set_valign(list, GTK_ALIGN_START);
  SetMarginAll(list, 12);
  auto *clamp = adw_clamp_new();
  adw_clamp_set_child(ADW_CLAMP(clamp), list);
  auto *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
  gtk_stack_add_named(GTK_STACK(stack), scroll, "list");
  auto *dialog = MakeDialog(TrCount("lng_context_seen_reacted", ReactionsCount(message)), 520, stack);
  g_object_set_data(G_OBJECT(dialog), "contrail-who-reacted", list);

  // The answer is dropped when the dialog has gone, or the chat.
  auto *lifetime = AttachData(dialog, "contrail-who-reacted-lifetime", std::make_unique<Td::Lifetime>());
  auto &store = chat.store();
  auto &window = chat.window();
  const auto token = chat.lifetime().token();
  store.client().request<api::addedReactions>(
      api::make_object<api::getMessageAddedReactions>(message.chat_id_, message.id_, ReactionFromKey(filter),
                                                      std::string(), kWhoReactedLimit),
      *lifetime,
      [=, &store, &window](api::addedReactions &added) {
        if (token.expired()) return;
        for (const auto &reaction : added.reactions_) {
          if (!reaction) continue;
          const auto sender = reaction->sender_id_.get();
          auto name = std::string();
          auto open = std::function<void()>();
          if (const auto user = Cast<api::messageSenderUser>(sender)) {
            const auto known = store.user(user->user_id_);
            name = user->user_id_ == store.selfId() ? Tr("lng_saved_messages")
                   : known                          ? Td::DisplayName(*known)
                                                    : std::string();
            open = [id = user->user_id_] { HandleLink("tg-user:" + std::to_string(id)); };
          } else if (const auto from = Cast<api::messageSenderChat>(sender)) {
            const auto known = store.chat(from->chat_id_);
            name = known ? known->title : std::string();
            open = [&window, id = from->chat_id_] { window.openChat(id); };
          }
          auto *row = adw_action_row_new();
          adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), Escape(name).c_str());
          adw_action_row_add_prefix(ADW_ACTION_ROW(row), SenderAvatar(store, sender, 36));
          adw_action_row_add_suffix(ADW_ACTION_ROW(row),
                                    ReactionView(store, ReactionKey(reaction->type_.get()), "reaction-row-emoji"));
          gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
          Connect<void(AdwActionRow *)>(row, "activated", [=](AdwActionRow *) {
            adw_dialog_close(ADW_DIALOG(dialog));
            if (open) open();
          });
          gtk_list_box_append(GTK_LIST_BOX(list), row);
        }
        gtk_stack_set_visible_child_name(GTK_STACK(stack), "list");
      },
      [=](api::error &error) {
        ShowToast(error.message_);
        adw_dialog_close(ADW_DIALOG(dialog));
      });
  chat.holdDialog(ADW_DIALOG(dialog));
  adw_dialog_present(ADW_DIALOG(dialog), window.widget());
}

std::string CustomReactionsState() {
  const auto &cache = CurrentCache();
  return cache ? cache->state() : std::string("custom=0");
}

}  // namespace Contrail
