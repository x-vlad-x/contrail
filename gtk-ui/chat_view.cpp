#include "chat_view.hpp"

#include "chat_live_locations.hpp"
#include "forward.hpp"
#include "live_location.hpp"
#include "local_state.hpp"
#include "location_ui.hpp"
#include "main_window.hpp"
#include "attach.hpp"
#include "media/audio_player.hpp"
#include "voice_record_bar.hpp"
#include "memory_trim.hpp"
#include "message.hpp"
#include "photo_viewer.hpp"
#include "picture_decode.hpp"
#include "reactions.hpp"
#include "scroll_helpers.hpp"
#include "td/chat_row.hpp"
#include "td/dates.hpp"
#include "td/message_text.hpp"
#include "td/strings.hpp"
#include "userpics.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Contrail {
namespace {

namespace api = Td::api;

constexpr auto kMessagesPerPage = 50;
constexpr auto kPreloadHeight = 6000.;  // About eight phone screens: pages come before a fling gets there.
// Room above the messages for older pages: a page of a channel with photos is up to ~20000px high,
// so this lasts ten pages of flinging without a stop. Much larger and float rounding of the
// render transforms starts to blur the text.
constexpr auto kTopReserve = 200000;
constexpr auto kTopReserveLow = 100000;
constexpr auto kTopReserveHigh = 300000;  // Rows removed above the screen leave their height to the reserve.
// Rows within this distance from the screen stay when the list stands still (well beyond where the next ones are
// loaded, so the two don't take turns), and only when there are more rows than this.
constexpr auto kKeepHeight = 2.5 * kPreloadHeight;
constexpr auto kMaxRows = 300;
// Pictures are decoded in the rows this close to the screen, about four phone screens each way: a fling finds them
// ready (a photo takes 25–50 ms). Beyond the farther distance they give their textures back; between the two a row
// keeps what it has, so one at the edge doesn't take turns.
constexpr auto kPictureNearHeight = 3000.;
constexpr auto kPictureFarHeight = 4500.;
// A fling or a drag of the scroll bar that stops this far above the rows doesn't walk there page by page: the
// messages where it stopped are loaded at once (jumpUp()).
constexpr auto kJumpGap = kPreloadHeight;
constexpr auto kJumpRowMin = 40.;
constexpr auto kJumpRowMax = 400.;
constexpr auto kDraftDelay = 2000u;
constexpr auto kStatusPeriod = 30000u;
constexpr auto kTypingPeriod = gint64(5) * G_USEC_PER_SEC;
// A server message id is the server's number shifted by 20 bits. History asked from the first one is the start of a
// chat, whichever messages are left there.
constexpr auto kFirstMessageId = std::int64_t(1) << 20;
constexpr auto kPinnedPreviewSize = 36;
constexpr auto kPinnedPage = 100;
constexpr auto kNoMessage = std::numeric_limits<std::int64_t>::max();

void OpenPath(const std::string &path) {
  if (auto *uri = g_filename_to_uri(path.c_str(), nullptr, nullptr)) {
    OpenUri(uri);
    g_free(uri);
  }
}

template <typename Type>
[[nodiscard]] const Type *Cast(const api::Object *object) {
  return (object && object->get_id() == Type::ID) ? static_cast<const Type *>(object) : nullptr;
}

[[nodiscard]] bool IsLocal(const MessageItem *item) {
  return item->local();
}

[[nodiscard]] api::object_ptr<api::formattedText> Formatted(const std::string &text) {
  return api::make_object<api::formattedText>(text, std::vector<api::object_ptr<api::textEntity>>());
}

[[nodiscard]] api::object_ptr<api::InputMessageContent> InputText(const std::string &text) {
  return api::make_object<api::inputMessageText>(Formatted(text), nullptr, true);
}

[[nodiscard]] api::object_ptr<api::InputMessageReplyTo> ReplyTo(std::int64_t id) {
  return id ? api::make_object<api::inputMessageReplyToMessage>(id, nullptr, 0, std::string()) : nullptr;
}

// The text a message has: its own, or the caption of its media.
[[nodiscard]] const api::formattedText *MessageTextOf(const api::message &message) {
  const auto content = message.content_.get();
  if (const auto text = Cast<api::messageText>(content)) return text->text_.get();
  if (const auto photo = Cast<api::messagePhoto>(content)) return photo->caption_.get();
  if (const auto video = Cast<api::messageVideo>(content)) return video->caption_.get();
  if (const auto document = Cast<api::messageDocument>(content)) return document->caption_.get();
  if (const auto audio = Cast<api::messageAudio>(content)) return audio->caption_.get();
  if (const auto animation = Cast<api::messageAnimation>(content)) return animation->caption_.get();
  if (const auto voice = Cast<api::messageVoiceNote>(content)) return voice->caption_.get();
  return nullptr;
}

[[nodiscard]] std::string PlainText(const api::message &message) {
  const auto text = MessageTextOf(message);
  return text ? text->text_ : std::string();
}

[[nodiscard]] std::int64_t SenderKey(const api::message &message) {
  if (const auto user = Cast<api::messageSenderUser>(message.sender_id_.get())) return user->user_id_;
  if (const auto chat = Cast<api::messageSenderChat>(message.sender_id_.get())) return chat->chat_id_;
  return 0;
}

// The name and the color of who sent a message.
void DescribeSender(const Td::Store &store, const api::message &message, std::string &name, int &color) {
  if (const auto sender = Cast<api::messageSenderUser>(message.sender_id_.get())) {
    if (const auto user = store.user(sender->user_id_)) {
      name = Td::DisplayName(*user);
      color = user->accentColor;
    }
  } else if (const auto sender = Cast<api::messageSenderChat>(message.sender_id_.get())) {
    if (const auto chat = store.chat(sender->chat_id_)) {
      name = chat->title;
      color = chat->accentColor;
    }
  }
}

// Who wrote a message to forward: of one forwarded already, who wrote it first.
[[nodiscard]] std::string ForwardSenderName(const Td::Store &store, const api::message &message) {
  const auto info = message.forward_info_.get();
  const auto origin = info ? info->origin_.get() : nullptr;
  if (const auto user = Cast<api::messageOriginUser>(origin)) {
    const auto known = store.user(user->sender_user_id_);
    return known ? Td::DisplayName(*known) : std::string();
  }
  if (const auto hidden = Cast<api::messageOriginHiddenUser>(origin)) return hidden->sender_name_;
  auto chat = Td::ChatId(0);
  if (const auto sender = Cast<api::messageOriginChat>(origin)) chat = sender->sender_chat_id_;
  if (const auto channel = Cast<api::messageOriginChannel>(origin)) chat = channel->chat_id_;
  if (chat) {
    const auto known = store.chat(chat);
    return known ? known->title : std::string();
  }
  auto name = std::string();
  auto color = 0;
  DescribeSender(store, message, name, color);
  return name;
}

// "online", "last seen today at 12:34".
[[nodiscard]] std::string OnlineText(const Td::User &user) {
  const auto now = Td::Now();
  if (user.onlineUntil > now) return Tr("lng_status_online");
  switch (user.lastSeen) {
    case 0: return Tr("lng_status_offline");
    case -1: return Tr("lng_status_recently");
    case -2: return Tr("lng_status_last_week");
    case -3: return Tr("lng_status_last_month");
    default: break;
  }
  const auto ago = now - user.lastSeen;
  if (ago < 60) return Tr("lng_status_lastseen_now");
  if (ago < 3600) return TrCount("lng_status_lastseen_minutes", ago / 60);
  const auto days = Td::DayNumber(now) - Td::DayNumber(user.lastSeen);
  if (days == 0 && ago < 12 * 3600) return TrCount("lng_status_lastseen_hours", ago / 3600);
  const auto time = Td::FormatTime(user.lastSeen);
  if (days == 0) return Tr("lng_status_lastseen_today", {{"time", time}});
  if (days == 1) return Tr("lng_status_lastseen_yesterday", {{"time", time}});
  return Tr("lng_status_lastseen_date", {{"date", Td::FormatDay(user.lastSeen)}});
}

[[nodiscard]] MessageWidget *RowMessage(GtkListBoxRow *row) {
  return row ? static_cast<MessageWidget *>(g_object_get_data(G_OBJECT(row), "contrail-message")) : nullptr;
}

// The row at `y` in the list, or the first shown one below when `y` falls on a date header (a row's allocation
// leaves its header out, and gtk_list_box_get_row_at_y finds nothing there) or above the first row. Null below the
// last one.
[[nodiscard]] GtkListBoxRow *RowFrom(GtkWidget *list, double y) {
  if (y >= gtk_widget_get_height(list)) return nullptr;
  if (y > 0) {
    if (auto *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(list), int(y))) return row;
  }
  GtkListBoxRow *row = nullptr;
  for (auto i = 0; (row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), i)); ++i) {
    graphene_rect_t bounds;
    if (gtk_widget_get_visible(GTK_WIDGET(row)) && gtk_widget_compute_bounds(GTK_WIDGET(row), list, &bounds)
        && bounds.origin.y + bounds.size.height > y) {
      return row;
    }
  }
  return nullptr;
}

// A bar under the header, as the phone apps show the pinned message: a tap on it does one thing, the buttons at its
// end others. In a revealer, to slide in and out.
[[nodiscard]] GtkWidget *MakeTopBar(const char *cssClass, GtkWidget *prefix, GtkWidget **title, GtkWidget **text,
                                    std::vector<GtkWidget *> buttons, std::function<void()> tapped) {
  auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(box, "chat-top-bar");
  gtk_widget_add_css_class(box, cssClass);
  if (prefix) {
    gtk_widget_set_valign(prefix, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), prefix);
  }
  auto *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(column, "top-bar-text");
  gtk_widget_set_hexpand(column, TRUE);
  gtk_widget_set_valign(column, GTK_ALIGN_CENTER);
  *title = gtk_label_new(nullptr);
  gtk_widget_add_css_class(*title, "top-bar-title");
  gtk_label_set_xalign(GTK_LABEL(*title), 0);
  gtk_label_set_ellipsize(GTK_LABEL(*title), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(column), *title);
  *text = gtk_label_new(nullptr);
  gtk_label_set_xalign(GTK_LABEL(*text), 0);
  gtk_label_set_ellipsize(GTK_LABEL(*text), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode(GTK_LABEL(*text), TRUE);
  gtk_box_append(GTK_BOX(column), *text);
  gtk_box_append(GTK_BOX(box), column);
  for (auto *button : buttons) {
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), button);
  }
  // The buttons claim their own taps.
  auto *click = gtk_gesture_click_new();
  Connect<void(GtkGestureClick *, int, double, double)>(click, "released",
                                                        [tapped](GtkGestureClick *, int, double, double) { tapped(); });
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));
  auto *revealer = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
  gtk_revealer_set_child(GTK_REVEALER(revealer), box);
  return revealer;
}

// The small picture of a message for the pinned bar: the blurred one it carries, and the file of a size that covers
// the bar's square (or the largest there is). Null where it has none: text, stickers, a hidden spoiler.
struct PinnedPicture {
  const api::minithumbnail *mini = nullptr;
  const api::file *file = nullptr;
};

[[nodiscard]] const api::file *ThumbnailFile(const api::object_ptr<api::thumbnail> &thumbnail) {
  return thumbnail ? thumbnail->file_.get() : nullptr;
}

[[nodiscard]] PinnedPicture PinnedPictureOf(const api::message &message, int pixels) {
  const auto content = message.content_.get();
  if (const auto photo = Cast<api::messagePhoto>(content); photo && photo->photo_) {
    auto result = PinnedPicture{photo->photo_->minithumbnail_.get()};
    if (photo->has_spoiler_) return result;
    const api::photoSize *best = nullptr;
    for (const auto &size : photo->photo_->sizes_) {
      if (!size || !size->photo_) continue;
      const auto covers = std::min(size->width_, size->height_) >= pixels;
      const auto bestCovers = best && std::min(best->width_, best->height_) >= pixels;
      // The smallest that covers it, or the largest of those that don't.
      if (!best || (covers && (!bestCovers || size->width_ < best->width_))
          || (!covers && !bestCovers && size->width_ > best->width_)) {
        best = size.get();
      }
    }
    result.file = best ? best->photo_.get() : nullptr;
    return result;
  }
  if (const auto video = Cast<api::messageVideo>(content); video && video->video_) {
    if (video->has_spoiler_) return {video->video_->minithumbnail_.get()};
    return {video->video_->minithumbnail_.get(), ThumbnailFile(video->video_->thumbnail_)};
  }
  if (const auto animation = Cast<api::messageAnimation>(content); animation && animation->animation_) {
    if (animation->has_spoiler_) return {animation->animation_->minithumbnail_.get()};
    return {animation->animation_->minithumbnail_.get(), ThumbnailFile(animation->animation_->thumbnail_)};
  }
  if (const auto note = Cast<api::messageVideoNote>(content); note && note->video_note_) {
    return {note->video_note_->minithumbnail_.get(), ThumbnailFile(note->video_note_->thumbnail_)};
  }
  if (const auto document = Cast<api::messageDocument>(content); document && document->document_) {
    return {document->document_->minithumbnail_.get(), ThumbnailFile(document->document_->thumbnail_)};
  }
  if (const auto audio = Cast<api::messageAudio>(content); audio && audio->audio_) {
    return {audio->audio_->album_cover_minithumbnail_.get(), ThumbnailFile(audio->audio_->album_cover_thumbnail_)};
  }
  return {};
}

[[nodiscard]] api::object_ptr<api::searchChatMessages> SearchPinned(Td::ChatId chat,
                                                                    api::object_ptr<api::MessageTopic> topic,
                                                                    std::int64_t from) {
  return api::make_object<api::searchChatMessages>(chat, std::move(topic), std::string(), nullptr, from, 0, kPinnedPage,
                                                   api::make_object<api::searchMessagesFilterPinned>());
}

[[nodiscard]] GtkListBoxRow *NextRow(GtkListBoxRow *row) {
  return gtk_list_box_get_row_at_index(GTK_LIST_BOX(gtk_widget_get_parent(GTK_WIDGET(row))),
                                       gtk_list_box_row_get_index(row) + 1);
}

[[nodiscard]] const api::file *LargestPhotoFile(const api::photo *photo) {
  const api::file *result = nullptr;
  auto best = std::int64_t(-1);
  if (!photo) return nullptr;
  for (const auto &size : photo->sizes_) {
    if (!size || !size->photo_) continue;
    if (const auto area = std::int64_t(size->width_) * size->height_; area > best) {
      best = area;
      result = size->photo_.get();
    }
  }
  return result;
}

// Videos, GIFs, stickers and pictures open by themselves once a tap has downloaded them (in 0.3.1 videos and
// pictures did). Other files open with a second tap.
[[nodiscard]] bool OpensByItself(const api::MessageContent *content) {
  if (!content) return false;
  switch (content->get_id()) {
    case api::messageVideo::ID:
    case api::messageAnimation::ID:
    case api::messageVideoNote::ID:
    case api::messageSticker::ID:
    case api::messagePhoto::ID: return true;
    case api::messageDocument::ID: {
      const auto &document = static_cast<const api::messageDocument &>(*content).document_;
      return document && document->mime_type_.starts_with("image/");
    }
    default: return false;
  }
}

// What the menu's Save saves of a message: its file and the name it had, or its photo. Stickers are not saved, as
// in 0.3.1.
struct Saved {
  const api::file *file = nullptr;
  std::string name;
  bool photo = false;
};

[[nodiscard]] Saved SavedOf(const api::message &message) {
  const auto content = message.content_.get();
  if (const auto photo = Cast<api::messagePhoto>(content)) return {LargestPhotoFile(photo->photo_.get()), {}, true};
  if (const auto document = Cast<api::messageDocument>(content); document && document->document_) {
    return {document->document_->document_.get(), document->document_->file_name_};
  }
  if (const auto video = Cast<api::messageVideo>(content); video && video->video_) {
    return {video->video_->video_.get(), video->video_->file_name_};
  }
  if (const auto animation = Cast<api::messageAnimation>(content); animation && animation->animation_) {
    return {animation->animation_->animation_.get(), animation->animation_->file_name_};
  }
  if (const auto audio = Cast<api::messageAudio>(content); audio && audio->audio_) {
    return {audio->audio_->audio_.get(), audio->audio_->file_name_};
  }
  if (const auto voice = Cast<api::messageVoiceNote>(content); voice && voice->voice_note_) {
    return {voice->voice_note_->voice_.get(), {}};
  }
  if (const auto note = Cast<api::messageVideoNote>(content); note && note->video_note_) {
    return {note->video_note_->video_.get(), {}};
  }
  return {};
}

// The files the sponsored message shows: TDLib wants them downloaded before it is shown.
[[nodiscard]] std::vector<const api::file *> SponsoredFiles(const api::MessageContent *content) {
  auto result = std::vector<const api::file *>();
  const auto add = [&](const api::file *file) {
    if (file) result.push_back(file);
  };
  if (const auto photo = Cast<api::messagePhoto>(content)) add(LargestPhotoFile(photo->photo_.get()));
  if (const auto video = Cast<api::messageVideo>(content); video && video->video_) {
    add(video->video_->video_.get());
    if (video->video_->thumbnail_) add(video->video_->thumbnail_->file_.get());
  }
  if (const auto animation = Cast<api::messageAnimation>(content); animation && animation->animation_) {
    add(animation->animation_->animation_.get());
    if (animation->animation_->thumbnail_) add(animation->animation_->thumbnail_->file_.get());
  }
  return result;
}

[[nodiscard]] std::string UserDirectory(GUserDirectory kind, const char *fallback) {
  const auto special = g_get_user_special_dir(kind);
  auto *path = special ? g_strdup(special) : g_build_filename(g_get_home_dir(), fallback, nullptr);
  auto result = std::string(path);
  g_free(path);
  g_mkdir_with_parents(result.c_str(), 0700);
  return result;
}

// "name.ext", or "name (1).ext" and on when that is taken, as 0.3.1 named them.
[[nodiscard]] std::string UniquePath(const std::string &directory, const std::string &name) {
  const auto dot = name.rfind('.');
  const auto stem = (dot == std::string::npos || dot == 0) ? name : name.substr(0, dot);
  const auto extension = (dot == std::string::npos || dot == 0) ? std::string() : name.substr(dot);
  for (auto index = 0;; ++index) {
    const auto candidate = index ? stem + " (" + std::to_string(index) + ")" + extension : name;
    auto *path = g_build_filename(directory.c_str(), candidate.c_str(), nullptr);
    auto result = std::string(path);
    g_free(path);
    if (!g_file_test(result.c_str(), G_FILE_TEST_EXISTS)) return result;
  }
}

// The downloaded file is copied (without blocking the chat: it may be a large video), then the toast says where.
void CopySaved(const std::string &path, const std::string &name, bool photo) {
  auto target = std::string();
  if (photo) {
    auto *now = g_date_time_new_now_local();
    auto *stamp = g_date_time_format(now, "photo_%Y-%m-%d_%H-%M-%S.jpg");
    target = UniquePath(UserDirectory(G_USER_DIRECTORY_PICTURES, "Pictures"), stamp);
    g_free(stamp);
    g_date_time_unref(now);
  } else {
    auto *base = g_path_get_basename(path.c_str());
    target = UniquePath(UserDirectory(G_USER_DIRECTORY_DOWNLOAD, "Downloads"), name.empty() ? base : name);
    g_free(base);
  }
  auto *source = g_file_new_for_path(path.c_str());
  auto *destination = g_file_new_for_path(target.c_str());
  auto *toast = new std::string();
  if (photo) {
    auto *base = g_path_get_basename(target.c_str());
    *toast = base;
    g_free(base);
  } else {
    *toast = Tr("lng_mediaview_saved_to", {{"downloads", Tr("lng_downloads_section")}});
  }
  g_file_copy_async(source, destination, G_FILE_COPY_NONE, G_PRIORITY_DEFAULT, nullptr, nullptr, nullptr,
                    +[](GObject *object, GAsyncResult *result, gpointer data) {
                      auto *toast = static_cast<std::string *>(data);
                      GError *error = nullptr;
                      if (g_file_copy_finish(G_FILE(object), result, &error)) {
                        ShowToast(*toast);
                      } else {
                        ShowToast(error ? error->message : "?");
                        g_clear_error(&error);
                      }
                      delete toast;
                    },
                    toast);
  g_object_unref(source);
  g_object_unref(destination);
}

// The items of a menu model, sections included, for the debug state.
void DescribeMenu(GMenuModel *model, std::string &out) {
  for (auto i = 0; i < g_menu_model_get_n_items(model); ++i) {
    char *label = nullptr, *action = nullptr;
    if (g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_LABEL, "s", &label)) {
      g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_ACTION, "s", &action);
      out += std::string(" [") + label + "=" + (action ? action : "") + "]";
    }
    g_free(label);
    g_free(action);
    if (auto *section = g_menu_model_get_item_link(model, i, G_MENU_LINK_SECTION)) {
      DescribeMenu(section, out);
      g_object_unref(section);
    }
  }
}

// Saves waiting for their downloads. They outlive the chat that asked, as in 0.3.1: the download goes on and the
// file is saved when it ends, wherever the window is.
struct PendingSave {
  std::string name;  // Empty: the downloaded file's.
  bool photo = false;
};
struct PendingSaves {
  Td::Lifetime lifetime;
  std::unordered_map<Td::FileId, PendingSave> files;
};
PendingSaves &Saves() {
  static auto saves = PendingSaves();
  return saves;
}

void SaveWhenLoaded(Td::Store &store, Td::FileId file, std::string name, bool photo) {
  auto &saves = Saves();
  if (saves.files.empty()) {
    // The first one since the last ended: the session's store is the one to listen to.
    saves.lifetime.reset();
    store.onFile([](const Td::File &file) {
      auto &saves = Saves();
      const auto i = saves.files.find(file.id);
      if (i == saves.files.end() || !file.complete) return;
      const auto save = std::move(i->second);
      saves.files.erase(i);
      CopySaved(file.path, save.name, save.photo);
    }, saves.lifetime);
  }
  saves.files[file] = {std::move(name), photo};
}

}  // namespace

void ChatView::Timer::callOnce(guint ms) {
  cancel();
  _source = g_timeout_add(ms, +[](gpointer data) -> gboolean {
    auto *self = static_cast<Timer *>(data);
    self->_source = 0;
    // The callback may close the chat, and this timer with it.
    auto callback = self->_callback;
    callback();
    return G_SOURCE_REMOVE;
  }, this);
}

void ChatView::Timer::cancel() {
  if (_source) g_source_remove(std::exchange(_source, 0));
}

ChatView::ChatView(Td::Store &store, Td::ChatId chat, MainWindow &window, std::int64_t showAt, std::int32_t topic)
    : _store(store), _chat(chat), _topic(topic), _window(window), _pendingShowAt(showAt),
      _readTimer([this] { markAsRead(); }),
      _statusTimer([this] {
        refreshHeader();
        _statusTimer.callOnce(kStatusPeriod);
      }),
      _reserveTimer([this] { maintainTopReserve(); }), _jumpTimer([this] { scheduleViewportUpdate(); }),
      _draftTimer([this] { saveDraft(); }), _pinnedTimer([this] { loadPinned(); }) {
  _root = adw_toolbar_view_new();
  g_object_ref_sink(_root);
  adw_toolbar_view_set_bottom_bar_style(ADW_TOOLBAR_VIEW(_root), ADW_TOOLBAR_RAISED);
  buildHeader();
  buildPinnedBar();
  if (const auto known = _store.chat(_chat); known && known->kind != Td::ChatKind::Channel) {
    _liveBar = std::make_unique<ChatLiveBar>(_window, _store, _chat);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(_root), _liveBar->widget());
  }
  buildList();
  buildComposer();
  refreshHeader();
  refreshComposerState();
  refreshForwardBar();
  _statusTimer.callOnce(kStatusPeriod);
  _signals.connect<void(GtkWidget *)>(_root, "map", [this](GtkWidget *) { scheduleViewportUpdate(); });
  _signals.connect<void(GObject *, GParamSpec *)>(_window.widget(), "notify::is-active",
                                                  [this](GObject *, GParamSpec *) { _readTimer.callOnce(300); });
  subscribe();
  loadPinned();

  // TDLib keeps a chat's messages up to date while it is open, and needs that to count views.
  _store.client().send(api::make_object<api::openChat>(_chat));
  // Its slow mode comes with the full info (updateSupergroupFullInfo), which TDLib then keeps up to date.
  if (const auto known = _store.chat(_chat); known && known->supergroup && known->kind == Td::ChatKind::Group) {
    const auto supergroup = known->peerId;
    _store.client().request<api::supergroupFullInfo>(
        api::make_object<api::getSupergroupFullInfo>(supergroup), _lifetime,
        [this, supergroup](api::supergroupFullInfo &full) { _store.setSupergroupFullInfo(supergroup, full); },
        [](api::error &) {});
  }
  if (!_topic) {
    start();
    return;
  }
  _topicHold = _store.holdTopics(_chat);
  _topicKnown = _store.topic(_chat, _topic) != nullptr;
  if (_topicKnown) {
    start();
    return;
  }
  // Opened from a link or a message: where it was read up to comes first, the messages from there.
  const auto token = _lifetime.token();
  _store.refreshTopic(_chat, _topic, [this, token] {
    if (token.expired()) return;
    _topicKnown = _store.topic(_chat, _topic) != nullptr;
    refreshHeader();
    refreshComposerState();
    start();
  });
}

void ChatView::start() {
  if (const auto known = readState()) {
    if (known->unread > 0 && known->readInbox) _unreadAfter = known->readInbox;
    _savedDraft = known->draft;
    _savedDraftReply = known->draftReplyTo;
    if (!known->draft.empty()) setComposerText(known->draft);
    if (const auto reply = known->draftReplyTo) {
      _store.client().request<api::message>(
          api::make_object<api::getMessage>(_chat, reply), _lifetime,
          [this](api::message &message) {
            auto item = MessageItem();
            item.data = api::make_object<api::message>();
            item.data->id_ = message.id_;
            item.data->sender_id_ = std::move(message.sender_id_);
            item.data->content_ = std::move(message.content_);
            // Messages waiting to be forwarded take the reply's place.
            if (!_editing && !forwarding()) replyTo(item);
          },
          [](api::error &) {});
    }
  }
  if (!_pendingShowAt) _pendingShowAt = _unreadAfter;
  _stickToBottom = !_pendingShowAt;
  loadInitial();
}

ChatView::~ChatView() {
  // Files prepared for this chat never go to another one.
  _pasteReader = nullptr;
  if (const auto attach = _attach.lock()) attach->close();
  // A menu open on a message goes before the rows do (GTK warns of a row finalized with it).
  dropMessageMenu(nullptr);
  if (_pinnedDialog) adw_dialog_force_close(ADW_DIALOG(_pinnedDialog));
  for (auto *dialog : std::exchange(_dialogs, {})) adw_dialog_force_close(ADW_DIALOG(dialog));
  _signals.clear();
  if (_viewportSource) g_source_remove(_viewportSource);
  if (_closeSource) g_source_remove(_closeSource);
  saveDraft();
  _store.client().send(api::make_object<api::closeChat>(_chat));
  _rows.clear();
  _itemRows.clear();
  _items.clear();
  g_object_unref(_root);
}

void ChatView::subscribe() {
  _store.client().subscribe([this](api::Update &update) { this->updated(update); }, _lifetime);
  _store.onChat([this](Td::ChatId id) {
    if (id != _chat) return;
    refreshHeader();
    refreshComposerState();
    refreshFooters();
    draftChanged();
  }, _lifetime);
  _store.onTopic([this](Td::ChatId chat, std::int32_t id) {
    if (_topic && chat == _chat && id == _topic) topicChanged();
  }, _lifetime);
  _store.onUser([this](Td::UserId id) {
    const auto known = _store.chat(_chat);
    if (known && known->kind == Td::ChatKind::Private && known->peerId == id) refreshHeader();
  }, _lifetime);
  _store.onFile([this](const Td::File &file) {
    if (_pinnedFile && file.id == _pinnedFile && file.complete) {
      _pinnedFile = 0;
      showPinnedPicture(file.path);
    }
    if (const auto i = _openWhenLoaded.find(file.id); i != _openWhenLoaded.end()) {
      if (file.complete && !file.path.empty()) {
        _openWhenLoaded.erase(i);
        OpenPath(file.path);
      } else if (!i->second) {
        // It wasn't here: a second tap opens it.
        _openWhenLoaded.erase(i);
      }
    }
    if (_sponsoredWaiting.contains(file.id) && file.complete) {
      _sponsoredWaiting.erase(file.id);
      if (_sponsoredWaiting.empty()) placeSponsored();
    }
    for (const auto &[raw, row] : _rows) row->fileChanged(file);
  }, _lifetime);
}

const Td::Chat *ChatView::readState() const {
  if (!_topic) return _store.chat(_chat);
  const auto known = _store.topic(_chat, _topic);
  return known ? &known->row : nullptr;
}

bool ChatView::belongs(const api::message &message) const {
  if (message.chat_id_ != _chat) return false;
  if (!_topic) return true;
  const auto topic = Td::MessageTopic(message);
  if (topic == _topic) return true;
  // A message without a topic in a group with topics is one of General's.
  const auto known = topic ? nullptr : _store.topic(_chat, _topic);
  return known && known->general;
}

api::object_ptr<api::MessageTopic> ChatView::topicObject() const {
  if (!_topic) return nullptr;
  return api::make_object<api::messageTopicForum>(_topic);
}

void ChatView::topicChanged() {
  if (_store.topic(_chat, _topic)) {
    _topicKnown = true;
    refreshHeader();
    refreshComposerState();
    draftChanged();
    return;
  }
  // The topic was deleted: nothing left to show. Not now: this runs inside the store's notification.
  if (!_topicKnown || _closeSource) return;
  _closeSource = g_idle_add_once(+[](gpointer data) {
    auto *self = static_cast<ChatView *>(data);
    self->_closeSource = 0;
    if (self->_window.chatView() == self) self->_window.closeChat();
  }, this);
}

void ChatView::updated(api::Update &update) {
  const auto refresh = [&](std::int64_t chat, std::int64_t id, auto change) {
    if (chat != _chat) return;
    if (const auto item = find(id)) {
      change(*item->data);
      refreshItem(item);
    }
  };
  switch (update.get_id()) {
    case api::updateNewMessage::ID: {
      auto &data = static_cast<api::updateNewMessage &>(update);
      if (!belongs(*data.message_) || data.message_->scheduling_state_) break;
      // Further back in the chat the newest messages aren't held: they come when the list gets there.
      if (!_reachedBottom && !data.message_->sending_state_) break;
      if (insert(std::move(data.message_))) {
        sync();
        if (_stickToBottom) _readTimer.callOnce(300);
      }
      break;
    }
    case api::updateMessageSendSucceeded::ID: {
      auto &data = static_cast<api::updateMessageSendSucceeded &>(update);
      messageSent(std::move(data.message_), data.old_message_id_);
      break;
    }
    case api::updateMessageSendFailed::ID: {
      auto &data = static_cast<api::updateMessageSendFailed &>(update);
      messageSent(std::move(data.message_), data.old_message_id_);
      break;
    }
    case api::updateMessageContent::ID: {
      auto &data = static_cast<api::updateMessageContent &>(update);
      refresh(data.chat_id_, data.message_id_,
              [&](api::message &message) { message.content_ = std::move(data.new_content_); });
      if (data.chat_id_ == _chat && data.message_id_ == _pinnedCurrent) fetchPinnedMessage();
      break;
    }
    case api::updateMessageEdited::ID: {
      auto &data = static_cast<api::updateMessageEdited &>(update);
      refresh(data.chat_id_, data.message_id_, [&](api::message &message) {
        message.edit_date_ = data.edit_date_;
        message.reply_markup_ = std::move(data.reply_markup_);
      });
      break;
    }
    case api::updateMessageInteractionInfo::ID: {
      // Views and reactions change all the time in a busy channel: only the footer and the chips change, never the
      // whole row (building rows again for them once froze scrolling there).
      auto &data = static_cast<api::updateMessageInteractionInfo &>(update);
      if (data.chat_id_ != _chat) break;
      if (const auto item = find(data.message_id_)) {
        item->data->interaction_info_ = std::move(data.interaction_info_);
        ++_interactionUpdates;
        if (const auto row = rowFor(item)) {
          row->refreshFooter();
          row->refreshReactions();
        }
      }
      break;
    }
    case api::updateMessageUnreadReactions::ID: {
      // A new reaction to a message already seen: it is read once it is on the screen again (viewMessages).
      auto &data = static_cast<api::updateMessageUnreadReactions &>(update);
      if (data.chat_id_ != _chat) break;
      if (const auto item = find(data.message_id_)) {
        item->data->unread_reactions_ = std::move(data.unread_reactions_);
        if (!item->data->unread_reactions_.empty() && _viewed.erase(item->id()) && !_readTimer.active()) {
          _readTimer.callOnce(300);
        }
      }
      break;
    }
    case api::updateMessageIsPinned::ID: {
      auto &data = static_cast<api::updateMessageIsPinned &>(update);
      if (data.chat_id_ != _chat) break;
      if (const auto item = find(data.message_id_)) item->data->is_pinned_ = data.is_pinned_;
      pinnedChanged(data.message_id_, data.is_pinned_);
      break;
    }
    case api::updateDeleteMessages::ID: {
      auto &data = static_cast<api::updateDeleteMessages &>(update);
      // Not permanent: TDLib only dropped them from its memory, or they were being sent.
      if (data.chat_id_ == _chat && data.is_permanent_) {
        remove(data.message_ids_);
        for (const auto id : data.message_ids_) pinnedChanged(id, false);
      }
      break;
    }
    default: break;
  }
}

// The message being sent has its server id now, or failed: its object changes, its row stays.
void ChatView::messageSent(api::object_ptr<api::message> message, std::int64_t oldId) {
  if (!message || !belongs(*message)) return;
  if (const auto item = find(oldId)) {
    item->data = std::move(message);
    resort();
    refreshItem(item);
    sync();
  } else if (_reachedBottom) {
    insert(std::move(message));
    sync();
  }
}

// --- header ------------------------------------------------------------------

void ChatView::buildHeader() {
  auto *header = adw_header_bar_new();
  _title = adw_window_title_new("", "");
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), _title);
  auto *avatar = MakeAvatar(_store, _chat, 32, true);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), avatar);
  // Section calls: only a person can receive a 1:1 call.
  const auto known = _store.chat(_chat);
  const auto user = known ? _store.user(known->peerId) : nullptr;
  if (known && known->kind == Td::ChatKind::Private && known->peerId != _store.selfId()
      && (!user || (!user->bot && !user->deleted))) {
    auto *call = MakeIconButton("call-start-symbolic", Tr("lng_call_start").c_str(), "flat");
    _signals.clicked(call, [this, id = known->peerId] { _window.startCall(id); });
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), call);
  }
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(_root), header);
  // A tap on the name or the picture: the chat's card.
  for (auto *widget : {_title, avatar}) {
    auto *click = gtk_gesture_click_new();
    _signals.connect<void(GtkGestureClick *, int, double, double)>(
        click, "released", [this](GtkGestureClick *, int, double, double) { _window.openProfile(_chat); });
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(click));
    gtk_widget_set_cursor_from_name(widget, "pointer");
  }
}

void ChatView::refreshHeader() {
  const auto known = _store.chat(_chat);
  if (!known) return;
  auto name = known->title;
  auto status = std::string();
  if (_topic) {
    // The topic, and the group it is in.
    if (const auto topic = _store.topic(_chat, _topic)) name = topic->row.title;
    status = known->title;
  } else if (known->kind == Td::ChatKind::Private || known->kind == Td::ChatKind::Secret) {
    if (known->peerId == _store.selfId()) {
      name = Tr("lng_saved_messages");
    } else if (const auto user = _store.user(known->peerId)) {
      status = user->bot ? Tr("lng_status_bot") : OnlineText(*user);
    }
  } else if (const auto group = _store.group(*known); group && group->members > 0) {
    status = TrCount(known->kind == Td::ChatKind::Channel ? "lng_chat_status_subscribers" : "lng_chat_status_members",
                     group->members);
  }
  // "typing...", "sending a photo..." while someone does.
  if (const auto action = Td::TypingText(*known, _store); !action.empty()) status = action;
  adw_window_title_set_title(ADW_WINDOW_TITLE(_title), name.c_str());
  adw_window_title_set_subtitle(ADW_WINDOW_TITLE(_title), status.c_str());
}

// --- pinned messages -------------------------------------------------------------

void ChatView::buildPinnedBar() {
  // A GtkImage keeps its size whatever the picture: a GtkPicture's height follows its width, which the bar's layout
  // must not depend on.
  _pinnedPreview = gtk_image_new();
  gtk_image_set_pixel_size(GTK_IMAGE(_pinnedPreview), kPinnedPreviewSize);
  gtk_widget_set_size_request(_pinnedPreview, kPinnedPreviewSize, kPinnedPreviewSize);
  gtk_widget_add_css_class(_pinnedPreview, "pinned-preview");
  gtk_widget_set_overflow(_pinnedPreview, GTK_OVERFLOW_HIDDEN);
  gtk_widget_set_visible(_pinnedPreview, FALSE);
  _pinnedListButton = MakeIconButton(
      "view-list-bullet-symbolic",
      Translated("All Pinned Messages", "Все закреплённые сообщения", "Усі закріплені повідомлення").c_str(), "flat");
  gtk_widget_add_css_class(_pinnedListButton, "circular");
  _signals.clicked(_pinnedListButton, [this] { showPinnedList(); });
  _pinnedClose = MakeIconButton("window-close-symbolic", Tr("lng_pinned_hide_all_hide").c_str(), "flat");
  gtk_widget_add_css_class(_pinnedClose, "circular");
  _signals.clicked(_pinnedClose, [this] { pinnedCloseClicked(); });
  const auto token = _lifetime.token();
  _pinnedBar = MakeTopBar("pinned-bar", _pinnedPreview, &_pinnedTitle, &_pinnedText, {_pinnedListButton, _pinnedClose},
                          [this, token] {
                            if (!token.expired()) pinnedBarClicked();
                          });
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(_root), _pinnedBar);
}

// All the ids, a page after another: a chat rarely has more than a few, and the bar counts and walks them.
void ChatView::loadPinned() {
  _pinnedLifetime.reset();
  _pinnedTimer.cancel();
  const auto first = std::make_shared<bool>(true);
  // The request waiting for its answer holds the loop; the loop itself holds nothing.
  const auto next = std::make_shared<std::function<void(std::int64_t)>>();
  *next = [this, first, weak = std::weak_ptr(next)](std::int64_t from) {
    const auto strong = weak.lock();
    if (!strong) return;
    _store.client().request<api::foundChatMessages>(
        SearchPinned(_chat, topicObject(), from), _pinnedLifetime,
        [this, first, strong](api::foundChatMessages &found) {
          pinnedPage(found, std::exchange(*first, false));
          if (found.next_from_message_id_ && !found.messages_.empty()
              && int(_pinnedIds.size()) < found.total_count_) {
            (*strong)(found.next_from_message_id_);
          }
        },
        [](api::error &) {});
  };
  (*next)(0);
}

void ChatView::pinnedPage(api::foundChatMessages &found, bool first) {
  if (first) _pinnedIds.clear();
  for (const auto &message : found.messages_) {
    if (message) _pinnedIds.push_back(message->id_);
  }
  std::ranges::sort(_pinnedIds);
  const auto [end, last] = std::ranges::unique(_pinnedIds);
  _pinnedIds.erase(end, last);
  _pinnedTotal = std::max(found.total_count_, int(_pinnedIds.size()));
  _pinnedLoaded = true;
  updatePinnedViewer();
  refreshPinnedBar();
}

void ChatView::pinnedChanged(std::int64_t id, bool pinned) {
  if (!_pinnedLoaded) return;
  if (_topic) {
    // Which topic the message is in isn't told: the topic's pinned ones are asked for again, once for many changes.
    if (pinned || std::ranges::binary_search(_pinnedIds, id)) _pinnedTimer.callOnce(300);
    return;
  }
  const auto i = std::ranges::lower_bound(_pinnedIds, id);
  const auto has = i != _pinnedIds.end() && *i == id;
  if (pinned == has) return;
  if (pinned) {
    _pinnedIds.insert(i, id);
    ++_pinnedTotal;
  } else {
    _pinnedIds.erase(i);
    _pinnedTotal = std::max(_pinnedTotal - 1, int(_pinnedIds.size()));
    if (_pinnedClicked == id) _pinnedClicked = 0;
  }
  updatePinnedViewer();
  refreshPinnedBar();
}

// The bar shows the last message pinned above the bottom of the screen (HistoryWidget::updatePinnedViewer).
void ChatView::updatePinnedViewer() {
  auto lessThan = kNoMessage;
  if (_initialLoaded && !_stickToBottom && !atBottom()) {
    auto bottom = std::int64_t(0);
    const auto page = gtk_adjustment_get_page_size(_adjustment);
    for (const auto row : visibleRows()) {
      for (const auto item : row->items()) {
        if (item->local() || item->sponsored || item->id() <= bottom) continue;
        auto *widget = row->widgetFor(item);
        double y = 0;
        const auto height = widget ? gtk_widget_get_height(widget) : 0;
        if (height > 0 && WidgetViewportY(widget, _scroll, y) && y + height > 0 && y < page) bottom = item->id();
      }
    }
    // None on the screen: it is in the room above the rows, older ones are on the way. The bar stays.
    lessThan = bottom ? bottom + 1 : _pinnedAround;
  }
  if (_pinnedClicked && !_scrollTarget && lessThan <= _pinnedClicked) {
    // Scrolled above the message shown with the bar.
    _pinnedClicked = 0;
  }
  if (_pinnedClicked && !_pinnedIds.empty() && _pinnedIds.front() >= _pinnedClicked) {
    // The oldest one was shown: round to the newest again.
    setPinnedAround(kNoMessage);
  } else {
    setPinnedAround(_pinnedClicked ? std::min(lessThan, _pinnedClicked) : lessThan);
  }
}

void ChatView::setPinnedAround(std::int64_t around) {
  _pinnedAround = around;
  auto current = std::int64_t(0);
  if (!_pinnedIds.empty()) {
    const auto i = std::ranges::lower_bound(_pinnedIds, around);
    current = (i != _pinnedIds.begin()) ? *(i - 1) : _pinnedIds.front();
  }
  if (current == _pinnedCurrent) return;
  _pinnedCurrent = current;
  if (!current) {
    _pinnedMessage = nullptr;
    setPinnedPreview();
    refreshPinnedBar();
    return;
  }
  // The bar keeps showing the one before until this one comes: no blink on a tap.
  fetchPinnedMessage();
}

void ChatView::fetchPinnedMessage() {
  const auto id = _pinnedCurrent;
  _store.client().send(
      api::make_object<api::getMessage>(_chat, id),
      [this, id](Td::ObjectPtr object) {
        if (id != _pinnedCurrent) return;
        if (!object || object->get_id() != api::message::ID) {
          // Gone, or not available any more: the bar goes on to another one.
          pinnedChanged(id, false);
          return;
        }
        _pinnedMessage = api::move_object_as<api::message>(object);
        setPinnedPreview();
        refreshPinnedBar();
      },
      _lifetime);
}

int ChatView::pinnedIndex() const {
  if (!_pinnedMessage) return 0;
  const auto i = std::ranges::lower_bound(_pinnedIds, _pinnedMessage->id_);
  return int(i - _pinnedIds.begin());
}

void ChatView::refreshPinnedBar() {
  const auto shown = _pinnedMessage && _pinnedCurrent && !pinnedHiddenByUser();
  gtk_revealer_set_reveal_child(GTK_REVEALER(_pinnedBar), shown);
  if (!shown) return;
  // As Telegram Desktop titles them: the newest, the other of two, or the number of an older one.
  const auto count = std::max(_pinnedTotal, 1);
  const auto index = std::clamp(pinnedIndex(), 0, count - 1);
  const auto title = (index + 1 >= count) ? Tr("lng_pinned_message")
                     : (count == 2)       ? Tr("lng_pinned_previous")
                                          : Tr("lng_pinned_message") + " #" + std::to_string(index + 1);
  gtk_label_set_text(GTK_LABEL(_pinnedTitle), title.c_str());
  gtk_label_set_text(GTK_LABEL(_pinnedText), Td::OneLine(Td::MessagePreview(*_pinnedMessage, _store)).c_str());
  // Several pinned: the list of them; one: unpin it (or hide the bar, without the right to unpin).
  gtk_widget_set_visible(_pinnedListButton, _pinnedTotal > 1);
  gtk_widget_set_visible(_pinnedClose, _pinnedTotal <= 1);
}

void ChatView::setPinnedPreview() {
  _pinnedFile = 0;
  gtk_image_clear(GTK_IMAGE(_pinnedPreview));
  gtk_widget_set_visible(_pinnedPreview, FALSE);
  if (!_pinnedMessage) return;
  const auto pixels = kPinnedPreviewSize * gtk_widget_get_scale_factor(_pinnedBar);
  const auto picture = PinnedPictureOf(*_pinnedMessage, pixels);
  if (const auto mini = picture.mini; mini && !mini->data_.empty()) {
    // A JPEG of a few hundred bytes, shown at once while the file comes.
    auto *bytes = g_bytes_new(mini->data_.data(), mini->data_.size());
    if (auto *texture = gdk_texture_new_from_bytes(bytes, nullptr)) {
      gtk_image_set_from_paintable(GTK_IMAGE(_pinnedPreview), GDK_PAINTABLE(texture));
      g_object_unref(texture);
      gtk_widget_set_visible(_pinnedPreview, TRUE);
    }
    g_bytes_unref(bytes);
  }
  const auto file = picture.file;
  if (!file) return;
  if (file->local_ && file->local_->is_downloading_completed_ && !file->local_->path_.empty()) {
    showPinnedPicture(file->local_->path_);
  } else if (const auto known = _store.file(file->id_); known && known->complete && !known->path.empty()) {
    showPinnedPicture(known->path);
  } else {
    _pinnedFile = file->id_;
    _store.download(file->id_, 16);
  }
}

// Decoded right here at the bar's size (a thumbnail of a few KB) and cut to its square: a few KB of texture.
void ChatView::showPinnedPicture(const std::string &path) {
  const auto pixels = kPinnedPreviewSize * gtk_widget_get_scale_factor(_pinnedBar);
  const auto options = DecodeOptions{.pixbufFallback = false, .waitForLarge = false};
  auto *decoded = DecodePictureFile(path.c_str(), pixels, pixels, options);
  if (!decoded) return;
  const auto width = gdk_pixbuf_get_width(decoded), height = gdk_pixbuf_get_height(decoded);
  const auto side = std::min(width, height);
  auto *square = gdk_pixbuf_new_subpixbuf(decoded, (width - side) / 2, (height - side) / 2, side, side);
  auto *scaled = gdk_pixbuf_scale_simple(square, pixels, pixels, GDK_INTERP_BILINEAR);
  g_object_unref(square);
  g_object_unref(decoded);
  if (!scaled) return;
  auto *texture = TextureFromPixbuf(scaled);
  gtk_image_set_from_paintable(GTK_IMAGE(_pinnedPreview), GDK_PAINTABLE(texture));
  g_object_unref(texture);
  gtk_widget_set_visible(_pinnedPreview, TRUE);
}

void ChatView::pinnedBarClicked() {
  const auto id = _pinnedCurrent;
  if (!id) return;
  _pinnedClicked = id;
  // A pinned message of an album: the album is shown, the bar goes on before all of it.
  if (const auto item = find(id); item && item->data->media_album_id_) {
    for (const auto &other : _items) {
      if (!other->local() && other->data->media_album_id_ == item->data->media_album_id_) {
        _pinnedClicked = std::min(_pinnedClicked, other->id());
      }
    }
  }
  showAt(id);
  updatePinnedViewer();
}

void ChatView::pinnedCloseClicked() {
  const auto id = _pinnedCurrent;
  if (!id) return;
  _store.client().request<api::messageProperties>(
      api::make_object<api::getMessageProperties>(_chat, id), _lifetime,
      [this, id](api::messageProperties &properties) {
        if (properties.can_be_pinned_) confirmUnpin(id);
        else confirmHidePinned();
      },
      [this](api::error &) { confirmHidePinned(); });
}

bool ChatView::canPinMessages() const {
  const auto known = _store.chat(_chat);
  return known && _store.canPin(*known);
}

bool ChatView::pinnedHiddenByUser() const {
  if (canPinMessages() || _pinnedIds.empty()) return false;
  return HiddenPinnedMessage(_chat, _topic) == _pinnedIds.back();
}

void ChatView::confirmPin(std::int64_t id) {
  const auto known = _store.chat(_chat);
  if (!known) return;
  const auto old = !_pinnedIds.empty() && id < _pinnedIds.back();
  const auto group = known->kind == Td::ChatKind::Group;
  auto *dialog = adw_alert_dialog_new(
      Tr(old ? "lng_pinned_pin_old_sure" : group ? "lng_pinned_pin_sure_group" : "lng_pinned_pin_sure").c_str(),
      nullptr);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", Tr("lng_cancel").c_str(), "pin",
                                 Tr("lng_pinned_pin").c_str(), nullptr);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "pin", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "pin");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  // As PinMessageBox: for the other person too in a chat with a user, the members notified in a group.
  GtkWidget *forPeer = nullptr;
  GtkWidget *notify = nullptr;
  if (known->kind == Td::ChatKind::Private && known->peerId != _store.selfId()) {
    const auto user = _store.user(known->peerId);
    forPeer = gtk_check_button_new_with_label(
        Tr("lng_pinned_also_for_other", {{"user", user ? user->firstName : known->title}}).c_str());
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), forPeer);
  } else if (!old && group) {
    notify = gtk_check_button_new_with_label(Tr("lng_pinned_notify").c_str());
    gtk_check_button_set_active(GTK_CHECK_BUTTON(notify), TRUE);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), notify);
  }
  // The answer may come after the chat is closed.
  auto &client = _store.client();
  const auto chat = _chat;
  Connect<void(AdwAlertDialog *, const char *)>(dialog, "response", [=, &client](AdwAlertDialog *, const char *response) {
    if (g_strcmp0(response, "pin") != 0) return;
    const auto silent = notify && !gtk_check_button_get_active(GTK_CHECK_BUTTON(notify));
    const auto onlyForSelf = forPeer && !gtk_check_button_get_active(GTK_CHECK_BUTTON(forPeer));
    client.send(api::make_object<api::pinChatMessage>(chat, id, silent, onlyForSelf), [](Td::ObjectPtr object) {
      if (object && object->get_id() == api::error::ID) ShowToast(static_cast<api::error &>(*object).message_);
    });
  });
  adw_dialog_present(ADW_DIALOG(dialog), _window.widget());
}

void ChatView::confirmUnpin(std::int64_t id) {
  auto *dialog = adw_alert_dialog_new(Tr("lng_pinned_unpin_sure").c_str(), nullptr);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", Tr("lng_cancel").c_str(), "unpin",
                                 Tr("lng_pinned_unpin").c_str(), nullptr);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "unpin", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  auto &client = _store.client();
  const auto chat = _chat;
  Connect<void(AdwAlertDialog *, const char *)>(dialog, "response", [=, &client](AdwAlertDialog *, const char *response) {
    if (g_strcmp0(response, "unpin") != 0) return;
    client.send(api::make_object<api::unpinChatMessage>(chat, id), [](Td::ObjectPtr object) {
      if (object && object->get_id() == api::error::ID) ShowToast(static_cast<api::error &>(*object).message_);
    });
  });
  adw_dialog_present(ADW_DIALOG(dialog), _window.widget());
}

void ChatView::confirmUnpinAll() {
  auto *dialog = adw_alert_dialog_new(Tr("lng_pinned_unpin_all_sure").c_str(), nullptr);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", Tr("lng_cancel").c_str(), "unpin",
                                 Tr("lng_pinned_unpin").c_str(), nullptr);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "unpin", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  auto &client = _store.client();
  const auto chat = _chat;
  const auto topic = _topic;
  Connect<void(AdwAlertDialog *, const char *)>(dialog, "response", [=, &client](AdwAlertDialog *, const char *response) {
    if (g_strcmp0(response, "unpin") != 0) return;
    auto function = topic ? Td::FunctionPtr(api::make_object<api::unpinAllForumTopicMessages>(chat, topic))
                          : Td::FunctionPtr(api::make_object<api::unpinAllChatMessages>(chat));
    client.send(std::move(function), [](Td::ObjectPtr object) {
      if (object && object->get_id() == api::error::ID) ShowToast(static_cast<api::error &>(*object).message_);
    });
  });
  adw_dialog_present(ADW_DIALOG(dialog), _window.widget());
}

// Without the right to unpin: the bar goes away until something new is pinned (HidePinnedBar).
void ChatView::confirmHidePinned() {
  auto *dialog = adw_alert_dialog_new(nullptr, Tr("lng_pinned_hide_all_sure").c_str());
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", Tr("lng_cancel").c_str(), "hide",
                                 Tr("lng_pinned_hide_all_hide").c_str(), nullptr);
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  const auto token = _lifetime.token();
  const auto chat = _chat;
  const auto topic = _topic;
  Connect<void(AdwAlertDialog *, const char *)>(dialog, "response", [=, this](AdwAlertDialog *, const char *response) {
    if (token.expired() || g_strcmp0(response, "hide") != 0 || _pinnedIds.empty()) return;
    SetHiddenPinnedMessage(chat, topic, _pinnedIds.back());
    refreshPinnedBar();
  });
  adw_dialog_present(ADW_DIALOG(dialog), _window.widget());
}

// All the pinned messages of the chat, newest first; a tap shows one.
void ChatView::showPinnedList() {
  if (_pinnedDialog) return;
  auto *dialog = adw_dialog_new();
  _pinnedDialog = GTK_WIDGET(dialog);
  adw_dialog_set_content_width(ADW_DIALOG(dialog), 480);
  adw_dialog_set_content_height(ADW_DIALOG(dialog), 900);
  auto *view = adw_toolbar_view_new();
  auto *header = adw_header_bar_new();
  auto *title = adw_window_title_new(Tr("lng_pinned_message").c_str(), nullptr);
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
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
  gtk_widget_set_vexpand(scroll, TRUE);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroll);
  // Unpin them all, or (without the right to) hide the bar.
  const auto canPin = canPinMessages();
  auto *action = gtk_button_new_with_label(
      (canPin ? TrCount("lng_pinned_unpin_all", std::max(_pinnedTotal, 1)) : Tr("lng_pinned_hide_all")).c_str());
  gtk_widget_add_css_class(action, "pill");
  gtk_widget_add_css_class(action, canPin ? "destructive-action" : "flat");
  gtk_widget_set_halign(action, GTK_ALIGN_CENTER);
  SetMarginAll(action, 12);
  adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(view), action);
  adw_dialog_set_child(ADW_DIALOG(dialog), view);
  g_object_set_data(G_OBJECT(dialog), "contrail-list", list);
  g_object_set_data(G_OBJECT(dialog), "contrail-action", action);
  g_object_set_data(G_OBJECT(dialog), "contrail-title", title);

  // The requests end with the dialog.
  struct State {
    Td::Lifetime lifetime;
    std::int64_t next = 0;  // Where the next page starts, 0 when there is none.
    bool loading = false;
  };
  auto *state = AttachData(dialog, "contrail-pinned-list", std::make_unique<State>());
  const auto token = _lifetime.token();
  Connect<void(AdwDialog *)>(dialog, "closed", [this, token](AdwDialog *) {
    if (!token.expired()) _pinnedDialog = nullptr;
  });
  Connect<void(GtkButton *)>(action, "clicked", [this, token, dialog, canPin](GtkButton *) {
    adw_dialog_close(ADW_DIALOG(dialog));
    if (token.expired()) return;
    if (canPin) confirmUnpinAll();
    else confirmHidePinned();
  });
  Connect<void(GtkListBox *, GtkListBoxRow *)>(list, "row-activated", [this, token, dialog](GtkListBox *, GtkListBoxRow *row) {
    const auto id = *static_cast<std::int64_t *>(g_object_get_data(G_OBJECT(row), "contrail-message"));
    adw_dialog_close(ADW_DIALOG(dialog));
    if (token.expired() || !id) return;
    _pinnedClicked = id;
    showAt(id);
    updatePinnedViewer();
  });
  const auto load = std::make_shared<std::function<void(std::int64_t)>>();
  *load = [this, token, state, list, title](std::int64_t from) {
    if (state->loading || token.expired()) return;
    state->loading = true;
    _store.client().request<api::foundChatMessages>(
        SearchPinned(_chat, topicObject(), from), state->lifetime,
        [this, token, state, list, title](api::foundChatMessages &found) {
          state->loading = false;
          if (token.expired()) return;
          state->next = found.messages_.empty() ? 0 : found.next_from_message_id_;
          if (found.total_count_ > 0) {
            adw_window_title_set_title(ADW_WINDOW_TITLE(title),
                                       TrCount("lng_pinned_messages_title", found.total_count_).c_str());
          }
          for (const auto &message : found.messages_) {
            if (!message) continue;
            auto name = std::string();
            auto color = 0;
            DescribeSender(_store, *message, name, color);
            auto *row = adw_action_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                          Escape(name + " · " + Td::FormatListDate(message->date_)).c_str());
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
                                        Escape(Td::OneLine(Td::MessagePreview(*message, _store))).c_str());
            adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 2);
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
            AttachData(row, "contrail-message", std::make_unique<std::int64_t>(message->id_));
            gtk_list_box_append(GTK_LIST_BOX(list), row);
          }
        },
        [state](api::error &) { state->loading = false; });
  };
  // The next page near the end of the list.
  Connect<void(GtkScrolledWindow *, GtkPositionType)>(scroll, "edge-reached",
                                                      [state, load](GtkScrolledWindow *, GtkPositionType position) {
    if (position == GTK_POS_BOTTOM && state->next) (*load)(state->next);
  });
  (*load)(0);
  adw_dialog_present(ADW_DIALOG(dialog), _window.widget());
}

// --- the list ----------------------------------------------------------------

int ChatView::compareRows(GtkListBoxRow *a, GtkListBoxRow *b, gpointer) {
  const auto first = RowMessage(a), second = RowMessage(b);
  if (!first || !second) return 0;
  // Messages being sent have local ids: they go after the others. A sponsored message goes right after the one it
  // follows.
  const auto key = [](const MessageItem *item) {
    return item->sponsored ? std::tuple(false, item->sponsoredAfter, 1) : std::tuple(IsLocal(item), item->id(), 0);
  };
  const auto left = key(first->item()), right = key(second->item());
  return (left < right) ? -1 : (right < left) ? 1 : 0;
}

// Date separators and grouping of consecutive messages from the same sender.
void ChatView::updateRowHeader(GtkListBoxRow *row, GtkListBoxRow *before, gpointer data) {
  const auto self = static_cast<ChatView *>(data);
  const auto message = RowMessage(row), previous = RowMessage(before);
  if (!message) return;
  const auto &item = *message->item()->data;
  const auto day = Td::DayNumber(item.date_);
  const auto newDay = !previous || Td::DayNumber(previous->item()->data->date_) != day;
  const auto unreadBar = message == self->_unreadRow;
  // Headers are recomputed for all the rows on every change of the list: keep the ones that stay the same instead
  // of creating new widgets (and a new layout) each time.
  const auto key = (newDay || unreadBar) ? gsize(day * 2 + (unreadBar ? 1 : 0)) : gsize(0);
  const auto same = (key == GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(row), "contrail-header-key")))
                    && (!key || gtk_list_box_row_get_header(row));
  g_object_set_data(G_OBJECT(row), "contrail-header-key", GSIZE_TO_POINTER(key));
  if (same) {
  } else if (newDay || unreadBar) {
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    if (newDay) {
      auto *label = gtk_label_new(Td::FormatDay(item.date_).c_str());
      gtk_widget_add_css_class(label, "date-separator");
      gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
      gtk_box_append(GTK_BOX(box), label);
    }
    if (unreadBar) {
      auto *label = gtk_label_new(Tr("lng_unread_bar_some").c_str());
      gtk_widget_add_css_class(label, "unread-bar");
      gtk_box_append(GTK_BOX(box), label);
    }
    gtk_list_box_row_set_header(row, box);
  } else {
    gtk_list_box_row_set_header(row, nullptr);
  }
  const auto service = [](const MessageWidget *widget) {
    return Td::IsServiceMessage(*widget->item()->data->content_);
  };
  const auto sameSender = !newDay && previous && !service(previous) && !service(message)
                          && SenderKey(*previous->item()->data) == SenderKey(item)
                          && item.date_ - previous->lastItem()->data->date_ < 15 * 60;
  message->setFirstInGroup(!sameSender);
}

void ChatView::buildList() {
  _list = gtk_list_box_new();
  gtk_widget_add_css_class(_list, "message-list");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(_list), GTK_SELECTION_NONE);
  gtk_widget_set_valign(_list, GTK_ALIGN_END);
  gtk_list_box_set_sort_func(GTK_LIST_BOX(_list), compareRows, this, nullptr);
  gtk_list_box_set_header_func(GTK_LIST_BOX(_list), updateRowHeader, this, nullptr);

  auto *clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 720);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 560);
  adw_clamp_set_child(ADW_CLAMP(clamp), _list);

  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  // Room for the older messages above the loaded ones (see setTopReserve).
  _topSpacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(box), _topSpacer);
  // The spinner and the retry button float over the messages.
  auto *spinner = adw_spinner_new();
  gtk_widget_set_size_request(spinner, 24, 24);
  _loading = adw_bin_new();
  adw_bin_set_child(ADW_BIN(_loading), spinner);
  gtk_widget_add_css_class(_loading, "osd");
  gtk_widget_add_css_class(_loading, "history-loading");
  gtk_widget_set_halign(_loading, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(_loading, GTK_ALIGN_START);
  gtk_widget_set_margin_top(_loading, 10);
  gtk_widget_set_can_target(_loading, FALSE);
  gtk_widget_set_visible(_loading, FALSE);
  _retry = gtk_button_new_with_label(Tr("lng_bot_download_retry").c_str());
  gtk_widget_add_css_class(_retry, "osd");
  gtk_widget_add_css_class(_retry, "pill");
  gtk_widget_set_halign(_retry, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(_retry, GTK_ALIGN_START);
  gtk_widget_set_margin_top(_retry, 10);
  gtk_widget_set_visible(_retry, FALSE);
  _signals.clicked(_retry, [this] {
    _loadFailed = false;
    gtk_widget_set_visible(_retry, FALSE);
    if (!_initialLoaded) loadInitial();
    else scheduleViewportUpdate();
  });
  gtk_box_append(GTK_BOX(box), clamp);
  gtk_widget_set_valign(box, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom(box, 6);

  _scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(_scroll), box);
  auto *viewport = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(_scroll));
  if (GTK_IS_VIEWPORT(viewport)) gtk_viewport_set_scroll_to_focus(GTK_VIEWPORT(viewport), FALSE);
  gtk_widget_set_vexpand(_scroll, TRUE);
  gtk_widget_add_css_class(_scroll, "chat-background");
  auto *touch = gtk_event_controller_legacy_new();
  gtk_event_controller_set_propagation_phase(touch, GTK_PHASE_CAPTURE);
  _signals.connect<gboolean(GtkEventControllerLegacy *, GdkEvent *)>(touch, "event",
      [this](GtkEventControllerLegacy *, GdkEvent *event) -> gboolean {
        const auto type = gdk_event_get_event_type(event);
        if (type == GDK_TOUCH_BEGIN || type == GDK_BUTTON_PRESS) {
          _touchActive = true;
        } else if (type == GDK_TOUCH_END || type == GDK_TOUCH_CANCEL || type == GDK_BUTTON_RELEASE) {
          _touchActive = false;
          _reserveTimer.callOnce(300);
        }
        if (type == GDK_TOUCH_UPDATE || type == GDK_SCROLL
            || (type == GDK_MOTION_NOTIFY && (gdk_event_get_modifier_state(event) & GDK_BUTTON1_MASK))) {
          // A real drag takes precedence over the initial unread/reply positioning.
          _scrollTarget = 0;
          _scrollTargetY = std::nullopt;
          _stickToBottom = false;
        }
        return FALSE;
      });
  gtk_widget_add_controller(_scroll, touch);
  _adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(_scroll));
  _signals.connect<void(GtkAdjustment *)>(_adjustment, "value-changed", [this](GtkAdjustment *) {
    if (!_adjustingScroll) NoteListMotion();
    PostponeMemoryTrim();
    scrolled();
  });
  _signals.connect<void(GObject *, GParamSpec *)>(_adjustment, "notify::upper", [this](GObject *, GParamSpec *) {
    // Emitted while the viewport is allocated, before the child is placed and anything is drawn:
    // a shift requested with the content change lands in the same frame, without a visible jump.
    if (_pendingShift != 0.) {
      const auto shift = std::exchange(_pendingShift, 0.);
      // A shrinking reserve may already have clamped the position.
      const auto base = (shift < 0) ? _shiftBase : gtk_adjustment_get_value(_adjustment);
      if (!scrollIdleIgnoringShift()) {
        // The reserve ran out during a fling: GTK's deceleration would put its own position back.
        gtk_scrolled_window_set_kinetic_scrolling(GTK_SCROLLED_WINDOW(_scroll), FALSE);
        gtk_scrolled_window_set_kinetic_scrolling(GTK_SCROLLED_WINDOW(_scroll), TRUE);
      }
      _adjustingScroll = true;
      gtk_adjustment_set_value(_adjustment, base + shift);
      _adjustingScroll = false;
    }
    scheduleViewportUpdate();
  });
  _signals.connect<void(GObject *, GParamSpec *)>(_adjustment, "notify::page-size",
                                                  [this](GObject *, GParamSpec *) { scheduleViewportUpdate(); });

  auto *overlay = gtk_overlay_new();
  gtk_overlay_set_child(GTK_OVERLAY(overlay), _scroll);
  _toBottom = gtk_button_new_from_icon_name("go-bottom-symbolic");
  gtk_widget_add_css_class(_toBottom, "circular");
  gtk_widget_add_css_class(_toBottom, "osd");
  gtk_widget_add_css_class(_toBottom, "to-bottom");
  gtk_widget_set_halign(_toBottom, GTK_ALIGN_END);
  gtk_widget_set_valign(_toBottom, GTK_ALIGN_END);
  gtk_widget_set_margin_end(_toBottom, 18);
  gtk_widget_set_margin_bottom(_toBottom, 18);
  gtk_widget_set_visible(_toBottom, FALSE);
  _signals.clicked(_toBottom, [this] {
    // Jumped somewhere into the past: the end of the chat comes again.
    if (!_reachedBottom) reload(0);
    else scrollToBottom();
  });
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), _toBottom);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), _loading);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), _retry);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(_root), overlay);
}

// --- pages of history ----------------------------------------------------------

void ChatView::loadInitial() {
  if (_loadingOlder || _loadingNewer || _applyingPage) return;
  if (_pendingShowAt > 0) {
    requestHistory(_pendingShowAt, -kMessagesPerPage / 2, kMessagesPerPage, Page::Initial);
  } else {
    requestHistory(0, 0, kMessagesPerPage, Page::Initial);
  }
}

void ChatView::loadOlder() {
  if (_loadingOlder || _loadingNewer || _applyingPage || _loadFailed || !_initialLoaded || _reachedTop) return;
  if (const auto oldest = oldestId()) requestHistory(oldest, 0, kMessagesPerPage, Page::Older);
}

void ChatView::loadNewer() {
  if (_loadingOlder || _loadingNewer || _applyingPage || _loadFailed || !_initialLoaded || _reachedBottom) return;
  if (const auto newest = newestId()) requestHistory(newest, -kMessagesPerPage + 1, kMessagesPerPage, Page::Newer);
}

void ChatView::requestHistory(std::int64_t from, int offset, int limit, Page page) {
  gtk_widget_set_visible(_loading, TRUE);
  gtk_widget_set_visible(_retry, FALSE);
  (page == Page::Newer ? _loadingNewer : _loadingOlder) = true;
  auto function = _topic ? Td::FunctionPtr(api::make_object<api::getForumTopicHistory>(_chat, _topic, from, offset, limit))
                         : Td::FunctionPtr(api::make_object<api::getChatHistory>(_chat, from, offset, limit, false));
  _store.client().request<api::messages>(
      std::move(function), _pageLifetime,
      [this, page](api::messages &messages) { applyPage(messages, page); },
      [this, page](api::error &error) {
        (page == Page::Newer ? _loadingNewer : _loadingOlder) = false;
        pageFailed(error.message_);
      });
}

void ChatView::pageFailed(const std::string &text) {
  _loadFailed = true;
  gtk_widget_set_visible(_loading, FALSE);
  gtk_widget_set_visible(_retry, TRUE);
  ShowToast(text);
}

void ChatView::applyPage(api::messages &messages, Page page) {
  _applyingPage = true;
  const auto first = !_initialLoaded;
  // An older page goes above the messages on the screen: measure how much it adds.
  const auto keepPosition = page == Page::Older && !first;
  const auto heightBefore = keepPosition ? listHeight() : 0;
  auto added = 0;
  for (auto &message : messages.messages_) {
    if (message && !message->scheduling_state_ && insert(std::move(message))) ++added;
  }
  // TDLib gives as many messages as it has at hand, sometimes one: only nothing new means the end.
  if (page == Page::Older && !added) _reachedTop = true;
  if (page == Page::Newer && !added) _reachedBottom = true;
  if (page == Page::Initial && !_pendingShowAt) _reachedBottom = true;
  if (first && _items.empty()) _reachedTop = _reachedBottom = true;
  const auto known = readState();
  if (known && newestId() >= known->lastMessageId) _reachedBottom = true;
  _initialLoaded = true;
  sync();
  if (keepPosition) {
    if (const auto grown = listHeight() - heightBefore; grown > 0) {
      const auto fromReserve = std::min(grown, _topReserve);
      // Without enough room above, the rest moves the content and the position with it.
      setTopReserve(_topReserve - fromReserve, grown - fromReserve);
    }
  }
  if (first) {
    setTopReserve(_reachedTop ? 0 : kTopReserve, 0);
    if (page == Page::Start) {
      // Whichever message is the first one left in the chat, at the top of the screen.
      _scrollTargetY = 0.;
      if (!scrollToLoaded(oldestId())) scrollToBottom();
    } else if (_pendingShowAt > 0) {
      if (!scrollToLoaded(std::exchange(_pendingShowAt, 0))) scrollToBottom();
    } else {
      scrollToBottom();
    }
    _readTimer.callOnce(500);
  }
  (page == Page::Newer ? _loadingNewer : _loadingOlder) = false;
  _applyingPage = false;
  gtk_widget_set_visible(_loading, FALSE);
  if (g_getenv("CONTRAIL_DEBUG")) {
    g_message("History page: %s received=%zu added=%d rows=%zu top=%d bottom=%d",
              page == Page::Older ? "older" : page == Page::Newer ? "newer" : "first", messages.messages_.size(),
              added, _rows.size(), int(_reachedTop), int(_reachedBottom));
  }
  scheduleViewportUpdate();
}

void ChatView::reload(std::int64_t showAt) {
  unload();
  _pendingShowAt = showAt;
  _stickToBottom = !showAt;
  loadInitial();
}

// No rows, no requests: the chat starts loading again.
void ChatView::unload() {
  _pageLifetime.reset();
  _initialLoaded = false;
  _loadFailed = false;
  _loadingOlder = _loadingNewer = false;
  _reachedTop = _reachedBottom = false;
  _scrollTarget = 0;
  _pendingShift = 0;
  setTopReserve(0, 0);
  _unreadRow = nullptr;
  _itemRows.clear();
  _nearRows.clear();
  _rows.clear();
  _items.clear();
  _viewed.clear();
}

// A fling or the scroll bar stopped `gap` px above the rows: the messages about that far back come at once. TDLib
// counts history only forward from a message, so the way back goes by date: as many seconds back as that many
// messages took around here.
void ChatView::jumpUp(double gap) {
  const auto value = gtk_adjustment_get_value(_adjustment);
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  // At the top of the scroll bar: the start of the chat.
  if (value < page) {
    jumpToStart();
    return;
  }
  const auto rowHeight = std::clamp(double(listHeight()) / std::max(_rows.size(), size_t(1)), kJumpRowMin, kJumpRowMax);
  const auto back = (gap + page / 2) / rowHeight;
  auto regular = Items();
  for (const auto &item : _items) {
    if (!IsLocal(item.get())) regular.push_back(item.get());
  }
  if (regular.size() < 2) {
    jumpToStart();
    return;
  }
  const auto oldest = regular.front()->data->date_;
  const auto span = regular.back()->data->date_ - oldest;
  const auto seconds = std::max(double(span) / double(regular.size() - 1), 1.);
  const auto date = std::int32_t(std::max(oldest - back * seconds, 1.));
  _pageLifetime.reset();
  _loadingOlder = true;
  gtk_widget_set_visible(_loading, TRUE);
  _store.client().request<api::message>(
      api::make_object<api::getChatMessageByDate>(_chat, date), _pageLifetime,
      [this, page](api::message &message) {
        _loadingOlder = false;
        jumpToMessage(message.id_, page / 2);
      },
      // Before the chat began.
      [this](api::error &) {
        _loadingOlder = false;
        jumpToStart();
      });
}

void ChatView::jumpToMessage(std::int64_t id, double y) {
  unload();
  _stickToBottom = false;
  _pendingShowAt = id;
  _scrollTargetY = y;
  loadInitial();
}

void ChatView::jumpToStart() {
  unload();
  _stickToBottom = false;
  requestHistory(kFirstMessageId, -kMessagesPerPage + 1, kMessagesPerPage, Page::Start);
}

// --- messages ------------------------------------------------------------------

MessageItem *ChatView::find(std::int64_t id) const {
  const auto i = std::ranges::find_if(_items, [&](const auto &item) { return item->id() == id; });
  return i != _items.end() ? i->get() : nullptr;
}

std::int64_t ChatView::oldestId() const {
  for (const auto &item : _items) {
    if (!item->local()) return item->id();
  }
  return 0;
}

std::int64_t ChatView::newestId() const {
  for (auto i = _items.rbegin(); i != _items.rend(); ++i) {
    if (!(*i)->local()) return (*i)->id();
  }
  return 0;
}

// Null when it is here already.
MessageItem *ChatView::insert(api::object_ptr<api::message> message) {
  if (find(message->id_)) return nullptr;
  auto item = std::make_unique<MessageItem>();
  item->data = std::move(message);
  const auto raw = item.get();
  const auto before = [](const std::unique_ptr<MessageItem> &a, const std::unique_ptr<MessageItem> &b) {
    return std::pair(a->local(), a->id()) < std::pair(b->local(), b->id());
  };
  const auto place = std::upper_bound(_items.begin(), _items.end(), item, before);
  _items.insert(place, std::move(item));
  return raw;
}

void ChatView::resort() {
  std::stable_sort(_items.begin(), _items.end(), [](const auto &a, const auto &b) {
    return std::pair(a->local(), a->id()) < std::pair(b->local(), b->id());
  });
}

void ChatView::remove(const std::vector<std::int64_t> &ids) {
  const auto gone = std::unordered_set<std::int64_t>(ids.begin(), ids.end());
  // The rows let go of the messages first.
  auto removed = std::vector<std::unique_ptr<MessageItem>>();
  for (auto i = _items.begin(); i != _items.end();) {
    if (gone.contains((*i)->id())) {
      removed.push_back(std::move(*i));
      i = _items.erase(i);
    } else {
      ++i;
    }
  }
  if (removed.empty()) return;
  sync();
}

// The rows follow the messages: one per message, one per album. A row keeps its widgets when its album grows (the
// rest of it comes with the next page) or shrinks (a message deleted).
void ChatView::sync() {
  auto units = std::vector<Items>();
  auto unitOfAlbum = std::unordered_map<std::int64_t, std::size_t>();
  for (const auto &item : _items) {
    const auto &message = *item->data;
    // A group became a supergroup: Telegram Desktop shows no line for what has no text.
    if (Td::IsServiceMessage(*message.content_) && Td::ServiceText(message, _store).empty()) continue;
    if (const auto album = message.media_album_id_) {
      const auto [i, added] = unitOfAlbum.emplace(album, units.size());
      if (!added) {
        units[i->second].push_back(item.get());
        continue;
      }
    }
    units.push_back({item.get()});
  }
  // Only while the message it follows is held: far up the chat it waits to be shown again.
  if (_sponsoredPlaced && find(_sponsored->sponsoredAfter)) units.push_back({_sponsored.get()});
  auto used = std::set<MessageWidget *>();
  for (auto &unit : units) {
    MessageWidget *row = nullptr;
    for (const auto item : unit) {
      if (const auto i = _itemRows.find(item); i != _itemRows.end() && !used.contains(i->second)) {
        row = i->second;
        break;
      }
    }
    if (row) {
      setRowItems(row, std::move(unit));
    } else {
      row = addRow(std::move(unit));
    }
    used.insert(row);
  }
  _unreadRow = nullptr;
  for (auto i = _rows.begin(); i != _rows.end();) {
    if (!used.contains(i->first)) {
      forgetRow(i->first);
      i = _rows.erase(i);
    } else {
      ++i;
    }
  }
  // The bar goes before the first message the user hasn't read, however his own ones lie around it.
  _unreadRow = nullptr;
  if (_unreadAfter) {
    for (const auto &item : _items) {
      if (item->id() > _unreadAfter && !item->local() && !item->data->is_outgoing_) {
        _unreadRow = rowFor(item.get());
        if (_unreadRow) break;
      }
    }
  }
  // Also recomputes the headers.
  gtk_list_box_invalidate_sort(GTK_LIST_BOX(_list));
  gtk_list_box_invalidate_headers(GTK_LIST_BOX(_list));
  scheduleViewportUpdate();
}

MessageWidget *ChatView::addRow(Items items) {
  auto row = std::make_unique<MessageWidget>(std::move(items), *this);
  const auto raw = row.get();
  gtk_list_box_append(GTK_LIST_BOX(_list), raw->row());
  for (const auto item : raw->items()) _itemRows[item] = raw;
  _rows.emplace(raw, std::move(row));
  return raw;
}

void ChatView::setRowItems(MessageWidget *row, Items items) {
  if (row->items() == items) return;
  for (const auto item : row->items()) {
    if (const auto i = _itemRows.find(item); i != _itemRows.end() && i->second == row) _itemRows.erase(i);
  }
  row->setItems(std::move(items));
  for (const auto item : row->items()) _itemRows[item] = row;
  gtk_list_box_row_changed(GTK_LIST_BOX_ROW(row->row()));
}

void ChatView::forgetRow(MessageWidget *row) {
  _nearRows.erase(row);
  for (const auto item : row->items()) {
    if (const auto i = _itemRows.find(item); i != _itemRows.end() && i->second == row) _itemRows.erase(i);
  }
}

MessageWidget *ChatView::rowFor(const MessageItem *item) const {
  const auto i = _itemRows.find(item);
  return i != _itemRows.end() ? i->second : nullptr;
}

void ChatView::refreshItem(const MessageItem *item) {
  if (auto *row = rowFor(item)) {
    row->refresh();
    gtk_list_box_row_changed(GTK_LIST_BOX_ROW(row->row()));
  }
}

const api::message *ChatView::heldMessage(std::int64_t id) const {
  const auto item = find(id);
  return item ? item->data.get() : nullptr;
}

void ChatView::holdDialog(AdwDialog *dialog) {
  _dialogs.push_back(GTK_WIDGET(dialog));
  const auto token = _lifetime.token();
  Connect<void(AdwDialog *)>(dialog, "closed", [this, token](AdwDialog *dialog) {
    if (!token.expired()) std::erase(_dialogs, GTK_WIDGET(dialog));
  });
}

void ChatView::refreshFooters() {
  for (const auto &[raw, row] : _rows) row->refreshFooter();
}

// The rows far from the screen go while the list stands still, with their messages: TDLib has them. Above the
// screen they leave their height to the room reserved there, so nothing on the screen moves.
void ChatView::trimRows() {
  if (int(_rows.size()) <= kMaxRows || _scrollTarget || !_initialLoaded) return;
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  const graphene_point_t zero = GRAPHENE_POINT_INIT_ZERO;
  graphene_point_t top;
  if (page <= 0 || !gtk_widget_compute_point(_scroll, _list, &zero, &top)) return;
  auto removed = std::vector<std::unique_ptr<MessageItem>>();
  const auto drop = [&](auto condition) {
    for (auto i = _items.begin(); i != _items.end();) {
      if (condition(**i)) {
        removed.push_back(std::move(*i));
        i = _items.erase(i);
      } else {
        ++i;
      }
    }
  };
  const auto keptBelow = RowMessage(RowFrom(_list, top.y + page + kKeepHeight));
  if (keptBelow && !_stickToBottom
      && gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(keptBelow->row())) < int(_rows.size()) - 1) {
    const auto newest = keptBelow->lastItem()->id();
    drop([&](const MessageItem &item) { return item.local() || item.id() > newest; });
    _reachedBottom = false;
    sync();
  }
  const auto aboveY = int(top.y - kKeepHeight);
  const auto keptAbove = (aboveY > 0) ? RowMessage(RowFrom(_list, aboveY)) : nullptr;
  if (keptAbove && gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(keptAbove->row())) > 0) {
    const auto oldest = keptAbove->item()->id();
    const auto heightBefore = listHeight();
    drop([&](const MessageItem &item) { return item.id() < oldest; });
    _reachedTop = false;
    sync();
    if (const auto freed = heightBefore - listHeight(); freed > 0) setTopReserve(_topReserve + freed, 0);
  }
  if (g_getenv("CONTRAIL_DEBUG")) g_message("History trimmed: rows=%zu reserve=%d", _rows.size(), _topReserve);
}

// --- replies -------------------------------------------------------------------

ReplyPreview ChatView::replyPreview(const MessageItem &item) const {
  auto result = ReplyPreview();
  const auto reply = Cast<api::messageReplyToMessage>(item.data->reply_to_.get());
  if (!reply) return result;
  const auto here = !reply->chat_id_ || reply->chat_id_ == _chat;
  const api::message *message = nullptr;
  if (const auto loaded = here ? find(reply->message_id_) : nullptr) {
    message = loaded->data.get();
  } else if (const auto i = _replied.find(item.id()); i != _replied.end()) {
    if (!i->second) {
      result.ready = true;  // Deleted.
      return result;
    }
    message = i->second.get();
  }
  if (!message) {
    // Comes once; the rows waiting for it are made again then.
    if (_repliedAsked.insert(item.id()).second) {
      const auto id = item.id();
      auto self = const_cast<ChatView *>(this);
      _store.client().send(
          api::make_object<api::getRepliedMessage>(_chat, id),
          [self, id](Td::ObjectPtr object) {
            auto message = api::object_ptr<api::message>();
            if (object && object->get_id() == api::message::ID) {
              message.reset(static_cast<api::message *>(object.release()));
            }
            self->_replied[id] = std::move(message);
            self->repliedLoaded(id);
          },
          _lifetime);
    }
    return result;
  }
  result.ready = true;
  DescribeSender(_store, *message, result.name, result.color);
  result.text = Td::MessagePreview(*message, _store);
  result.target = (message->chat_id_ == _chat) ? message->id_ : 0;
  return result;
}

void ChatView::repliedLoaded(std::int64_t id) {
  if (const auto item = find(id)) refreshItem(item);
}

// --- scrolling -----------------------------------------------------------------

void ChatView::scrollToBottom() {
  _stickToBottom = true;
  _scrollTarget = 0;
  scheduleViewportUpdate();
}

void ChatView::scrollToItem(const MessageItem *item, bool highlight) {
  _stickToBottom = false;
  _scrollTarget = item->id();
  // Coming back to where the chat was: the same place on the screen, nothing lit up.
  _scrollTargetHighlight = highlight && !_scrollTargetY;
  scheduleViewportUpdate();
}

bool ChatView::scrollToLoaded(std::int64_t id) {
  if (const auto item = find(id); item && rowFor(item)) {
    // The place the chat opens at, the first unread message after it: nothing to point out there.
    if (id != _unreadAfter) {
      scrollToItem(item, true);
      return true;
    }
  }
  if (_unreadAfter && id == _unreadAfter) {
    if (_unreadRow) {
      scrollToItem(_unreadRow->item(), false);
      return true;
    }
    if (const auto item = find(id); item && rowFor(item)) {
      scrollToItem(item, false);
      return true;
    }
  }
  // The next message that exists, after a jump to a place that has none.
  if (_scrollTargetY) {
    for (const auto &item : _items) {
      if (item->id() >= id && rowFor(item.get())) {
        scrollToItem(item.get(), false);
        return true;
      }
    }
  }
  return false;
}

void ChatView::showAt(std::int64_t id) {
  _scrollTargetY = std::nullopt;
  if (scrollToLoaded(id)) return;
  reload(id);
}

bool ChatView::atBottom() const {
  const auto value = gtk_adjustment_get_value(_adjustment);
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  return value + page >= gtk_adjustment_get_upper(_adjustment) - 40 && _reachedBottom;
}

bool ChatView::scrollIdle() const {
  return scrollIdleIgnoringShift() && _pendingShift == 0.;
}

bool ChatView::scrollIdleIgnoringShift() const {
  return !_touchActive && !ListMoving();
}

void ChatView::scrolled() {
  const auto value = gtk_adjustment_get_value(_adjustment);
  const auto down = value > _lastScrollValue;
  _lastScrollValue = value;
  if (_adjustingScroll || _applyingPage || !_initialLoaded) return;
  if (down && _pinnedClicked && !_scrollTarget) {
    // Scrolled down by hand: the bar follows the screen again.
    _pinnedClicked = 0;
  }
  const auto upper = gtk_adjustment_get_upper(_adjustment);
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  const auto bottom = atBottom();
  // GTK also changes the adjustment while allocating newly inserted rows. Those changes do not
  // represent a user drag and must not drop the bottom/anchor/target requested before layout.
  // Only input events opt out of bottom following. Nor is a reserve shrinking at the start of the chat a way to
  // the bottom: GTK clamps the position to the smaller list before the shift waiting for it puts it back.
  if (!_viewportSource && !_scrollTarget && bottom && _pendingShift == 0.) _stickToBottom = true;
  gtk_widget_set_visible(_toBottom, !bottom && upper > page);
  if (!_viewportSource) scheduleViewportUpdate();
  if (!_readTimer.active()) _readTimer.callOnce(300);
}

int ChatView::listHeight() const {
  const auto width = gtk_widget_get_width(_list);
  if (width <= 0) return 0;
  // GtkListBox gives each row its minimum height for the width.
  auto minimum = 0;
  gtk_widget_measure(_list, GTK_ORIENTATION_VERTICAL, width, &minimum, nullptr, nullptr, nullptr);
  return minimum;
}

// GTK's touch scrolling and fling keep their own absolute position: changing the scroll position
// while the finger or a fling moves the list makes it jump back, and loads more pages on the way.
// So older pages don't move the messages on the screen: they take the place of empty room reserved
// above them. The room is topped up (and removed at the start of the chat) only while the list
// stands still, together with the matching scroll position, in the same frame.
void ChatView::setTopReserve(int reserve, double shift) {
  reserve = std::max(reserve, 0);
  if (reserve != _topReserve) {
    _topReserve = reserve;
    gtk_widget_set_size_request(_topSpacer, -1, reserve);
  }
  if (shift != 0.) {
    if (_pendingShift == 0.) _shiftBase = gtk_adjustment_get_value(_adjustment);
    _pendingShift += shift;
  }
}

void ChatView::maintainTopReserve() {
  if (!_initialLoaded || _applyingPage || _loadingOlder) return;
  const auto wanted = [&] {
    return _reachedTop ? 0
           : (_topReserve < kTopReserveLow || _topReserve > kTopReserveHigh) ? kTopReserve
                                                                             : _topReserve;
  };
  const auto trim = int(_rows.size()) > kMaxRows && !_scrollTarget;
  if (wanted() == _topReserve && !trim) return;
  if (!scrollIdle()) {
    _reserveTimer.callOnce(300);
    return;
  }
  if (trim) trimRows();
  if (const auto now = wanted(); now != _topReserve) setTopReserve(now, now - _topReserve);
}

void ChatView::scheduleViewportUpdate() {
  if (_viewportSource) return;
  // Waits for GTK's next frame to allocate new rows before restoring scroll coordinates.
  _viewportSource = g_timeout_add(20, +[](gpointer data) -> gboolean {
    auto *self = static_cast<ChatView *>(data);
    self->_viewportSource = 0;
    self->updateViewport();
    return G_SOURCE_REMOVE;
  }, this);
}

void ChatView::updateViewport() {
  if (_applyingPage || !_initialLoaded) return;
  if (!gtk_widget_get_mapped(_root)) return;
  if (gtk_adjustment_get_page_size(_adjustment) <= 0
      || std::ranges::any_of(_rows, [](const auto &pair) {
           // A hidden row (an empty service message) has no height and never gets one.
           auto *row = pair.second->row();
           return gtk_widget_get_visible(row) && gtk_widget_get_height(row) <= 0;
         })) {
    scheduleViewportUpdate();
    return;
  }
  _adjustingScroll = true;
  const auto before = gtk_adjustment_get_value(_adjustment);
  if (_scrollTarget) {
    const auto item = find(_scrollTarget);
    if (const auto row = item ? rowFor(item) : nullptr) {
      // A message of an album: its cell, not the album's top.
      const auto y = _scrollTargetY.value_or(gtk_adjustment_get_page_size(_adjustment) / 3);
      if (ScrollWidgetToY(row->widgetFor(item), _scroll, _adjustment, y)) {
        if (_scrollTargetHighlight) row->highlight(item);
        _scrollTarget = 0;
        _scrollTargetY = std::nullopt;
      }
    } else {
      _scrollTarget = 0;
    }
  } else if (_stickToBottom) {
    gtk_adjustment_set_value(_adjustment,
                             gtk_adjustment_get_upper(_adjustment) - gtk_adjustment_get_page_size(_adjustment));
  }
  _adjustingScroll = false;
  maintainTopReserve();
  const auto upper = gtk_adjustment_get_upper(_adjustment);
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  gtk_widget_set_visible(_toBottom, !atBottom() && upper > page);
  if (atBottom()) showSponsored();
  preloadVisibleEdges();
  updateNearPictures();
  // The rows are where the new position puts them only after the next allocation: the bar looks at them then.
  if (gtk_adjustment_get_value(_adjustment) != before) scheduleViewportUpdate();
  else updatePinnedViewer();
  if (!_readTimer.active()) _readTimer.callOnce(300);
}

void ChatView::updateNearPictures() {
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  const graphene_point_t zero = GRAPHENE_POINT_INIT_ZERO;
  graphene_point_t top;
  if (page <= 0 || !gtk_widget_compute_point(_scroll, _list, &zero, &top)) return;
  // A fling past the first message, into the room reserved above, comes back to it: the rows at the edge of the list
  // stay as near as if the screen stood there.
  top.y = std::clamp(double(top.y), 0., std::max(double(gtk_widget_get_height(_list)) - page, 0.));
  // The rows move by the shift still to come: until it lands nothing is far.
  for (auto i = _nearRows.begin(); _pendingShift == 0. && i != _nearRows.end();) {
    graphene_rect_t bounds;
    if (!gtk_widget_compute_bounds((*i)->row(), _list, &bounds)
        || bounds.origin.y + bounds.size.height < top.y - kPictureFarHeight
        || bounds.origin.y > top.y + page + kPictureFarHeight) {
      (*i)->setPicturesNear(false);
      i = _nearRows.erase(i);
    } else {
      ++i;
    }
  }
  // Rows from the first one reaching the near distance above the screen, in their order down. The decodes go one
  // after another: those on the screen first, then outwards.
  auto coming = std::vector<std::pair<double, MessageWidget *>>();
  for (auto *row = RowFrom(_list, top.y - kPictureNearHeight); row; row = NextRow(row)) {
    graphene_rect_t bounds;
    if (!gtk_widget_get_visible(GTK_WIDGET(row)) || !gtk_widget_compute_bounds(GTK_WIDGET(row), _list, &bounds)) {
      continue;
    }
    if (bounds.origin.y > top.y + page + kPictureNearHeight) break;
    if (auto *message = RowMessage(row); message && !_nearRows.contains(message)) {
      const auto bottom = bounds.origin.y + bounds.size.height;
      const auto distance = (bottom < top.y) ? top.y - bottom : std::max(bounds.origin.y - top.y - page, 0.);
      coming.emplace_back(distance, message);
    }
  }
  std::ranges::stable_sort(coming, {}, &std::pair<double, MessageWidget *>::first);
  for (const auto &[distance, message] : coming) {
    _nearRows.insert(message);
    message->setPicturesNear(true);
  }
}

void ChatView::preloadVisibleEdges() {
  if (_applyingPage || _loadingOlder || _loadingNewer || !_initialLoaded || _loadFailed || _scrollTarget) return;
  const auto value = gtk_adjustment_get_value(_adjustment);
  const auto upper = gtk_adjustment_get_upper(_adjustment);
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  if (value - _topReserve < kPreloadHeight && !_reachedTop) {
    if (!_topic && value + page < _topReserve - kJumpGap) {
      // Far above the rows (by date, which a topic doesn't go by): the messages there are loaded when the list stops, not every page on the way.
      if (scrollIdle()) {
        jumpUp(_topReserve - value - page);
      } else {
        _jumpTimer.callOnce(300);
      }
      return;
    }
    loadOlder();
  } else if (value + page > upper - kPreloadHeight) {
    loadNewer();
  }
}

std::vector<MessageWidget *> ChatView::visibleRows() const {
  auto result = std::vector<MessageWidget *>();
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  const graphene_point_t zero = GRAPHENE_POINT_INIT_ZERO;
  graphene_point_t top;
  if (page <= 0 || !gtk_widget_compute_point(_scroll, _list, &zero, &top)) return result;
  // Above the first message (the room for older pages) the list starts lower on the screen; a date header at the
  // top belongs to the row below it.
  for (auto *row = RowFrom(_list, top.y); row; row = NextRow(row)) {
    graphene_rect_t bounds;
    if (!gtk_widget_get_visible(GTK_WIDGET(row))) continue;
    if (!gtk_widget_compute_bounds(GTK_WIDGET(row), _list, &bounds)) break;
    if (bounds.origin.y >= top.y + page) break;
    if (auto *message = RowMessage(row)) result.push_back(message);
  }
  return result;
}

// What was seen whole goes to TDLib, which counts the views and moves the read mark. At the end of the chat
// everything is read, whatever was not on the screen.
void ChatView::markAsRead() {
  if (!gtk_window_is_active(GTK_WINDOW(_window.widget())) || _window.currentChat() != _chat
      || _window.currentTopic() != _topic || !_initialLoaded
      || _scrollTarget || !gtk_widget_get_mapped(_root)) {
    return;
  }
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  const auto known = readState();
  if (page <= 0 || !known) return;
  auto ids = std::vector<std::int64_t>();
  for (const auto row : visibleRows()) {
    for (const auto item : row->items()) {
      if (item->local() || item->sponsored || _viewed.contains(item->id())) continue;
      double y = 0;
      auto *widget = row->widgetFor(item);
      const auto height = gtk_widget_get_height(widget);
      if (height > 0 && WidgetViewportY(widget, _scroll, y) && y + height >= 0 && y + height <= page) {
        ids.push_back(item->id());
        _viewed.insert(item->id());
      }
    }
  }
  if ((_stickToBottom || atBottom()) && known->unread > 0 && known->lastMessageId
      && !std::ranges::count(ids, known->lastMessageId)) {
    ids.push_back(known->lastMessageId);
  }
  if ((_stickToBottom || atBottom()) && known->markedUnread) {
    _store.client().send(api::make_object<api::toggleChatIsMarkedAsUnread>(_chat, false));
  }
  reportSponsoredView();
  if (ids.empty()) return;
  auto source = _topic ? api::object_ptr<api::MessageSource>(api::make_object<api::messageSourceForumTopicHistory>())
                      : api::object_ptr<api::MessageSource>(api::make_object<api::messageSourceChatHistory>());
  _store.client().send(api::make_object<api::viewMessages>(_chat, std::move(ids), std::move(source), false));
}

// --- the message field ---------------------------------------------------------

void ChatView::buildComposer() {
  auto *composer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  // Replying to / editing a message.
  _replyBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(_replyBar, "reply-bar");
  _replyIcon = gtk_image_new_from_icon_name("mail-reply-sender-symbolic");
  gtk_widget_add_css_class(_replyIcon, "accent");
  gtk_box_append(GTK_BOX(_replyBar), _replyIcon);
  auto *replyColumn = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(replyColumn, TRUE);
  _replyTitle = gtk_label_new(nullptr);
  gtk_widget_add_css_class(_replyTitle, "reply-name");
  gtk_label_set_xalign(GTK_LABEL(_replyTitle), 0);
  gtk_label_set_ellipsize(GTK_LABEL(_replyTitle), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(replyColumn), _replyTitle);
  _replyText = gtk_label_new(nullptr);
  gtk_label_set_xalign(GTK_LABEL(_replyText), 0);
  gtk_label_set_ellipsize(GTK_LABEL(_replyText), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode(GTK_LABEL(_replyText), TRUE);
  gtk_widget_add_css_class(_replyText, "dim-label");
  gtk_box_append(GTK_BOX(replyColumn), _replyText);
  gtk_box_append(GTK_BOX(_replyBar), replyColumn);
  auto *cancel = MakeIconButton("window-close-symbolic", Tr("lng_cancel").c_str(), "flat");
  gtk_widget_add_css_class(cancel, "circular");
  _signals.clicked(cancel, [this] {
    if (_replyToId || _editing) {
      cancelReplyOrEdit();
    } else {
      cancelForward();
    }
  });
  gtk_box_append(GTK_BOX(_replyBar), cancel);
  // Messages to forward: a tap on them shows what can be done with them.
  auto *replyClick = gtk_gesture_click_new();
  _signals.connect<void(GtkGestureClick *, int, double, double)>(
      replyClick, "released", [this](GtkGestureClick *, int, double, double) {
        if (_forwardShown && !_replyToId && !_editing) showForwardMenu();
      });
  gtk_widget_add_controller(replyColumn, GTK_EVENT_CONTROLLER(replyClick));
  gtk_widget_set_visible(_replyBar, FALSE);
  gtk_box_append(GTK_BOX(composer), _replyBar);

  // The message field.
  auto *field = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(field, "composer");
  _input = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(_input), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(_input), FALSE);
  gtk_text_view_set_input_hints(GTK_TEXT_VIEW(_input),
                                GtkInputHints(GTK_INPUT_HINT_SPELLCHECK | GTK_INPUT_HINT_WORD_COMPLETION
                                              | GTK_INPUT_HINT_EMOJI | GTK_INPUT_HINT_UPPERCASE_SENTENCES));
  gtk_widget_add_css_class(_input, "message-input");
  auto *inputScroll = gtk_scrolled_window_new();
  // No scrollbar (a long text still scrolls with the finger): its minimum height made the empty
  // field almost twice as high as one line.
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(inputScroll), GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(inputScroll), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(inputScroll), 160);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(inputScroll), 32);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(inputScroll), _input);
  gtk_widget_set_hexpand(inputScroll, TRUE);
  gtk_widget_set_valign(inputScroll, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(inputScroll, "message-input-frame");
  // A file or a location, as the phone apps' attach menu.
  auto *attachMenu = g_menu_new();
  g_menu_append(attachMenu, Tr("lng_attach_file").c_str(), "attach.file");
  g_menu_append(attachMenu, Tr("lng_maps_point").c_str(), "attach.location");
  auto *attach = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(attach), "mail-attachment-symbolic");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(attach), G_MENU_MODEL(attachMenu));
  g_object_unref(attachMenu);
  gtk_widget_set_tooltip_text(attach, Tr("lng_attach").c_str());
  gtk_widget_add_css_class(attach, "flat");
  gtk_widget_add_css_class(attach, "circular");
  gtk_widget_set_valign(attach, GTK_ALIGN_END);
  auto *attachActions = g_simple_action_group_new();
  const auto addAttachAction = [&](const char *name, void (ChatView::*method)()) {
    auto *action = g_simple_action_new(name, nullptr);
    _signals.connect<void(GSimpleAction *, GVariant *)>(action, "activate",
                                                        [this, method](GSimpleAction *, GVariant *) { (this->*method)(); });
    g_action_map_add_action(G_ACTION_MAP(attachActions), G_ACTION(action));
    g_object_unref(action);
  };
  addAttachAction("file", &ChatView::chooseFiles);
  addAttachAction("location", &ChatView::chooseLocation);
  gtk_widget_insert_action_group(field, "attach", G_ACTION_GROUP(attachActions));
  g_object_unref(attachActions);
  gtk_box_append(GTK_BOX(field), attach);
  gtk_box_append(GTK_BOX(field), inputScroll);

  // Placeholder: a label over the empty field.
  const auto known = _store.chat(_chat);
  const auto broadcast = known && known->kind == Td::ChatKind::Channel;
  const auto placeholderText = Tr(broadcast ? "lng_broadcast_ph" : "lng_message_ph");
  auto *placeholder = gtk_label_new(placeholderText.c_str());
  // GtkTextView gives overlay children their minimum size, which an ellipsized label has none of.
  gtk_label_set_width_chars(GTK_LABEL(placeholder), std::min(int(g_utf8_strlen(placeholderText.c_str(), -1)), 24));
  gtk_widget_add_css_class(placeholder, "dim-label");
  gtk_widget_set_can_target(placeholder, FALSE);
  gtk_label_set_xalign(GTK_LABEL(placeholder), 0);
  gtk_label_set_ellipsize(GTK_LABEL(placeholder), PANGO_ELLIPSIZE_END);
  gtk_text_view_add_overlay(GTK_TEXT_VIEW(_input), placeholder, 0, 0);
  auto *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(_input));
  _signals.connect<void(GtkTextBuffer *)>(buffer, "changed", [this, placeholder](GtkTextBuffer *changed) {
    gtk_widget_set_visible(placeholder, gtk_text_buffer_get_char_count(changed) == 0);
    composerChanged();
  });

  _sendButton = MakeIconButton("contrail-send-symbolic", Tr("lng_send_button").c_str(), "suggested-action");
  gtk_widget_add_css_class(_sendButton, "circular");
  gtk_widget_set_valign(_sendButton, GTK_ALIGN_END);
  _signals.clicked(_sendButton, [this] { send(); });
  gtk_box_append(GTK_BOX(field), _sendButton);
  refreshSendButton();

  // Enter sends, Shift+Enter starts a new line.
  auto *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  _signals.connect<gboolean(GtkEventControllerKey *, guint, guint, GdkModifierType)>(
      keys, "key-pressed", [this](GtkEventControllerKey *, guint key, guint, GdkModifierType state) -> gboolean {
        if ((key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) && !(state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK))) {
          send();
          return TRUE;
        }
        if (key == GDK_KEY_Up && composerText().empty() && !_editing) {
          editLast();
          return TRUE;
        }
        return FALSE;
      });
  gtk_widget_add_controller(_input, keys);
  // Ctrl+V, Shift+Insert and "Paste" in the field's menu: files and pictures go to the attach dialog, text stays with
  // the field.
  _signals.connect<void(GtkTextView *)>(_input, "paste-clipboard", [this](GtkTextView *view) {
    if (ClassifyPaste(gdk_clipboard_get_formats(gtk_widget_get_clipboard(_input))) != PasteKind::Text) {
      g_signal_stop_emission_by_name(view, "paste-clipboard");
      paste();
    }
  });

  _composerStack = gtk_stack_new();
  gtk_stack_set_hhomogeneous(GTK_STACK(_composerStack), FALSE);
  gtk_stack_set_vhomogeneous(GTK_STACK(_composerStack), FALSE);
  gtk_stack_add_named(GTK_STACK(_composerStack), field, "field");
  _joinButton = gtk_button_new_with_label("");
  gtk_widget_add_css_class(_joinButton, "flat");
  gtk_widget_add_css_class(_joinButton, "join-button");
  _signals.clicked(_joinButton, [this] {
    const auto known = _store.chat(_chat);
    const auto group = known ? _store.group(*known) : nullptr;
    if (group && !group->member) {
      _store.client().send(api::make_object<api::joinChat>(_chat));
    } else {
      if (known) _store.setMuted(_chat, !known->muted);
    }
  });
  gtk_stack_add_named(GTK_STACK(_composerStack), _joinButton, "join");
  gtk_box_append(GTK_BOX(composer), _composerStack);
  adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(_root), composer);
  // With an empty field the send button records a voice message while it is held.
  _recordBar = std::make_unique<VoiceRecordBar>(*this, composer, _composerStack, _sendButton, _toBottom);
  refreshSendButton();
}

void ChatView::refreshComposerState() {
  const auto known = _store.chat(_chat);
  if (!known) return;
  const auto group = _store.group(*known);
  const auto broadcast = known->kind == Td::ChatKind::Channel;
  auto closed = false;
  if (group && !group->member) {
    gtk_button_set_label(GTK_BUTTON(_joinButton),
                         Tr(broadcast ? "lng_profile_join_channel" : "lng_profile_join_group").c_str());
    gtk_stack_set_visible_child_name(GTK_STACK(_composerStack), "join");
  } else if (!_store.canWrite(*known)) {
    gtk_button_set_label(GTK_BUTTON(_joinButton), Tr(known->muted ? "lng_channel_unmute" : "lng_channel_mute").c_str());
    gtk_stack_set_visible_child_name(GTK_STACK(_composerStack), "join");
  } else if (const auto topic = _topic ? _store.topic(_chat, _topic) : nullptr;
             topic && topic->closed && !(group && group->manageTopics)) {
    // Only those who manage topics write in a closed one.
    closed = true;
    gtk_button_set_label(GTK_BUTTON(_joinButton), Tr("lng_forum_topic_closed").c_str());
    gtk_stack_set_visible_child_name(GTK_STACK(_composerStack), "join");
  } else {
    gtk_stack_set_visible_child_name(GTK_STACK(_composerStack), "field");
  }
  gtk_widget_set_sensitive(_joinButton, !closed);
  if (_recordBar) {
    _recordBar->composerShown(g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(_composerStack)), "field") == 0);
  }
}

std::string ChatView::composerText() const {
  auto *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(_input));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  auto *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  auto result = std::string(text);
  g_free(text);
  return result;
}

void ChatView::setComposerText(const std::string &text) {
  auto *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(_input));
  _settingComposerText = true;
  gtk_text_buffer_set_text(buffer, text.data(), int(text.size()));
  _settingComposerText = false;
}

void ChatView::refreshSendButton() {
  const auto empty = gtk_text_buffer_get_char_count(gtk_text_view_get_buffer(GTK_TEXT_VIEW(_input))) == 0;
  // Messages to forward go without a comment too.
  const auto forward = !_editing && forwarding();
  gtk_widget_set_sensitive(_sendButton, (!empty || forward) && !_editPending);
  if (_recordBar) _recordBar->setFieldEmpty(empty && !forward && !_editing);
}

void ChatView::composerChanged() {
  refreshSendButton();
  if (_settingComposerText) return;
  _draftTimer.callOnce(kDraftDelay);
  // Telegram shows "typing" for a few seconds from one request.
  const auto now = g_get_monotonic_time();
  if (composerText().empty() || now - _lastTyping < kTypingPeriod) return;
  _lastTyping = now;
  _store.client().send(api::make_object<api::sendChatAction>(_chat, topicObject(), std::string(),
                                                             api::make_object<api::chatActionTyping>()));
}

// The text being written goes to Telegram, and with it to the other devices. An edit is no draft.
void ChatView::saveDraft() {
  _draftTimer.cancel();
  if (_editing) return;
  const auto text = composerText();
  if (text == _savedDraft && _replyToId == _savedDraftReply) return;
  _savedDraft = text;
  _savedDraftReply = _replyToId;
  auto draft = api::object_ptr<api::draftMessage>();
  if (!text.empty() || _replyToId) {
    draft = api::make_object<api::draftMessage>(
        ReplyTo(_replyToId), 0, api::make_object<api::draftMessageContentText>(Formatted(text), nullptr), 0, nullptr);
  }
  _store.client().send(api::make_object<api::setChatDraftMessage>(_chat, topicObject(), std::move(draft)));
}

// Written on another device, while this field has nothing of its own.
void ChatView::draftChanged() {
  const auto known = readState();
  if (!known || known->draft == _savedDraft || _editing || _draftTimer.active()) return;
  if (composerText() != _savedDraft) return;
  _savedDraft = known->draft;
  _savedDraftReply = known->draftReplyTo;
  setComposerText(known->draft);
}

void ChatView::showReplyBar(const std::string &title, const std::string &text, const char *icon) {
  gtk_image_set_from_icon_name(GTK_IMAGE(_replyIcon), icon);
  gtk_label_set_text(GTK_LABEL(_replyTitle), title.c_str());
  gtk_label_set_text(GTK_LABEL(_replyText), Td::OneLine(text).c_str());
  gtk_widget_set_visible(_replyBar, TRUE);
  gtk_widget_grab_focus(_input);
}

void ChatView::replyTo(const MessageItem &item) {
  if (item.local() || item.sponsored) return;
  if (_editing) cancelReplyOrEdit();
  if (forwarding()) cancelForward();
  _replyToId = item.id();
  auto name = std::string();
  auto color = 0;
  DescribeSender(_store, *item.data, name, color);
  showReplyBar(name, Td::MessagePreview(*item.data, _store), "mail-reply-sender-symbolic");
  _draftTimer.callOnce(kDraftDelay);
}

void ChatView::edit(const MessageItem &item) {
  if (_editPending) return;
  if (!_editing) {
    _beforeEditText = composerText();
    _beforeEditReplyTo = _replyToId;
  }
  _draftTimer.cancel();
  _replyToId = 0;
  _editing = item.id();
  const auto text = PlainText(*item.data);
  setComposerText(text);
  showReplyBar(Tr("lng_edit_message"), text, "document-edit-symbolic");
}

// The up arrow in an empty field: the last message that can still be edited.
void ChatView::editLast() {
  for (auto i = _items.rbegin(); i != _items.rend(); ++i) {
    const auto &item = **i;
    if (item.local() || !item.data->is_outgoing_ || PlainText(*item.data).empty()) continue;
    const auto id = item.id();
    _store.client().request<api::messageProperties>(
        api::make_object<api::getMessageProperties>(_chat, id), _lifetime,
        [this, id](api::messageProperties &properties) {
          if (const auto found = find(id); found && properties.can_be_edited_) edit(*found);
        },
        [](api::error &) {});
    return;
  }
}

void ChatView::cancelReplyOrEdit() {
  const auto wasEditing = _editing != 0;
  _replyToId = 0;
  _editing = 0;
  _editPending = false;
  gtk_text_view_set_editable(GTK_TEXT_VIEW(_input), TRUE);
  gtk_widget_set_visible(_replyBar, FALSE);
  _forwardShown = false;
  if (wasEditing) {
    setComposerText(std::exchange(_beforeEditText, std::string()));
    if (const auto reply = std::exchange(_beforeEditReplyTo, 0)) {
      if (const auto item = find(reply)) replyTo(*item);
    }
  } else {
    _draftTimer.callOnce(kDraftDelay);
  }
  // The messages to forward come back after an edit.
  if (!_replyToId) refreshForwardBar();
  refreshSendButton();
}

bool ChatView::handleEscape() {
  if (_recordBar && _recordBar->cancel()) return true;
  if (_editing || _replyToId) {
    cancelReplyOrEdit();
    return true;
  }
  if (forwarding()) {
    cancelForward();
    return true;
  }
  return false;
}

void ChatView::applyEdit(const std::string &text) {
  const auto item = find(_editing);
  if (!item) {
    cancelReplyOrEdit();
    return;
  }
  _editPending = true;
  refreshSendButton();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(_input), FALSE);
  const auto editing = _editing;
  const auto finished = [this, editing] {
    if (_editing == editing) cancelReplyOrEdit();
  };
  const auto failed = [this, editing, finished](api::error &error) {
    if (_editing != editing) return;
    if (error.message_ == "MESSAGE_NOT_MODIFIED") {
      finished();
      return;
    }
    _editPending = false;
    gtk_text_view_set_editable(GTK_TEXT_VIEW(_input), TRUE);
    refreshSendButton();
    ShowToast(error.message_);
  };
  auto function = Td::FunctionPtr();
  if (item->data->content_->get_id() == api::messageText::ID) {
    function = api::make_object<api::editMessageText>(
        _chat, editing, nullptr, api::make_object<api::inputMessageText>(Formatted(text), nullptr, false));
  } else {
    function = api::make_object<api::editMessageCaption>(_chat, editing, nullptr, Formatted(text), false);
  }
  _store.client().request<api::message>(std::move(function), _lifetime,
                                        [finished](api::message &) { finished(); }, failed);
}

void ChatView::send() {
  if (_editPending) return;
  const auto known = _store.chat(_chat);
  if (!known || !_store.canWrite(*known)) return;
  auto text = composerText();
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    // Only the messages to forward.
    if (!_editing && forwarding()) {
      sendForward();
      if (_reachedBottom) scrollToBottom();
      else reload(0);
    }
    return;
  }
  text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
  if (_editing) {
    applyEdit(text);
    return;
  }
  // The comment goes first, then the messages to forward.
  _store.client().send(api::make_object<api::sendMessage>(_chat, topicObject(), ReplyTo(_replyToId), nullptr, nullptr,
                                                          InputText(text)));
  sendForward();
  setComposerText(std::string());
  _draftTimer.cancel();
  // clear_draft has done it on the server.
  _savedDraft.clear();
  _savedDraftReply = 0;
  cancelReplyOrEdit();
  _draftTimer.cancel();
  if (_reachedBottom) scrollToBottom();
  else reload(0);
}

void ChatView::sendText(const std::string &text) {
  const auto known = _store.chat(_chat);
  if (!known || !_store.canWrite(*known) || text.empty()) return;
  _store.client().send(api::make_object<api::sendMessage>(_chat, topicObject(), nullptr, nullptr, nullptr, InputText(text)));
  if (_reachedBottom) scrollToBottom();
  else reload(0);
}

// --- the menu of a message -----------------------------------------------------

void ChatView::showMessageMenu(const MessageItem &item, GtkWidget *widget, double x, double y) {
  // A sponsored message has no menu, as in 0.3.1.
  if (item.sponsored) return;
  const auto id = item.id();
  // What may be done with a message is TDLib's to say. A message being sent has nothing to ask about.
  // The reactions possible for it come with them, asked at the same time.
  struct Asked {
    int left = 2;
    bool canEdit = false, canForward = false, canPin = false, forAll = false, onlyForSelf = false;
    std::shared_ptr<api::availableReactions> reactions;
  };
  const auto asked = std::make_shared<Asked>();
  const auto show = [this, id, widget, x, y, asked] {
    const auto [left, canEdit, canForward, canPin, forAll, onlyForSelf, reactions] = *asked;
    const auto found = find(id);
    if (!found || !gtk_widget_get_mapped(widget)) return;
    const auto known = _store.chat(_chat);
    const auto failed = Cast<api::messageSendingStateFailed>(found->data->sending_state_.get());
    const auto service = Td::IsServiceMessage(*found->data->content_);
    const auto text = PlainText(*found->data);
    auto *menu = g_menu_new();
    // The reactions on top, as in the phone apps.
    auto *strip = (service || found->local()) ? nullptr : MakeReactionsStrip(*this, *found->data, reactions, nullptr);
    if (strip) {
      auto *section = g_menu_new();
      auto *custom = g_menu_item_new(nullptr, nullptr);
      g_menu_item_set_attribute(custom, "custom", "s", "reactions");
      g_menu_append_item(section, custom);
      g_object_unref(custom);
      g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
      g_object_unref(section);
    }
    if (failed) g_menu_append(menu, Tr("lng_bot_download_retry").c_str(), "msg.retry");
    if (!found->local() && !service && known && _store.canWrite(*known)) {
      g_menu_append(menu, Tr("lng_context_reply_msg").c_str(), "msg.reply");
    }
    if (!text.empty()) g_menu_append(menu, Tr("lng_context_copy_text").c_str(), "msg.copy");
    if (canForward && !found->local() && !service) {
      g_menu_append(menu, Tr("lng_context_forward_msg").c_str(), "msg.forward");
    }
    if (canPin && !found->local() && !service) {
      g_menu_append(menu, Tr(found->data->is_pinned_ ? "lng_context_unpin_msg" : "lng_context_pin_msg").c_str(),
                    found->data->is_pinned_ ? "msg.unpin" : "msg.pin");
    }
    if (canEdit) g_menu_append(menu, Tr("lng_context_edit_msg").c_str(), "msg.edit");
    if (!found->local() && SavedOf(*found->data).file) {
      g_menu_append(menu, Tr("lng_context_save_file").c_str(), "msg.save");
    }
    if (Live().sharing(_chat, found->id())) {
      g_menu_append(menu, Tr("lng_group_call_screen_share_stop").c_str(), "msg.stop-live");
    }
    if (CanShowWhoReacted(*this, *found->data)) {
      g_menu_append(menu, TrCount("lng_context_seen_reacted", ReactionsCount(*found->data)).c_str(), "msg.reacted");
    }
    if (forAll || onlyForSelf) g_menu_append(menu, Tr("lng_context_delete_msg").c_str(), "msg.delete");
    if (!g_menu_model_get_n_items(G_MENU_MODEL(menu))) {
      g_object_unref(menu);
      return;
    }
    auto *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    g_object_unref(menu);
    if (strip) {
      // Choosing a reaction closes the menu (the popover is destroyed after it is closed).
      auto *frame = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_box_append(GTK_BOX(frame), strip);
      gtk_popover_menu_add_child(GTK_POPOVER_MENU(popover), frame, "reactions");
      for (auto *child = gtk_widget_get_first_child(strip); child; child = gtk_widget_get_next_sibling(child)) {
        if (GTK_IS_BUTTON(child)) {
          Connect<void(GtkButton *)>(child, "clicked", [popover](GtkButton *) { gtk_popover_popdown(GTK_POPOVER(popover)); });
        }
      }
    }
    // One menu at a time; it goes with the row it is on (dropMessageMenu()).
    dropMessageMenu(nullptr);
    gtk_widget_set_parent(popover, widget);
    _messageMenu = popover;
    g_object_add_weak_pointer(G_OBJECT(popover), reinterpret_cast<gpointer *>(&_messageMenu));
    const GdkRectangle rect = {int(x), int(y), 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
    DestroyPopoverOnClose(popover);
    auto *group = g_simple_action_group_new();
    const auto add = [&](const char *name, std::function<void()> callback) {
      auto *action = g_simple_action_new(name, nullptr);
      Connect<void(GSimpleAction *, GVariant *)>(action, "activate",
                                                 [callback](GSimpleAction *, GVariant *) { callback(); });
      g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
      g_object_unref(action);
    };
    add("retry", [this, id] {
      _store.client().send(
          api::make_object<api::resendMessages>(_chat, std::vector<std::int64_t>{id}, nullptr, 0));
    });
    add("reply", [this, id] {
      if (const auto item = find(id)) replyTo(*item);
    });
    add("copy", [text] {
      CopyToClipboard(text);
      ShowToast(Tr("lng_text_copied"));
    });
    add("save", [this, id] { saveFile(id); });
    add("stop-live", [this, id] { Live().stop(_chat, id); });
    add("forward", [this, id] {
      if (const auto item = find(id)) forward(*item);
    });
    add("edit", [this, id] {
      if (const auto item = find(id)) edit(*item);
    });
    add("pin", [this, id] { confirmPin(id); });
    add("unpin", [this, id] { confirmUnpin(id); });
    add("delete", [this, id, forAll, onlyForSelf] { confirmDelete(id, forAll, onlyForSelf); });
    add("reacted", [this, id] {
      if (const auto item = find(id)) ShowWhoReacted(*this, *item->data);
    });
    gtk_widget_insert_action_group(popover, "msg", G_ACTION_GROUP(group));
    g_object_unref(group);
    gtk_popover_popup(GTK_POPOVER(popover));
  };
  if (item.local()) {
    asked->onlyForSelf = true;
    show();
    return;
  }
  const auto answered = [asked, show] {
    if (--asked->left == 0) show();
  };
  _store.client().request<api::messageProperties>(
      api::make_object<api::getMessageProperties>(_chat, id), _lifetime,
      [asked, answered](api::messageProperties &properties) {
        asked->canEdit = properties.can_be_edited_;
        asked->canForward = properties.can_be_forwarded_;
        asked->canPin = properties.can_be_pinned_;
        asked->forAll = properties.can_be_deleted_for_all_users_;
        asked->onlyForSelf = properties.can_be_deleted_only_for_self_;
        answered();
      },
      [answered](api::error &) { answered(); });
  _store.client().request<api::availableReactions>(
      api::make_object<api::getMessageAvailableReactions>(_chat, id, 8), _lifetime,
      [asked, answered](api::availableReactions &available) {
        asked->reactions = std::make_shared<api::availableReactions>(std::move(available));
        answered();
      },
      [answered](api::error &) { answered(); });
}

void ChatView::confirmDelete(std::int64_t id, bool forAll, bool onlyForSelf) {
  auto *dialog = adw_alert_dialog_new(Tr("lng_selected_delete_sure_this").c_str(), nullptr);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", Tr("lng_cancel").c_str(), "delete",
                                 Tr("lng_box_delete").c_str(), nullptr);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  GtkWidget *revoke = nullptr;
  if (forAll && onlyForSelf) {
    revoke = gtk_check_button_new_with_label(Tr("lng_delete_for_everyone_check").c_str());
    gtk_check_button_set_active(GTK_CHECK_BUTTON(revoke), TRUE);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), revoke);
  }
  auto &client = _store.client();
  const auto chat = _chat;
  Connect<void(AdwAlertDialog *, const char *)>(dialog, "response", [=, &client](AdwAlertDialog *, const char *response) {
    if (g_strcmp0(response, "delete") != 0) return;
    // Channels and supergroups: deletion is always for everyone.
    const auto all = revoke ? bool(gtk_check_button_get_active(GTK_CHECK_BUTTON(revoke))) : forAll;
    client.send(api::make_object<api::deleteMessages>(chat, std::vector<std::int64_t>{id}, all));
  });
  adw_dialog_present(ADW_DIALOG(dialog), _window.widget());
}

// --- forwarding ------------------------------------------------------------------

const ForwardDraft *ChatView::forwardDraft() const {
  return _window.forwardDraft(_chat, _topic);
}

void ChatView::forwardChanged() {
  refreshForwardBar();
}

void ChatView::forward(const MessageItem &item) {
  if (item.local()) return;
  auto draft = ForwardDraft();
  draft.from = _chat;
  const auto add = [&](const MessageItem &message) {
    draft.ids.push_back(message.id());
    auto name = ForwardSenderName(_store, *message.data);
    if (std::ranges::find(draft.senders, name) == draft.senders.end()) draft.senders.push_back(std::move(name));
  };
  // An album goes whole.
  if (const auto album = item.data->media_album_id_) {
    for (const auto &other : _items) {
      if (!other->local() && other->data->media_album_id_ == album) add(*other);
    }
  }
  if (draft.ids.empty()) add(item);
  if (draft.ids.size() == 1) draft.preview = Td::MessagePreview(*item.data, _store);
  _window.showForwardPicker(std::move(draft));
}

void ChatView::refreshForwardBar() {
  const auto draft = forwardDraft();
  if (!draft || _editing) {
    // Shown again when the edit is over.
    if (std::exchange(_forwardShown, false) && !_replyToId && !_editing) gtk_widget_set_visible(_replyBar, FALSE);
    refreshSendButton();
    return;
  }
  // One bar: the messages to forward take the place of a reply.
  if (std::exchange(_replyToId, 0)) _draftTimer.callOnce(kDraftDelay);
  const auto count = int(draft->ids.size());
  const auto &senders = draft->senders;
  auto text = std::string();
  if (!draft->names) {
    text = Tr("lng_forward_sender_names_removed");
  } else if (senders.size() > 2) {
    text = TrCount("lng_forwarding_from", std::int64_t(senders.size() - 1), {{"user", senders.front()}});
  } else if (senders.size() == 2) {
    text = Tr("lng_forwarding_from_two", {{"user", senders[0]}, {"second_user", senders[1]}});
  } else if (!senders.empty()) {
    text = senders.front();
  }
  if (count == 1 && !draft->preview.empty()) text = text.empty() ? draft->preview : text + ": " + draft->preview;
  gtk_image_set_from_icon_name(GTK_IMAGE(_replyIcon), "mail-forward-symbolic");
  gtk_label_set_text(GTK_LABEL(_replyTitle),
                     (count == 1 ? Tr("lng_forward_title") : TrCount("lng_forward_many_title", count)).c_str());
  gtk_label_set_text(GTK_LABEL(_replyText), Td::OneLine(text).c_str());
  gtk_widget_set_visible(_replyBar, TRUE);
  _forwardShown = true;
  refreshSendButton();
}

void ChatView::cancelForward() {
  _window.setForwardDraft(_chat, _topic, ForwardDraft());
}

// With or without the senders' names, or to another chat, as the phone apps offer.
void ChatView::showForwardMenu() {
  const auto draft = forwardDraft();
  if (!draft) return;
  const auto many = draft->ids.size() > 1;
  auto *menu = g_menu_new();
  g_menu_append(menu,
                Tr(draft->names ? (many ? "lng_forward_action_hide_senders" : "lng_forward_action_hide_sender")
                                : (many ? "lng_forward_action_show_senders" : "lng_forward_action_show_sender"))
                    .c_str(),
                "fwd.names");
  g_menu_append(menu, Tr("lng_forward_change_recipient").c_str(), "fwd.recipient");
  auto *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  g_object_unref(menu);
  gtk_widget_set_parent(popover, _replyBar);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_TOP);
  DestroyPopoverOnClose(popover);
  auto *group = g_simple_action_group_new();
  const auto add = [&](const char *name, std::function<void()> callback) {
    auto *action = g_simple_action_new(name, nullptr);
    Connect<void(GSimpleAction *, GVariant *)>(action, "activate",
                                               [callback](GSimpleAction *, GVariant *) { callback(); });
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    g_object_unref(action);
  };
  add("names", [this] {
    if (const auto draft = forwardDraft()) setForwardNames(!draft->names);
  });
  add("recipient", [this] {
    const auto draft = forwardDraft();
    if (!draft) return;
    auto moved = *draft;
    cancelForward();
    _window.showForwardPicker(std::move(moved));
  });
  gtk_widget_insert_action_group(popover, "fwd", G_ACTION_GROUP(group));
  g_object_unref(group);
  gtk_popover_popup(GTK_POPOVER(popover));
}

void ChatView::sendForward() {
  const auto draft = forwardDraft();
  if (!draft) return;
  // Without the senders' names they go as copies, with their captions, as Telegram Desktop sends them. The answer
  // may come after the chat is closed: an error is told anyway.
  _store.client().send(
      api::make_object<api::forwardMessages>(_chat, topicObject(), draft->from, std::vector<std::int64_t>(draft->ids),
                                             nullptr, !draft->names, false),
      [](Td::ObjectPtr object) {
        if (object && object->get_id() == api::error::ID) ShowToast(static_cast<api::error &>(*object).message_);
      });
  cancelForward();
}

// --- what the messages ask for -------------------------------------------------

void ChatView::openChat(Td::ChatId chat) {
  _window.openChat(chat);
}

// The row downloads its photo by itself: a tap opens the viewer, which asks for the largest size first.
void ChatView::openPhoto(const MessageItem &item, Td::FileId file) {
  if (const auto photo = PhotoWithFile(item.data ? item.data->content_.get() : nullptr, file)) {
    ShowPhotoViewer(*this, item.id(), *photo);
    return;
  }
  if (const auto known = _store.file(file); known && known->complete) {
    OpenPath(known->path);
    return;
  }
  _openWhenLoaded[file] = true;
  _store.download(file, 32);
}

void ChatView::openDocument(const MessageItem &item, Td::FileId file) {
  const auto known = _store.file(file);
  if (known && known->complete) {
    OpenPath(known->path);
  } else if (known && known->downloading) {
    _store.cancelDownload(file);
    _openWhenLoaded.erase(file);
  } else {
    // Unknown to the store, it may be on disk already: the answer tells (subscribe()).
    _openWhenLoaded[file] = OpensByItself(item.data ? item.data->content_.get() : nullptr);
    _store.download(file, 32);
  }
}

bool ChatView::opensWhenLoaded(Td::FileId file) const {
  const auto i = _openWhenLoaded.find(file);
  return i != _openWhenLoaded.end() && i->second;
}

void ChatView::dropMessageMenu(GtkWidget *row) {
  auto *menu = _messageMenu;
  if (!menu) return;
  auto *parent = gtk_widget_get_parent(menu);
  if (row && parent && parent != row && !gtk_widget_is_ancestor(parent, row)) return;
  // The pointer goes first: the menu may be finalized right here.
  g_object_remove_weak_pointer(G_OBJECT(menu), reinterpret_cast<gpointer *>(&_messageMenu));
  _messageMenu = nullptr;
  gtk_popover_popdown(GTK_POPOVER(menu));
  if (gtk_widget_get_parent(menu)) gtk_widget_unparent(menu);
}

void ChatView::saveFile(std::int64_t id) {
  const auto item = find(id);
  const auto saved = item ? SavedOf(*item->data) : Saved();
  if (!saved.file) return;
  const auto known = _store.file(saved.file->id_);
  auto path = (known && known->complete) ? known->path : std::string();
  if (!known && saved.file->local_ && saved.file->local_->is_downloading_completed_) path = saved.file->local_->path_;
  if (!path.empty()) {
    CopySaved(path, saved.name, saved.photo);
    return;
  }
  // As in 0.3.1: the download starts, and the file is saved when it ends.
  SaveWhenLoaded(_store, saved.file->id_, saved.name, saved.photo);
  _store.download(saved.file->id_, 32);
  ShowToast(Tr("lng_downloads_section"));
}

// --- sponsored messages --------------------------------------------------------

void ChatView::showSponsored() {
  if (_sponsoredAsked || _topic || !_initialLoaded || !_reachedBottom) return;
  const auto known = _store.chat(_chat);
  if (!known) return;
  auto canHave = known->kind == Td::ChatKind::Channel;
  if (known->kind == Td::ChatKind::Private) {
    const auto user = _store.user(known->peerId);
    canHave = user && user->bot;
  }
  if (!canHave) return;
  // Once a chat opening: TDLib keeps the answer for a few minutes itself.
  _sponsoredAsked = true;
  _store.client().request<api::sponsoredMessages>(
      api::make_object<api::getChatSponsoredMessages>(_chat), _lifetime,
      [this](api::sponsoredMessages &found) {
        // One, after the newest message, as 0.3.1 showed them.
        if (found.messages_.empty() || !found.messages_.front() || !found.messages_.front()->content_) return;
        auto sponsored = std::move(found.messages_.front());
        auto item = std::make_unique<MessageItem>();
        item->data = api::make_object<api::message>();
        item->data->id_ = sponsored->message_id_;
        item->data->chat_id_ = _chat;
        item->data->content_ = std::move(sponsored->content_);
        item->sponsored = std::move(sponsored);
        for (const auto file : SponsoredFiles(item->data->content_.get())) {
          const auto known = _store.file(file->id_);
          const auto complete = known ? known->complete
                                      : (file->local_ && file->local_->is_downloading_completed_);
          if (complete) continue;
          _sponsoredWaiting.insert(file->id_);
          _store.download(file->id_, 1);
        }
        _sponsored = std::move(item);
        if (_sponsoredWaiting.empty()) placeSponsored();
      },
      [](api::error &) {});
}

void ChatView::placeSponsored() {
  const auto newest = find(newestId());
  if (!_sponsored || _sponsoredPlaced || !newest) return;
  _sponsored->sponsoredAfter = newest->id();
  _sponsored->data->date_ = newest->data->date_;  // No date line of its own.
  _sponsoredPlaced = true;
  sync();
}

// Seen once the whole message is on the screen (or fills it, if it is taller), as 0.3.1 counted it.
void ChatView::reportSponsoredView() {
  if (!_sponsoredPlaced || _sponsoredViewed) return;
  const auto row = rowFor(_sponsored.get());
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  double y = 0;
  const auto height = row ? gtk_widget_get_height(row->row()) : 0;
  if (height <= 0 || !WidgetViewportY(row->row(), _scroll, y)) return;
  if (!((y >= 0 && y + height <= page) || (y <= 0 && y + height >= page))) return;
  _sponsoredViewed = true;
  _store.client().send(api::make_object<api::viewMessages>(_chat, std::vector<std::int64_t>{_sponsored->id()},
                                                           api::make_object<api::messageSourceChatHistory>(), false));
}

void ChatView::sponsoredClicked(const MessageItem &item, bool media) {
  if (!item.sponsored) return;
  ++_sponsoredClicks;
  _store.client().send(api::make_object<api::clickChatSponsoredMessage>(_chat, item.id(), media, false));
}

// --- debug ---------------------------------------------------------------------

void ChatView::debugState() const {
  const auto value = gtk_adjustment_get_value(_adjustment);
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  const auto upper = gtk_adjustment_get_upper(_adjustment);
  const auto oldest = oldestId(), newest = newestId();
  // Which topics the held messages are in: in a topic, only that one.
  auto topics = std::set<std::int32_t>();
  for (const auto &item : _items) topics.insert(Td::MessageTopic(*item->data));
  auto topicList = std::string();
  for (const auto topic : topics) topicList += (topicList.empty() ? "" : ",") + std::to_string(topic);
  auto newestText = std::string();
  if (!_items.empty()) newestText = Td::OneLine(PlainText(*_items.back()->data), 60);
  g_message("chat topic=%d topics=%s newestText=\"%s\" id=%lld", int(_topic), topicList.c_str(), newestText.c_str(),
            (long long)_chat);
  // The last messages, and whose they were when forwarded.
  auto last = std::string();
  for (auto i = _items.size() > 4 ? _items.size() - 4 : 0; i < _items.size(); ++i) {
    const auto &message = *_items[i]->data;
    last += " [\"" + Td::OneLine(PlainText(message), 40) + "\"";
    if (message.forward_info_) last += " from=\"" + ForwardSenderName(_store, message) + "\"";
    last += "]";
  }
  const auto draft = forwardDraft();
  g_message("chat forward: waiting=%zu names=%d bar=%d sendable=%d title=\"%s\" text=\"%s\" last:%s",
            draft ? draft->ids.size() : 0, draft ? int(draft->names) : 1, int(_forwardShown),
            int(gtk_widget_get_sensitive(_sendButton)), gtk_label_get_text(GTK_LABEL(_replyTitle)),
            gtk_label_get_text(GTK_LABEL(_replyText)), last.c_str());
  g_message("chat state: messages=%zu rows=%zu nearRows=%zu oldest=%lld newest=%lld top=%d bottom=%d stick=%d reserve=%d value=%.0f "
            "page=%.0f upper=%.0f toBottom=%d replyTo=%lld editing=%lld draft=\"%s\"",
            _items.size(), _rows.size(), _nearRows.size(), (long long)(oldest >> 20), (long long)(newest >> 20), int(_reachedTop),
            int(_reachedBottom), int(_stickToBottom), _topReserve, value, page, upper,
            int(gtk_widget_get_visible(_toBottom)), (long long)(_replyToId >> 20), (long long)(_editing >> 20),
            _savedDraft.c_str());
}

void ChatView::debugScroll(const std::string &where) {
  _scrollTarget = 0;
  _stickToBottom = false;
  const auto page = gtk_adjustment_get_page_size(_adjustment);
  if (where == "top") {
    gtk_adjustment_set_value(_adjustment, 0);
  } else if (where == "bottom") {
    gtk_adjustment_set_value(_adjustment, gtk_adjustment_get_upper(_adjustment) - page);
  } else {
    gtk_adjustment_set_value(_adjustment, gtk_adjustment_get_value(_adjustment) + std::atof(where.c_str()));
  }
}

void ChatView::debugSend(const std::string &text) {
  setComposerText(text);
  send();
}

void ChatView::debugType(const std::string &text) {
  setComposerText(text);
  refreshSendButton();
}

void ChatView::debugReply() {
  if (const auto newest = find(newestId())) replyTo(*newest);
}

void ChatView::debugEdit(const std::string &text) {
  for (auto i = _items.rbegin(); i != _items.rend(); ++i) {
    if ((*i)->data->is_outgoing_ && !(*i)->local()) {
      edit(**i);
      debugSend(text);
      return;
    }
  }
}

void ChatView::debugForward() {
  for (auto i = _items.rbegin(); i != _items.rend(); ++i) {
    if ((*i)->local()) continue;
    const auto id = (*i)->id();
    // As the menu does: only a message that can be forwarded.
    _store.client().request<api::messageProperties>(
        api::make_object<api::getMessageProperties>(_chat, id), _lifetime,
        [this, id](api::messageProperties &properties) {
          const auto found = find(id);
          if (found && properties.can_be_forwarded_) forward(*found);
          else g_message("chat forward: message %lld can't be forwarded", (long long)(id >> 20));
        },
        [](api::error &) {});
    return;
  }
}

void ChatView::debugForwardMode(bool names) {
  setForwardNames(names);
}

void ChatView::setForwardNames(bool names) {
  const auto draft = forwardDraft();
  if (!draft || draft->names == names) return;
  auto changed = *draft;
  changed.names = names;
  _window.setForwardDraft(_chat, _topic, std::move(changed));
}

void ChatView::debugForwardCancel() {
  cancelForward();
}

void ChatView::debugForwardMenu() {
  showForwardMenu();
}

void ChatView::debugEditLast() {
  editLast();
}

void ChatView::debugDelete() {
  if (const auto newest = newestId()) {
    _store.client().send(api::make_object<api::deleteMessages>(_chat, std::vector<std::int64_t>{newest}, true));
  }
}

// The message of this server number among the loaded ones, the newest without one.
void ChatView::debugMessageMenu(std::int64_t number) {
  const auto item = number ? debugItem(number) : debugItem(0);
  const auto row = item ? rowFor(item) : nullptr;
  if (!row) {
    g_message("chat menu: message %lld isn't loaded", (long long)number);
    return;
  }
  // As a finger does it: on the screen. GTK closes a menu whose message is off it.
  double y = 0;
  const auto height = gtk_widget_get_height(row->row());
  if (WidgetViewportY(row->row(), _scroll, y) && y + height > 0 && y < gtk_adjustment_get_page_size(_adjustment)) {
    showMessageMenu(*item, row->widgetFor(item), 10, 10);
    return;
  }
  scrollToItem(item, false);
  struct Later {
    ChatView *self;
    std::weak_ptr<void> token;
    std::int64_t id;
  };
  g_timeout_add_once(400, +[](gpointer data) {
    const auto later = std::unique_ptr<Later>(static_cast<Later *>(data));
    if (later->token.expired()) return;
    const auto item = later->self->find(later->id);
    if (const auto row = item ? later->self->rowFor(item) : nullptr) {
      later->self->showMessageMenu(*item, row->widgetFor(item), 10, 10);
    }
  }, new Later{this, _lifetime.token(), item->id()});
}

void ChatView::debugPin(std::int64_t number, bool pin) {
  const auto id = number ? (number << 20) : newestId();
  if (!id) return;
  // As the menu does: only a message that can be pinned.
  _store.client().request<api::messageProperties>(
      api::make_object<api::getMessageProperties>(_chat, id), _lifetime,
      [this, id, pin](api::messageProperties &properties) {
        if (!properties.can_be_pinned_) g_message("chat pinned: message %lld can't be pinned", (long long)(id >> 20));
        else if (pin) confirmPin(id);
        else confirmUnpin(id);
      },
      [](api::error &) {});
}

void ChatView::debugPinnedBar(const std::string &what) {
  if (what == "tap") {
    pinnedBarClicked();
  } else if (what == "close") {
    pinnedCloseClicked();
  } else if (what == "list") {
    showPinnedList();
  } else {
    auto ids = std::string();
    for (const auto id : _pinnedIds) ids += (ids.empty() ? "" : ",") + std::to_string(id >> 20);
    // The picture's own size: the bar's square for a decoded file, the blurred one's for that.
    auto *paintable = gtk_image_get_paintable(GTK_IMAGE(_pinnedPreview));
    const auto pictureWidth = paintable ? gdk_paintable_get_intrinsic_width(paintable) : 0;
    const auto pictureHeight = paintable ? gdk_paintable_get_intrinsic_height(paintable) : 0;
    g_message("chat pinned: loaded=%d ids=[%s] total=%d current=%lld clicked=%lld shown=%d hidden=%d canPin=%d "
              "title=\"%s\" text=\"%s\" preview=%d %dx%d picture=%dx%d bar=%dx%d list=%d close=%d",
              int(_pinnedLoaded), ids.c_str(), _pinnedTotal, (long long)(_pinnedCurrent >> 20),
              (long long)(_pinnedClicked >> 20), int(gtk_revealer_get_reveal_child(GTK_REVEALER(_pinnedBar))),
              int(pinnedHiddenByUser()), int(canPinMessages()), gtk_label_get_text(GTK_LABEL(_pinnedTitle)),
              gtk_label_get_text(GTK_LABEL(_pinnedText)), int(gtk_widget_get_visible(_pinnedPreview)),
              gtk_widget_get_width(_pinnedPreview), gtk_widget_get_height(_pinnedPreview), pictureWidth, pictureHeight,
              gtk_widget_get_width(_pinnedBar), gtk_widget_get_height(_pinnedBar),
              int(gtk_widget_get_visible(_pinnedListButton)), int(gtk_widget_get_visible(_pinnedClose)));
  }
}

void ChatView::debugPinnedList(const std::string &what) {
  if (!_pinnedDialog) {
    g_message("pinned list: none");
    return;
  }
  auto *list = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(_pinnedDialog), "contrail-list"));
  auto *action = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(_pinnedDialog), "contrail-action"));
  if (what.starts_with("pick ")) {
    if (auto *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), std::atoi(what.c_str() + 5))) {
      g_signal_emit_by_name(list, "row-activated", row);
    }
  } else if (what == "action") {
    gtk_widget_activate(action);
  } else {
    auto rows = std::string();
    auto count = 0;
    for (auto *row = gtk_widget_get_first_child(list); row; row = gtk_widget_get_next_sibling(row), ++count) {
      if (!ADW_IS_ACTION_ROW(row)) continue;
      rows += std::string(" [\"") + adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row)) + "\" \""
          + adw_action_row_get_subtitle(ADW_ACTION_ROW(row)) + "\"]";
    }
    auto *title = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(_pinnedDialog), "contrail-title"));
    g_message("pinned list: title=\"%s\" rows=%d action=\"%s\"%s",
              adw_window_title_get_title(ADW_WINDOW_TITLE(title)), count, gtk_button_get_label(GTK_BUTTON(action)),
              rows.c_str());
  }
}

// --- reactions (debug) -------------------------------------------------------------

namespace {

// The buttons of reactions under `widget`: chips under a message, choices in a menu or a dialog.
void CollectReactionButtons(GtkWidget *widget, const char *cssClass, std::vector<GtkWidget *> &out) {
  if (GTK_IS_BUTTON(widget) && gtk_widget_has_css_class(widget, cssClass)
      && g_object_get_data(G_OBJECT(widget), "contrail-reaction")) {
    out.push_back(widget);
  }
  for (auto *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    CollectReactionButtons(child, cssClass, out);
  }
}

// What a chip or a choice shows: its emoji as text, or the size of its picture, and its number or avatars.
std::string DescribeReactionButton(GtkWidget *button) {
  auto result = std::string(static_cast<const char *>(g_object_get_data(G_OBJECT(button), "contrail-reaction")));
  if (gtk_widget_has_css_class(button, "chosen")) result += '*';
  const std::function<void(GtkWidget *)> walk = [&](GtkWidget *widget) {
    if (GTK_IS_PICTURE(widget) && gtk_widget_get_visible(widget)) {
      auto *paintable = gtk_picture_get_paintable(GTK_PICTURE(widget));
      result += " picture=" + (paintable ? std::to_string(gdk_paintable_get_intrinsic_width(paintable)) + "px@"
                                               + std::to_string(gtk_widget_get_width(widget)) + "x"
                                               + std::to_string(gtk_widget_get_height(widget))
                                         : std::string("none"));
    } else if (GTK_IS_LABEL(widget) && gtk_widget_has_css_class(widget, "reaction-count")) {
      result += std::string(" count=") + gtk_label_get_text(GTK_LABEL(widget));
    } else if (GTK_IS_LABEL(widget) && gtk_widget_has_css_class(widget, "reaction-custom-alt")) {
      result += std::string(" alt=") + gtk_label_get_text(GTK_LABEL(widget));
    } else if (ADW_IS_AVATAR(widget)) {
      result += " avatar";
      return;
    }
    for (auto *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
      walk(child);
    }
  };
  walk(button);
  return "[" + result + "]";
}

}  // namespace

void ChatView::debugReact(const std::string &key, std::int64_t number) {
  const auto item = number ? find(number << 20) : find(newestId());
  if (!item) {
    g_message("chat reactions: message %lld isn't loaded", (long long)number);
    return;
  }
  // A tap on its chip when it has one, else as the menu does.
  if (const auto row = rowFor(item)) {
    auto chips = std::vector<GtkWidget *>();
    CollectReactionButtons(row->widgetFor(item), "reaction-chip", chips);
    for (auto *chip : chips) {
      if (key == static_cast<const char *>(g_object_get_data(G_OBJECT(chip), "contrail-reaction"))) {
        g_signal_emit_by_name(chip, "clicked");
        return;
      }
    }
  }
  ToggleReaction(*this, *item->data, key);
}

void ChatView::debugUnreact(std::int64_t number) {
  const auto item = number ? find(number << 20) : find(newestId());
  const auto info = item ? item->data->interaction_info_.get() : nullptr;
  if (!info || !info->reactions_) return;
  auto chosen = std::vector<std::string>();
  for (const auto &reaction : info->reactions_->reactions_) {
    if (reaction->is_chosen_) chosen.push_back(ReactionKey(reaction->type_.get()));
  }
  for (const auto &key : chosen) {
    if (const auto message = heldMessage(item->id())) ToggleReaction(*this, *message, key);
  }
}

void ChatView::debugReacted(std::int64_t number) {
  const auto item = number ? find(number << 20) : find(newestId());
  if (!item || !CanShowWhoReacted(*this, *item->data)) {
    g_message("chat reactions: who reacted to %lld isn't told", (long long)number);
    return;
  }
  ShowWhoReacted(*this, *item->data);
}

void ChatView::debugReactionsMore(std::int64_t number) {
  const auto item = number ? find(number << 20) : find(newestId());
  const auto row = item ? rowFor(item) : nullptr;
  if (!row) return;
  const std::function<GtkWidget *(GtkWidget *)> more = [&](GtkWidget *widget) -> GtkWidget * {
    if (GTK_IS_BUTTON(widget) && gtk_widget_has_css_class(widget, "reactions-more")) return widget;
    for (auto *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
      if (auto *found = more(child)) return found;
    }
    return nullptr;
  };
  for (auto *child = gtk_widget_get_first_child(row->widgetFor(item)); child; child = gtk_widget_get_next_sibling(child)) {
    if (!GTK_IS_POPOVER(child) || !gtk_widget_get_visible(child)) continue;
    if (auto *button = more(child)) {
      g_signal_emit_by_name(button, "clicked");
      return;
    }
  }
  g_message("chat reactions: no menu with all the reactions");
}

void ChatView::debugReactQuick(std::int64_t number) {
  if (const auto item = number ? find(number << 20) : find(newestId())) ToggleQuickReaction(*this, *item->data);
}

void ChatView::debugReactionsState(std::int64_t number) {
  const auto item = number ? find(number << 20) : find(newestId());
  auto reactions = std::string();
  auto chips = std::string();
  auto unread = 0;
  auto canList = 0;
  if (item) {
    unread = int(item->data->unread_reactions_.size());
    canList = int(CanShowWhoReacted(*this, *item->data));
    if (const auto info = item->data->interaction_info_.get(); info && info->reactions_) {
      for (const auto &reaction : info->reactions_->reactions_) {
        reactions += " " + ReactionKey(reaction->type_.get()) + "=" + std::to_string(reaction->total_count_)
                     + (reaction->is_chosen_ ? "*" : "");
      }
    }
    if (const auto row = rowFor(item)) {
      auto buttons = std::vector<GtkWidget *>();
      CollectReactionButtons(row->widgetFor(item), "reaction-chip", buttons);
      for (auto *button : buttons) chips += DescribeReactionButton(button);
    }
  }
  const auto known = _store.chat(_chat);
  g_message("chat reactions: message=%lld [%s ] chips=%s who=%d unread=%d chatUnread=%d quick=%s rows=%zu builds=%llu "
            "rebuilds=%llu bars=%llu updates=%llu %s",
            (long long)(item ? item->id() >> 20 : 0), reactions.c_str(), chips.c_str(), canList, unread,
            known ? known->unreadReactions : -1, ReactionKey(_store.defaultReaction()).c_str(), _rows.size(),
            (unsigned long long)MessageWidget::BuildCount(), (unsigned long long)MessageWidget::RebuildCount(),
            (unsigned long long)MessageWidget::ReactionBarCount(), (unsigned long long)_interactionUpdates,
            CustomReactionsState().c_str());
  // The dialogs open: who reacted, all the reactions; and the menu's strip.
  for (auto *dialog : _dialogs) {
    if (auto *list = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(dialog), "contrail-who-reacted"))) {
      auto rows = std::string();
      for (auto *row = gtk_widget_get_first_child(list); row; row = gtk_widget_get_next_sibling(row)) {
        if (ADW_IS_ACTION_ROW(row)) rows += std::string(" \"") + adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row)) + "\"";
      }
      g_message("chat reactions who: title=\"%s\" rows:%s", adw_dialog_get_title(ADW_DIALOG(dialog)), rows.c_str());
    }
    if (auto *grid = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(dialog), "contrail-reactions-grid"))) {
      auto buttons = std::vector<GtkWidget *>();
      CollectReactionButtons(grid, "reaction-choice", buttons);
      auto all = std::string();
      for (auto *button : buttons) all += DescribeReactionButton(button);
      g_message("chat reactions all: %zu %s", buttons.size(), all.c_str());
    }
  }
  if (const auto row = item ? rowFor(item) : nullptr) {
    // The menu's popover hangs on the message's widget.
    auto buttons = std::vector<GtkWidget *>();
    for (auto *child = gtk_widget_get_first_child(row->widgetFor(item)); child; child = gtk_widget_get_next_sibling(child)) {
      if (GTK_IS_POPOVER(child) && gtk_widget_get_visible(child)) CollectReactionButtons(child, "reaction-choice", buttons);
    }
    if (!buttons.empty()) {
      auto strip = std::string();
      for (auto *button : buttons) strip += DescribeReactionButton(button);
      g_message("chat reactions menu: %zu %s", buttons.size(), strip.c_str());
    }
  }
}

void ChatView::debugMenuState() const {
  auto *model = _messageMenu ? gtk_popover_menu_get_menu_model(GTK_POPOVER_MENU(_messageMenu)) : nullptr;
  auto items = std::string();
  if (model) DescribeMenu(model, items);
  g_message("chat menu: open=%d items:%s", int(_messageMenu && gtk_widget_get_visible(_messageMenu)), items.c_str());
}

void ChatView::debugMenuDo(const std::string &action) {
  if (!_messageMenu) {
    g_message("chat menu: none open");
    return;
  }
  // As a tap on the item: the action, then the menu closes.
  auto *menu = _messageMenu;
  g_object_ref(menu);
  gtk_widget_activate_action(menu, ("msg." + action).c_str(), nullptr);
  gtk_popover_popdown(GTK_POPOVER(menu));
  g_object_unref(menu);
}

// Of this server number, 0: the newest (being sent too), -1: the sponsored message.
MessageItem *ChatView::debugItem(std::int64_t number) const {
  if (number < 0) return _sponsoredPlaced ? _sponsored.get() : nullptr;
  if (number == 0) return _items.empty() ? nullptr : _items.back().get();
  for (const auto &item : _items) {
    if ((item->id() >> 20) == number) return item.get();
  }
  return nullptr;
}

void ChatView::debugMedia(std::int64_t number) {
  const auto item = debugItem(number);
  const auto row = item ? rowFor(item) : nullptr;
  if (!row) {
    g_message("chat media: message %lld isn't shown", (long long)number);
    return;
  }
  double y = 0;
  const auto shown = WidgetViewportY(row->row(), _scroll, y);
  g_message("chat media: message=%lld local=%d failed=%d y=%.0f %s", (long long)(item->id() >> 20), int(item->local()),
            int(item->data->sending_state_ && item->data->sending_state_->get_id() == api::messageSendingStateFailed::ID),
            shown ? y : -1., row->debugDescribe().c_str());
}

void ChatView::debugTap(std::int64_t number, int index) {
  const auto item = debugItem(number);
  if (const auto row = item ? rowFor(item) : nullptr) row->debugTap(index);
  else g_message("chat tap: message %lld isn't shown", (long long)number);
}

void ChatView::debugTapText(std::int64_t number, const std::string &part) {
  const auto item = debugItem(number);
  const auto row = item ? rowFor(item) : nullptr;
  if (!row || !row->debugTapText(part)) g_message("chat tap: no link with \"%s\" in message %lld", part.c_str(), (long long)number);
}

void ChatView::debugSponsored() const {
  const auto row = _sponsoredPlaced ? rowFor(_sponsored.get()) : nullptr;
  g_message("chat sponsored: asked=%d received=%d waiting=%zu placed=%d after=%lld shown=%d viewed=%d clicks=%d",
            int(_sponsoredAsked), int(_sponsored != nullptr), _sponsoredWaiting.size(), int(_sponsoredPlaced),
            (long long)(_sponsored ? _sponsored->sponsoredAfter >> 20 : 0), int(row != nullptr), int(_sponsoredViewed),
            _sponsoredClicks);
}

// --- locations ---------------------------------------------------------------

void ChatView::chooseLocation() {
  const auto known = _store.chat(_chat);
  if (!known || !_store.canWrite(*known)) return;
  ShowLocationPicker(_window, {.chat = _chat,
                               .topic = _topic,
                               .replyTo = _replyToId,
                               .send = [this, token = _lifetime.token()](
                                           api::object_ptr<api::InputMessageContent> content) {
                                 if (!token.expired()) sendContent(std::move(content));
                               }});
}

void ChatView::sendContent(api::object_ptr<api::InputMessageContent> content) {
  _store.client().send(api::make_object<api::sendMessage>(_chat, topicObject(), ReplyTo(_replyToId), nullptr, nullptr,
                                                          std::move(content)));
  if (_replyToId) cancelReplyOrEdit();
  if (_reachedBottom) scrollToBottom();
  else reload(0);
}

// --- files sent here ---

void ChatView::chooseFiles() {
  auto *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, Tr("lng_attach").c_str());
  // The chat may close while the dialog is open: then nothing is sent.
  struct Context {
    std::weak_ptr<void> alive;
    ChatView *self = nullptr;
  };
  gtk_file_dialog_open_multiple(
      dialog, GTK_WINDOW(_window.widget()), nullptr,
      +[](GObject *source, GAsyncResult *result, gpointer data) {
        auto *context = static_cast<Context *>(data);
        if (auto *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source), result, nullptr)) {
          GSList *list = nullptr;
          for (auto i = g_list_model_get_n_items(files); i != 0; --i) {
            list = g_slist_prepend(list, g_list_model_get_item(files, i - 1));
          }
          const auto split = SplitPastedFiles(list);
          g_slist_free_full(list, g_object_unref);
          g_object_unref(files);
          if (!context->alive.expired() && !split.local.empty()) context->self->openAttach(split.local);
        }
        delete context;
      },
      new Context{_lifetime.token(), this});
  g_object_unref(dialog);
}

void ChatView::paste() {
  auto *clipboard = gtk_widget_get_clipboard(_input);
  const auto kind = ClassifyPaste(gdk_clipboard_get_formats(clipboard));
  // A new paste replaces one still being read; the reader dies with this chat, and its answer with it.
  _pasteReader = std::make_unique<ClipboardReader>(clipboard, kind, [this](ClipboardReader::Result &&result) {
    _pasteReader = nullptr;
    if (!result.files.remote.empty()) {
      // Links to remote files are text.
      auto text = std::string();
      for (const auto &uri : result.files.remote) text += (text.empty() ? "" : "\n") + uri;
      pasteText(text);
    }
    if (!result.files.local.empty()) {
      openAttach(result.files.local);
    } else if (result.image) {
      openAttach({}, std::exchange(result.image, nullptr), result.text);
    } else if (result.files.remote.empty() && !result.text.empty()) {
      pasteText(result.text);
    }
  });
}

void ChatView::pasteText(const std::string &text) {
  if (text.empty() || !gtk_text_view_get_editable(GTK_TEXT_VIEW(_input))) return;
  auto *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(_input));
  gtk_text_buffer_delete_selection(buffer, TRUE, TRUE);
  gtk_text_buffer_insert_interactive_at_cursor(buffer, text.data(), int(text.size()), TRUE);
  gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(_input), gtk_text_buffer_get_insert(buffer));
}

void ChatView::openAttach(const std::vector<std::string> &paths, GdkTexture *image, const std::string &textOnCancel) {
  const auto known = _store.chat(_chat);
  if (_editing || (_recordBar && _recordBar->recording()) || !known || !_store.canWrite(*known)) {
    g_clear_object(&image);
    pasteText(textOnCancel);
    return;
  }
  if (!_store.canSendFiles(*known)) {
    ShowToast(Tr("lng_forward_send_files_cant"));
    g_clear_object(&image);
    pasteText(textOnCancel);
    return;
  }
  if (const auto open = _attach.lock()) open->close();
  // Where the files go is fixed now: the chat, the topic and the reply.
  const auto replyTo = _replyToId;
  const auto self = _store.user(_store.selfId());
  auto text = composerText();
  const auto first = text.find_first_not_of(" \t\r\n");
  text = first == std::string::npos ? std::string() : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
  const auto alive = std::weak_ptr<void>(_lifetime.token());
  _attach = AttachDialog::Show({
      .parent = _window.widget(),
      .paths = paths,
      .image = image,
      .caption = text,
      .premium = self && self->premium,
      .send = [this, alive, replyTo](const std::vector<AttachFile> &files, bool compress, bool group,
                                     const std::string &caption) {
        return !alive.expired() && sendAttachments(files, compress, group, caption, replyTo);
      },
      .cancelled = [this, alive, textOnCancel] {
        // As in Telegram Desktop: the text pasted with a picture goes to the field instead.
        if (!alive.expired()) pasteText(textOnCancel);
      },
  });
}

namespace {

// Ui::FormatDurationWordsSlowmode: "42 seconds", or "1 min 30 s".
std::string SlowModeDuration(int seconds) {
  if (seconds < 60) return TrCount("lng_slowmode_seconds", seconds);
  return Tr("lng_duration_minutes_seconds", {{"minutes_count", TrCount("lng_duration_minsec_minutes", seconds / 60)},
                                              {"seconds_count", TrCount("lng_duration_minsec_seconds", seconds % 60)}});
}

}  // namespace

// As SendFilesBox did for 0.3.1: the files go in albums of up to ten, the caption with the last one. TDLib sends the
// media of a chat in the order they were given (its queue of unsent media), keeps them while it is closed, and
// uploads in the background.
bool ChatView::sendAttachments(const std::vector<AttachFile> &files, bool compress, bool group,
                               const std::string &caption, std::int64_t replyTo) {
  if (const auto over = CaptionLength(caption) - _store.captionLimit(); over > 0) {
    ShowToast(TrCount("lng_caption_limit_reached", over));
    return false;
  }
  const auto known = _store.chat(_chat);
  if (const auto left = known ? _store.slowModeLeft(*known) : 0) {
    ShowToast(Tr("lng_slowmode_enabled", {{"left", SlowModeDuration(left)}}));
    return false;
  }
  // In slow mode the files go as one message (one album) or not at all, as in 0.3.1.
  auto groups = DivideAttachments(files, compress, group || (known && _store.slowModeApplies(*known)));
  if (groups.empty()) return false;
  if (groups.size() > 1 && known && _store.slowModeApplies(*known)) {
    ShowToast(Tr("lng_slowmode_no_many"));
    return false;
  }
  const auto captioned = CaptionIndex(groups.back());
  for (std::size_t g = 0; g != groups.size(); ++g) {
    const auto &files = groups[g].files;
    auto contents = std::vector<api::object_ptr<api::InputMessageContent>>();
    for (std::size_t i = 0; i != files.size(); ++i) {
      const auto withCaption = g + 1 == groups.size() && i == captioned;
      contents.push_back(AttachContent(files[i], compress, withCaption ? caption : std::string()));
    }
    if (contents.size() > 1) {
      _store.client().send(api::make_object<api::sendMessageAlbum>(_chat, topicObject(), ReplyTo(replyTo), nullptr,
                                                                   std::move(contents)));
    } else {
      _store.client().send(api::make_object<api::sendMessage>(_chat, topicObject(), ReplyTo(replyTo), nullptr, nullptr,
                                                              std::move(contents.front())));
    }
  }
  // The caption came from the message field.
  if (!_editing) {
    setComposerText(std::string());
    cancelReplyOrEdit();
  }
  if (_reachedBottom) scrollToBottom();
  else reload(0);
  return true;
}

void ChatView::cancelUpload(const MessageItem &item) {
  if (!item.data || !item.local()) return;
  _store.client().send(api::make_object<api::deleteMessages>(_chat, std::vector<std::int64_t>{item.id()}, true));
}

void ChatView::debugAttach(const std::string &action) {
  if (action == "paste") {
    g_signal_emit_by_name(_input, "paste-clipboard");
  } else if (action.starts_with("files ")) {
    auto paths = std::vector<std::string>();
    auto rest = action.substr(6);
    for (auto bar = rest.find('|'); ; bar = rest.find('|')) {
      paths.push_back(rest.substr(0, bar));
      if (bar == std::string::npos) break;
      rest = rest.substr(bar + 1);
    }
    openAttach(paths);
  } else if (action.starts_with("copy-")) {
    // What another program would have put on the clipboard: a picture, files, or text.
    auto *clipboard = gtk_widget_get_clipboard(_input);
    const auto value = action.substr(action.find(' ') + 1);
    if (action.starts_with("copy-image ")) {
      if (auto *texture = gdk_texture_new_from_filename(value.c_str(), nullptr)) {
        gdk_clipboard_set_texture(clipboard, texture);
        g_object_unref(texture);
      }
    } else if (action.starts_with("copy-files ")) {
      auto files = std::vector<GFile *>();
      auto rest = value;
      for (auto bar = rest.find('|'); ; bar = rest.find('|')) {
        files.push_back(g_file_new_for_commandline_arg(rest.substr(0, bar).c_str()));
        if (bar == std::string::npos) break;
        rest = rest.substr(bar + 1);
      }
      auto *list = gdk_file_list_new_from_array(files.data(), files.size());
      gdk_clipboard_set(clipboard, GDK_TYPE_FILE_LIST, list);
      g_boxed_free(GDK_TYPE_FILE_LIST, list);
      for (auto *file : files) g_object_unref(file);
    } else if (action.starts_with("copy-text ")) {
      gdk_clipboard_set_text(clipboard, value.c_str());
    }
  } else if (action == "field") {
    g_message("attach field: \"%s\"", composerText().c_str());
  } else if (action.starts_with("menu ")) {
    // An item of the attach menu, as a tap on it: file or location.
    gtk_widget_activate_action(_input, ("attach." + action.substr(5)).c_str(), nullptr);
  } else if (const auto attach = _attach.lock()) {
    attach->debugAction(action);
  } else {
    g_message("attach state: no dialog");
  }
}

// --- voice messages recorded here ---

bool ChatView::canRecordVoice() const {
  const auto shown = gtk_stack_get_visible_child_name(GTK_STACK(_composerStack));
  return !_editing && (g_strcmp0(shown, "field") == 0 || g_strcmp0(shown, "record") == 0);
}

void ChatView::recordingEnded(bool sent) {
  refreshComposerState();
  if (!sent) return;
  cancelReplyOrEdit();
  if (_reachedBottom) scrollToBottom();
  else reload(0);
}

void ChatView::openLocation(const MessageItem &item) {
  if (item.data) ShowLocation(_window, *item.data);
}

// --- the place on the screen, kept while the app is in the background -----------------------

ChatView::Place ChatView::place() const {
  if (_stickToBottom || atBottom()) return {};
  for (const auto &item : _items) {
    const auto row = rowFor(item.get());
    auto *widget = row ? row->widgetFor(item.get()) : nullptr;
    auto point = graphene_point_t();
    const auto zero = GRAPHENE_POINT_INIT(0, 0);
    if (!widget || !gtk_widget_get_mapped(widget) || !gtk_widget_compute_point(widget, _scroll, &zero, &point)) continue;
    if (point.y >= 0) return {item->id(), double(point.y)};
  }
  return {};
}

void ChatView::restorePlace(Place place) {
  // A topic finds its place in the group first; it opens where it was read instead.
  if (place.message && !_topic) jumpToMessage(place.message, place.y);
}

// --- the player ---

AudioPlayer *ChatView::player() {
  auto *player = _window.player();
  if (!player) return nullptr;
  player->setNext([this](const AudioPlayer::Track &finished) -> std::optional<AudioPlayer::Track> {
    auto result = std::optional<AudioPlayer::Track>();
    for (const auto &item : _items) {
      if (!item->data || item->local() || item->id() <= finished.message) continue;
      if (result && item->id() >= result->message) continue;
      const auto track = PlayerTrack(*item->data);
      if (track && track->chat == finished.chat && track->voice == finished.voice) result = track;
    }
    return result;
  }, _lifetime);
  return player;
}

void ChatView::debugPlayer(const std::string &what) {
  auto *player = this->player();
  if (!player) return;
  const auto gap = what.find(' ');
  const auto name = what.substr(0, gap);
  const auto rest = gap == std::string::npos ? std::string() : what.substr(gap + 1);
  const auto &state = player->state();
  if (name == "play") {
    const auto item = debugItem(std::atoll(rest.c_str()));
    const auto track = (item && item->data) ? PlayerTrack(*item->data) : std::nullopt;
    if (track) player->playPause(*track);
    else g_message("player: message %s has nothing to play", rest.c_str());
  } else if (name == "pause") {
    if (state.status != AudioPlayer::Status::Stopped) player->playPause(state.track);
  } else if (name == "seek" || name == "drag") {
    if (state.status == AudioPlayer::Status::Stopped) return;
    const auto track = state.track;
    if (name == "drag") player->startSeeking(track);
    player->seek(track, g_ascii_strtod(rest.c_str(), nullptr));
  } else if (name == "stop") {
    player->stop();
  }
}

}  // namespace Contrail
