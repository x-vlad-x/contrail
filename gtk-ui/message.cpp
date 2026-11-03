#include "message.hpp"

#include "chat_view.hpp"
#include "layout.hpp"
#include "device_scale.hpp"
#include "main_window.hpp"
#include "media_progress.hpp"
#include "picture_decode.hpp"
#include "reactions.hpp"
#include "scroll_helpers.hpp"
#include "td/dates.hpp"
#include "td/message_text.hpp"
#include "td/strings.hpp"
#include "text.hpp"
#include "userpics.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Contrail {
namespace {

namespace api = Td::api;

constexpr auto kMaxPhotoWidth = 280;
constexpr auto kMaxPhotoHeight = 320;
constexpr auto kMinPhotoSize = 100;
constexpr auto kStickerSize = 160;
constexpr auto kRoundVideoWidth = 200;
constexpr auto kAlbumMinWidth = 100;  // The narrowest cell, before the album is scaled to the screen.
constexpr auto kAlbumSpacing = 2;
constexpr auto kDescriptionLimit = 300;  // UTF-16 units, as TDLib counts.
constexpr auto kPictureDownloadPriority = 8;
constexpr auto kMegabyte = std::int64_t(1024 * 1024);
constexpr auto kKilobyte = std::int64_t(1024);
constexpr auto kPlayIcon = "media-playback-start-symbolic";
constexpr auto kImageIcon = "image-x-generic-symbolic";
constexpr auto kClockIcon = "contrail-clock-symbolic";
constexpr auto kIconKey = "contrail-icon";
constexpr auto kQualityKey = "contrail-media-quality";
// Bumped when a picture gives its texture back: a decode started before then is dropped when it ends.
constexpr auto kGenerationKey = "contrail-media-generation";
constexpr auto kMiniKey = "contrail-media-mini";  // The blurred thumbnail, what a picture far off shows.
constexpr auto kCancelKey = "contrail-media-cancel";  // Cancels the picture's decodes not started yet.
// Pictures are decoded two at a time, plus the large lane (FeedDecodes()). GLib's pool behind g_task_run_in_thread
// grows to ten threads under a page of photos, and they all take the cores the drawing needs; two keep up with a
// fling (25–50 ms a photo on the FLX1s).
constexpr auto kDecodeThreads = 2;

// Counted for the debug state: a reaction coming in must not build its row again.
std::uint64_t rowBuilds = 0;
std::uint64_t rowRebuilds = 0;
std::uint64_t reactionBarBuilds = 0;

// Double taps on these are theirs (a photo opens, a button acts), not a reaction.
[[nodiscard]] bool InteractiveTarget(GtkWidget *target, GtkWidget *bubble) {
  for (auto *widget = target; widget && widget != bubble; widget = gtk_widget_get_parent(widget)) {
    if (GTK_IS_BUTTON(widget) || GTK_IS_PICTURE(widget) || GTK_IS_FLOW_BOX(widget) || GTK_IS_RANGE(widget)) return true;
  }
  return false;
}

// What a picture shows. A slower decode of something worse must not replace something better.
enum PictureQuality { kQualityThumbnail = 1, kQualityFull = 2 };

template <typename Type, typename Base>
[[nodiscard]] const Type *Cast(const Base *object) {
  return (object && object->get_id() == Type::ID) ? static_cast<const Type *>(object) : nullptr;
}

[[nodiscard]] const api::MessageContent *ContentOf(const MessageItem *item) {
  return (item && item->data) ? item->data->content_.get() : nullptr;
}

[[nodiscard]] Size FitPhoto(int w, int h, int maxWidth) {
  if (w <= 0 || h <= 0) {
    return {maxWidth, maxWidth * 3 / 4};
  }
  auto width = std::min(w, maxWidth);
  auto height = int(std::int64_t(h) * width / w);
  if (height > kMaxPhotoHeight) {
    height = kMaxPhotoHeight;
    width = std::max(int(std::int64_t(w) * height / h), kMinPhotoSize);
  }
  return {std::max(width, kMinPhotoSize), std::max(height, 48)};
}

[[nodiscard]] const api::photoSize *LargestSize(const api::photo &photo) {
  const api::photoSize *result = nullptr;
  auto best = std::int64_t(-1);
  for (const auto &size : photo.sizes_) {
    if (!size || !size->photo_) continue;
    const auto area = std::int64_t(size->width_) * size->height_;
    if (area > best) {
      best = area;
      result = size.get();
    }
  }
  return result;
}

// The smallest size that fills `width` x `height`, the largest there is when none does.
[[nodiscard]] const api::photoSize *FittingSize(const api::photo &photo, int width, int height) {
  const api::photoSize *result = nullptr;
  for (const auto &size : photo.sizes_) {
    if (!size || !size->photo_ || size->width_ < width || size->height_ < height) continue;
    if (!result || std::int64_t(size->width_) * size->height_ < std::int64_t(result->width_) * result->height_) {
      result = size.get();
    }
  }
  return result ? result : LargestSize(photo);
}

// A video or a GIF, which are drawn the same.
struct Visual {
  int width = 0;
  int height = 0;
  const api::minithumbnail *mini = nullptr;
  const api::thumbnail *thumbnail = nullptr;
  const api::file *file = nullptr;
  int duration = 0;
  bool gif = false;
};

[[nodiscard]] Visual VisualOf(const api::MessageContent *content) {
  if (const auto message = Cast<api::messageVideo>(content); message && message->video_) {
    const auto &video = *message->video_;
    return {video.width_, video.height_, video.minithumbnail_.get(), video.thumbnail_.get(), video.video_.get(),
            video.duration_, false};
  }
  if (const auto message = Cast<api::messageAnimation>(content); message && message->animation_) {
    const auto &animation = *message->animation_;
    return {animation.width_, animation.height_, animation.minithumbnail_.get(), animation.thumbnail_.get(),
            animation.animation_.get(), animation.duration_, true};
  }
  return {};
}

// A photo or a video: these make the grid of an album. Files (pictures sent as files too) and music make a
// column, as in Telegram's apps.
[[nodiscard]] bool IsVisualMedia(const MessageItem *item) {
  const auto content = ContentOf(item);
  if (const auto message = Cast<api::messagePhoto>(content)) {
    return message->photo_ && LargestSize(*message->photo_);
  }
  return VisualOf(content).file != nullptr;
}

[[nodiscard]] Size VisualSize(const MessageItem *item) {
  const auto content = ContentOf(item);
  auto size = Size();
  if (const auto message = Cast<api::messagePhoto>(content); message && message->photo_) {
    if (const auto largest = LargestSize(*message->photo_)) size = {largest->width_, largest->height_};
  } else {
    const auto visual = VisualOf(content);
    size = {visual.width, visual.height};
  }
  return (size.width > 0 && size.height > 0) ? size : Size{kMaxPhotoWidth, kMaxPhotoWidth};
}

// The text or the caption.
[[nodiscard]] const api::formattedText *TextOf(const MessageItem *item) {
  const auto content = ContentOf(item);
  if (!content) return nullptr;
  const api::formattedText *result = nullptr;
  switch (content->get_id()) {
    case api::messageText::ID: result = static_cast<const api::messageText *>(content)->text_.get(); break;
    case api::messagePhoto::ID: result = static_cast<const api::messagePhoto *>(content)->caption_.get(); break;
    case api::messageVideo::ID: result = static_cast<const api::messageVideo *>(content)->caption_.get(); break;
    case api::messageAnimation::ID:
      result = static_cast<const api::messageAnimation *>(content)->caption_.get();
      break;
    case api::messageVoiceNote::ID:
      result = static_cast<const api::messageVoiceNote *>(content)->caption_.get();
      break;
    case api::messageAudio::ID: result = static_cast<const api::messageAudio *>(content)->caption_.get(); break;
    case api::messageDocument::ID:
      result = static_cast<const api::messageDocument *>(content)->caption_.get();
      break;
    default: break;
  }
  return (result && !result->text_.empty()) ? result : nullptr;
}

[[nodiscard]] const char *DocumentIcon(const std::string &mime) {
  if (mime.starts_with("audio/")) {
    return "audio-x-generic-symbolic";
  } else if (mime.starts_with("video/")) {
    return "video-x-generic-symbolic";
  } else if (mime.starts_with("image/")) {
    return kImageIcon;
  }
  return "text-x-generic-symbolic";
}

[[nodiscard]] std::string PeerColorClass(int color) {
  return "peer-color-" + std::to_string(std::abs(color) % 7);
}

[[nodiscard]] std::string UserName(const Td::Store &store, Td::UserId id) {
  const auto user = store.user(id);
  return user ? Td::DisplayName(*user) : std::string();
}

[[nodiscard]] std::string ChatTitle(const Td::Store &store, Td::ChatId id) {
  const auto chat = store.chat(id);
  return chat ? chat->title : std::string();
}

[[nodiscard]] std::string SenderName(const Td::Store &store, const api::MessageSender *sender) {
  if (const auto user = Cast<api::messageSenderUser>(sender)) return UserName(store, user->user_id_);
  if (const auto chat = Cast<api::messageSenderChat>(sender)) return ChatTitle(store, chat->chat_id_);
  return {};
}

[[nodiscard]] int SenderColor(const Td::Store &store, const api::MessageSender *sender) {
  if (const auto id = Cast<api::messageSenderUser>(sender)) {
    const auto user = store.user(id->user_id_);
    return user ? user->accentColor : 0;
  }
  if (const auto id = Cast<api::messageSenderChat>(sender)) {
    const auto chat = store.chat(id->chat_id_);
    return chat ? chat->accentColor : 0;
  }
  return 0;
}

[[nodiscard]] std::string OriginName(const Td::Store &store, const api::MessageOrigin *origin) {
  if (const auto user = Cast<api::messageOriginUser>(origin)) return UserName(store, user->sender_user_id_);
  if (const auto hidden = Cast<api::messageOriginHiddenUser>(origin)) return hidden->sender_name_;
  if (const auto chat = Cast<api::messageOriginChat>(origin)) return ChatTitle(store, chat->sender_chat_id_);
  if (const auto channel = Cast<api::messageOriginChannel>(origin)) return ChatTitle(store, channel->chat_id_);
  return {};
}

// The state of a file when a row is built. The store knows a file only once it has changed or was asked for: TDLib
// sends no update for one already on disk, and the object in the message is all there is.
[[nodiscard]] Td::File FileState(const Td::Store &store, const api::file &file) {
  if (const auto known = store.file(file.id_)) return *known;
  auto result = Td::File();
  result.id = file.id_;
  result.size = file.size_ ? file.size_ : file.expected_size_;
  if (file.local_) {
    result.downloaded = file.local_->downloaded_size_;
    result.downloading = file.local_->is_downloading_active_;
    result.complete = file.local_->is_downloading_completed_;
    if (result.complete) result.path = file.local_->path_;
  }
  return result;
}

// The rows that want a picture: one sticker can be in many messages, and the last of them to go stops the download.
[[nodiscard]] std::unordered_map<Td::FileId, int> &WantedPictures() {
  static auto result = std::unordered_map<Td::FileId, int>();
  return result;
}

void WantPicture(Td::FileId id) {
  if (id) ++WantedPictures()[id];
}

void ReleasePicture(Td::Store &store, Td::FileId id) {
  if (!id) return;
  auto &wanted = WantedPictures();
  const auto it = wanted.find(id);
  if (it == wanted.end() || --it->second > 0) return;
  wanted.erase(it);
  const auto known = store.file(id);
  if (!known || !known->complete) store.cancelDownload(id);
}

struct PictureDecode {
  std::string path;
  int width = 0;
  int height = 0;
  int generation = 0;
  // Found to be a large picture whose turn hadn't come: it waits for the large lane. Written by the decode thread
  // before the task comes back to the main thread.
  bool large = false;
};

// The decodes waiting, on the main thread, each holding its task. They are handed to the threads a few at a time,
// the nearest to the screen first, as it is when a thread gets free: a fling's later pictures don't wait behind the
// ones it has left. Large progressive JPEGs (most of Telegram's photos) take turns in a lane of their own, so they
// don't keep both threads waiting for each other.
std::vector<GTask *> PendingDecodes;
std::atomic<int> DecodesInPool = 0;   // Handed to DecodePool(), not finished.
std::atomic<int> DecodesInLarge = 0;  // Handed to LargePool(), not finished.
guint FeedSource = 0;

void FeedDecodes();

[[nodiscard]] PictureDecode &DecodeOf(GTask *task) {
  return *static_cast<PictureDecode *>(g_task_get_task_data(task));
}

// The main thread only: one feeding for everything asked in this pass (a row's pictures, a viewport's rows).
void ScheduleFeed() {
  if (FeedSource) return;
  FeedSource = g_idle_add(+[](gpointer) -> gboolean {
    FeedSource = 0;
    FeedDecodes();
    return G_SOURCE_REMOVE;
  }, nullptr);
}

// From a decode thread. The task holds its picture (the source object): its last reference must go on the main
// thread, where the widget may be finalized, never here. Holding this one until then keeps GTask's own references
// from being the last.
// At the default priority, above GTK's drawing: a free thread doesn't wait for a fling's frames to leave time.
void ReleaseOnMain(GTask *task) {
  g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer data) -> gboolean {
    g_object_unref(data);
    FeedDecodes();
    return G_SOURCE_REMOVE;
  }, task, nullptr);
}

// A texture as large as the picture on the screen: drawing a downscaled full-size photo costs time on every frame
// while scrolling, and the memory of all the loaded photos. `lane` is non-null in the large lane, which waits for
// its turn; the other threads leave a large picture whose turn hasn't come to it.
void DecodeInPool(gpointer data, gpointer lane) {
  auto *task = G_TASK(data);
  const auto inLarge = (lane != nullptr);
  auto &counter = inLarge ? DecodesInLarge : DecodesInPool;
  if (!g_task_return_error_if_cancelled(task)) {
    auto &decode = DecodeOf(task);
    static const auto debug = g_getenv("CONTRAIL_DEBUG") != nullptr;
    const auto start = debug ? g_get_monotonic_time() : 0;
    auto deferred = false;
    auto *pixbuf = DecodePictureFile(decode.path.c_str(), decode.width, decode.height,
                                     {.waitForLarge = inLarge, .deferred = &deferred,
                                      .cancellable = g_task_get_cancellable(task)});
    if (deferred) {
      decode.large = true;
      --counter;
      // Back to the main thread's queue, with its reference.
      g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer data) -> gboolean {
        PendingDecodes.push_back(G_TASK(data));
        FeedDecodes();
        return G_SOURCE_REMOVE;
      }, task, nullptr);
      return;
    }
    if (debug && pixbuf) {
      g_debug("Picture: %dx%d in %.1f ms, %s", gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf),
              (g_get_monotonic_time() - start) / 1000., decode.path.c_str());
    }
    if (!pixbuf && g_task_return_error_if_cancelled(task)) {
      // Cancelled while it waited for its turn.
    } else {
      g_task_return_pointer(task, pixbuf, pixbuf ? g_object_unref : nullptr);
    }
  }
  --counter;
  ReleaseOnMain(task);
}

[[nodiscard]] GThreadPool *DecodePool() {
  static auto *pool = g_thread_pool_new(DecodeInPool, nullptr, kDecodeThreads, FALSE, nullptr);
  return pool;
}

[[nodiscard]] GThreadPool *LargePool() {
  static auto *pool = g_thread_pool_new(DecodeInPool, GINT_TO_POINTER(1), 1, FALSE, nullptr);
  return pool;
}

void PictureDecoded(GObject *source, GAsyncResult *result, gpointer) {
  // The picture is the task's source object: it is alive here even if its message has gone.
  auto *picture = GTK_WIDGET(source);
  auto *task = G_TASK(result);
  auto *pixbuf = static_cast<GdkPixbuf *>(g_task_propagate_pointer(task, nullptr));
  if (!pixbuf) return;
  const auto quality = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(task), kQualityKey));
  if (GPOINTER_TO_INT(g_object_get_data(source, kQualityKey)) > quality
      || GPOINTER_TO_INT(g_object_get_data(source, kGenerationKey)) != DecodeOf(task).generation
      || gdk_pixbuf_get_bits_per_sample(pixbuf) != 8) {
    g_object_unref(pixbuf);
    return;
  }
  g_object_set_data(source, kQualityKey, GINT_TO_POINTER(quality));
  auto *texture = TextureFromPixbuf(pixbuf);
  gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(texture));
  g_object_unref(texture);
}

// How far the picture is from the visible part of the list it scrolls in, 0 on the screen.
[[nodiscard]] double ScreenDistance(GtkWidget *picture) {
  auto *scroll = gtk_widget_get_ancestor(picture, GTK_TYPE_SCROLLED_WINDOW);
  graphene_rect_t bounds;
  if (!scroll || !gtk_widget_compute_bounds(picture, scroll, &bounds)) return std::numeric_limits<double>::max();
  const auto top = double(bounds.origin.y), bottom = top + bounds.size.height;
  return (bottom < 0) ? -bottom : std::max(top - gtk_widget_get_height(scroll), 0.);
}

void FeedDecodes() {
  // The cancelled ones end here, and let their pictures go.
  std::erase_if(PendingDecodes, [](GTask *task) {
    if (!g_task_return_error_if_cancelled(task)) return false;
    g_object_unref(task);
    return true;
  });
  // One more than the threads in each lane: none waits for work (the second large one waits for the turn).
  auto freePool = kDecodeThreads + 1 - DecodesInPool.load();
  auto freeLarge = 2 - DecodesInLarge.load();
  const auto anyLarge = std::ranges::any_of(PendingDecodes, [](GTask *task) { return DecodeOf(task).large; });
  const auto anySmall = std::ranges::any_of(PendingDecodes, [](GTask *task) { return !DecodeOf(task).large; });
  if (!(freePool > 0 && anySmall) && !(freeLarge > 0 && anyLarge)) return;
  // The distances are measured once a feeding; only the few nearest are put in order.
  auto order = std::vector<std::pair<double, std::size_t>>();
  order.reserve(PendingDecodes.size());
  for (auto i = std::size_t(0); i < PendingDecodes.size(); ++i) {
    order.emplace_back(ScreenDistance(GTK_WIDGET(g_task_get_source_object(PendingDecodes[i]))), i);
  }
  // By distance, then in the order asked. At most this many are handed out of each lane's kind.
  const auto wanted = std::min(order.size(), std::size_t(std::max(freePool, 0) + std::max(freeLarge, 0)));
  if (anyLarge && anySmall) {
    std::ranges::sort(order);  // Mixed: the nearest of each kind can be anywhere.
  } else {
    std::ranges::partial_sort(order, order.begin() + std::ptrdiff_t(wanted));
    order.resize(wanted);
  }
  auto handed = std::vector<bool>(PendingDecodes.size());
  for (const auto &[distance, i] : order) {
    auto *task = PendingDecodes[i];
    if (DecodeOf(task).large) {
      if (freeLarge <= 0) continue;
      --freeLarge;
      ++DecodesInLarge;
      g_thread_pool_push(LargePool(), task, nullptr);
    } else {
      if (freePool <= 0) continue;
      --freePool;
      ++DecodesInPool;
      g_thread_pool_push(DecodePool(), task, nullptr);
    }
    handed[i] = true;
    if (freePool <= 0 && freeLarge <= 0) break;
  }
  auto kept = std::size_t(0);
  for (auto i = std::size_t(0); i < PendingDecodes.size(); ++i) {
    if (!handed[i]) PendingDecodes[kept++] = PendingDecodes[i];
  }
  PendingDecodes.resize(kept);
}

[[nodiscard]] GCancellable *PictureCancellable(GtkWidget *picture) {
  auto *cancellable = static_cast<GCancellable *>(g_object_get_data(G_OBJECT(picture), kCancelKey));
  if (!cancellable) {
    cancellable = g_cancellable_new();
    g_object_set_data_full(G_OBJECT(picture), kCancelKey, cancellable, g_object_unref);
  }
  return cancellable;
}

// The picture's decodes still waiting don't run (a large one waiting for its turn stops there); the next ones get a
// new cancellable.
void CancelPictureDecodes(GtkWidget *picture) {
  if (auto *cancellable = g_object_get_data(G_OBJECT(picture), kCancelKey)) {
    g_cancellable_cancel(G_CANCELLABLE(cancellable));
    g_object_set_data(G_OBJECT(picture), kCancelKey, nullptr);
    ScheduleFeed();  // Lets the queue drop them, and the picture.
  }
}

void LoadPicture(GtkWidget *picture, const std::string &path, PictureQuality quality) {
  const auto pixels = [&](const char *key) {
    return DevicePixels(picture, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(picture), key)));
  };
  auto *decode = new PictureDecode{path, pixels("contrail-media-width"), pixels("contrail-media-height"),
                                   GPOINTER_TO_INT(g_object_get_data(G_OBJECT(picture), kGenerationKey))};
  auto *task = g_task_new(picture, PictureCancellable(picture), PictureDecoded, nullptr);
  g_task_set_task_data(task, decode, [](gpointer data) { delete static_cast<PictureDecode *>(data); });
  g_object_set_data(G_OBJECT(task), kQualityKey, GINT_TO_POINTER(int(quality)));
  PendingDecodes.push_back(task);  // The queue holds the reference.
  ScheduleFeed();
}

// The blurred picture a message carries in itself, a JPEG of a few hundred bytes: small enough to decode right here.
void SetMiniThumbnail(GtkWidget *picture, const api::minithumbnail *mini) {
  if (!mini || mini->data_.empty()) return;
  auto *bytes = g_bytes_new(mini->data_.data(), mini->data_.size());
  if (auto *texture = gdk_texture_new_from_bytes(bytes, nullptr)) {
    gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(texture));
    g_object_set_data_full(G_OBJECT(picture), kMiniKey, texture, g_object_unref);
  }
  g_bytes_unref(bytes);
}

// The picture gives back its decoded texture (and the memory it holds, here and in the GPU) and shows its blurred
// thumbnail, or nothing; a decode still running is dropped. Its size stays: the list doesn't move.
void DropPicture(GtkWidget *picture) {
  CancelPictureDecodes(picture);
  const auto generation = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(picture), kGenerationKey));
  g_object_set_data(G_OBJECT(picture), kGenerationKey, GINT_TO_POINTER(generation + 1));
  g_object_set_data(G_OBJECT(picture), kQualityKey, nullptr);
  gtk_picture_set_paintable(GTK_PICTURE(picture),
                            static_cast<GdkPaintable *>(g_object_get_data(G_OBJECT(picture), kMiniKey)));
}

[[nodiscard]] std::string Percent(double value) {
  return std::to_string(int(value * 100)) + "%";
}

// Tenths of a unit, as Telegram's apps round them: down.
[[nodiscard]] std::string Tenths(std::int64_t value, std::int64_t unit) {
  const auto tenths = value * 10 / unit;
  return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}

[[nodiscard]] std::string FormatSize(std::int64_t size) {
  if (size >= kMegabyte) return Tenths(size, kMegabyte) + " MB";
  if (size >= kKilobyte) return Tenths(size, kKilobyte) + " KB";
  return std::to_string(size) + " B";
}

// "12.3 / 45.6 MB · 27%"; before the first part comes (other downloads go first): waiting.
[[nodiscard]] std::string DownloadText(std::int64_t offset, std::int64_t size) {
  if (offset <= 0) return Translated("Waiting…", "Ожидание…", "Очікування…");
  if (size <= 0) return FormatSize(offset);
  auto result = (size >= kMegabyte)   ? Tenths(offset, kMegabyte) + " / " + Tenths(size, kMegabyte) + " MB"
                : (size >= kKilobyte) ? Tenths(offset, kKilobyte) + " / " + Tenths(size, kKilobyte) + " KB"
                                      : std::to_string(offset) + " / " + std::to_string(size) + " B";
  return result + " · " + Percent(DownloadFraction(offset, size));
}

[[nodiscard]] std::string FormatDuration(int seconds) {
  char buffer[32];
  const auto hours = seconds / 3600, minutes = seconds / 60 % 60, rest = seconds % 60;
  if (hours > 0) {
    std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", hours, minutes, rest);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%d:%02d", minutes, rest);
  }
  return buffer;
}

[[nodiscard]] std::string PhoneText(const std::string &phone) {
  return (phone.empty() || phone.front() == '+') ? phone : "+" + phone;
}

[[nodiscard]] std::string FallbackStatus(int duration, bool gif) {
  if (duration > 0) return FormatDuration(duration);
  return gif ? "GIF" : "";
}

// A download's progress over a video, in place of its play button: a ring around a stop sign, as a tap
// on the video cancels the download.
GtkWidget *MakeProgressRing() {
  constexpr auto kSize = 48;
  auto *ring = gtk_drawing_area_new();
  gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(ring), kSize);
  gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(ring), kSize);
  gtk_widget_set_halign(ring, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(ring, GTK_ALIGN_CENTER);
  gtk_widget_set_can_target(ring, FALSE);
  gtk_widget_set_visible(ring, FALSE);
  gtk_drawing_area_set_draw_func(
      GTK_DRAWING_AREA(ring),
      [](GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer) {
        const auto fraction = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(area), "contrail-fraction")) / 1000.;
        const auto x = width / 2., y = height / 2., radius = std::min(width, height) / 2.;
        const auto line = radius - 5.;
        cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
        cairo_arc(cr, x, y, radius, 0, 2 * std::numbers::pi);
        cairo_fill(cr);
        cairo_set_line_width(cr, 3);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.3);
        cairo_arc(cr, x, y, line, 0, 2 * std::numbers::pi);
        cairo_stroke(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        if (fraction > 0) {
          cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
          cairo_arc(cr, x, y, line, -std::numbers::pi / 2, -std::numbers::pi / 2 + 2 * std::numbers::pi * fraction);
          cairo_stroke(cr);
        }
        cairo_rectangle(cr, x - 6, y - 6, 12, 12);
        cairo_fill(cr);
      },
      nullptr, nullptr);
  return ring;
}

void SetRingFraction(GtkWidget *ring, double fraction) {
  g_object_set_data(G_OBJECT(ring), "contrail-fraction", GINT_TO_POINTER(int(std::clamp(fraction, 0., 1.) * 1000)));
  gtk_widget_queue_draw(ring);
}

// The picture of a page in a link preview.
[[nodiscard]] const api::photo *PreviewPhoto(const api::LinkPreviewType *type) {
  if (const auto article = Cast<api::linkPreviewTypeArticle>(type)) return article->photo_.get();
  if (const auto photo = Cast<api::linkPreviewTypePhoto>(type)) return photo->photo_.get();
  if (const auto app = Cast<api::linkPreviewTypeApp>(type)) return app->photo_.get();
  if (const auto app = Cast<api::linkPreviewTypeWebApp>(type)) return app->photo_.get();
  if (const auto video = Cast<api::linkPreviewTypeVideo>(type)) return video->cover_.get();
  if (const auto player = Cast<api::linkPreviewTypeEmbeddedVideoPlayer>(type)) return player->thumbnail_.get();
  if (const auto player = Cast<api::linkPreviewTypeEmbeddedAnimationPlayer>(type)) return player->thumbnail_.get();
  if (const auto player = Cast<api::linkPreviewTypeEmbeddedAudioPlayer>(type)) return player->thumbnail_.get();
  return nullptr;
}

// A player's bar takes no input of its own: the wheel over it scrolls the chat, and the message seeks with its own
// gestures, which leave vertical moves to the scrolling (MessageWidget::setupSeeking()).
void DisableControllers(GtkWidget *widget) {
  auto *controllers = gtk_widget_observe_controllers(widget);
  const auto count = g_list_model_get_n_items(controllers);
  for (guint i = 0; i != count; ++i) {
    auto *controller = GTK_EVENT_CONTROLLER(g_list_model_get_item(controllers, i));
    gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_NONE);
    g_object_unref(controller);
  }
  g_object_unref(controllers);
}

[[nodiscard]] std::string LoadingText() {
  return Translated("Loading…", "Загрузка…", "Завантаження…");
}

}  // namespace

std::optional<AudioPlayer::Track> PlayerTrack(const api::message &message) {
  const auto make = [&](const api::file *file, int duration, bool voice) -> std::optional<AudioPlayer::Track> {
    if (!file) return std::nullopt;
    return AudioPlayer::Track{message.chat_id_, message.id_, file->id_, duration, voice};
  };
  if (const auto voice = Cast<api::messageVoiceNote>(message.content_.get()); voice && voice->voice_note_) {
    return make(voice->voice_note_->voice_.get(), voice->voice_note_->duration_, true);
  }
  if (const auto audio = Cast<api::messageAudio>(message.content_.get()); audio && audio->audio_) {
    return make(audio->audio_->audio_.get(), audio->audio_->duration_, false);
  }
  return std::nullopt;
}

const api::photo *PhotoWithFile(const api::MessageContent *content, Td::FileId file) {
  const api::photo *photo = nullptr;
  if (const auto message = Cast<api::messagePhoto>(content)) {
    photo = message->photo_.get();
  } else if (const auto text = Cast<api::messageText>(content); text && text->link_preview_) {
    photo = PreviewPhoto(text->link_preview_->type_.get());
  }
  if (!photo) return nullptr;
  const auto has = std::any_of(photo->sizes_.begin(), photo->sizes_.end(),
                               [&](const auto &size) { return size && size->photo_ && size->photo_->id_ == file; });
  return has ? photo : nullptr;
}

MessageWidget::MessageWidget(Items items, ChatView &chat) : _items(std::move(items)), _chat(chat) {

  g_assert(!_items.empty());
  _row = gtk_list_box_row_new();
  g_object_ref_sink(_row);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(_row), FALSE);
  gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(_row), FALSE);
  gtk_widget_set_focusable(_row, FALSE);
  g_object_set_data(G_OBJECT(_row), "contrail-message", this);
  build();
}

MessageWidget::~MessageWidget() {
  // A menu open on this message mustn't outlive the row it is a child of (GTK warns of a row finalized with it).
  _chat.dropMessageMenu(_row);
  _signals.clear();
  clearFiles();
  if (_highlightTimeout) {
    g_source_remove(_highlightTimeout);
  }
  g_object_set_data(G_OBJECT(_row), "contrail-message", nullptr);
  if (auto *parent = gtk_widget_get_parent(_row)) {
    gtk_list_box_remove(GTK_LIST_BOX(parent), _row);
  }
  g_object_unref(_row);
}

void MessageWidget::setItems(Items items) {
  if (items.empty() || items == _items) return;
  _items = std::move(items);
  refresh();
}

GtkWidget *MessageWidget::widgetFor(const MessageItem *item) const {
  if (_items.size() > 1) {
    for (const auto &cell : _cells) {
      if (cell->item == item && cell->widget) return cell->widget;
    }
  }
  return _row;
}

void MessageWidget::refresh() {
  ++rowRebuilds;
  _signals.clear();
  clearFiles();
  if (_highlightTimeout) {
    g_source_remove(std::exchange(_highlightTimeout, 0));
  }
  _highlighted = nullptr;
  _replyPending = false;
  _taps.clear();
  // A menu open on the old widgets goes with them.
  _chat.dropMessageMenu(_row);
  _name = _avatar = _footer = _footerTime = _footerStatus = _reactions = nullptr;
  build();
}

void MessageWidget::clearFiles() {
  cancelSeeking();
  _playerLifetime.reset();
  _playerWatched = false;
  auto &store = _chat.store();
  for (const auto &cell : _cells) {
    ReleasePicture(store, cell->pictureFile);
    ReleasePicture(store, cell->thumbnailFile);
    // The cell's widgets go: a decode not started yet would be for nothing.
    if (cell->picture) CancelPictureDecodes(cell->picture);
  }
  _cells.clear();
}

MessageWidget::Cell &MessageWidget::addCell(MessageItem *item) {
  _cells.push_back(std::make_unique<Cell>(Cell{.item = item}));
  return *_cells.back();
}

bool MessageWidget::isAlbumGrid() const {
  return _items.size() > 1 && std::all_of(_items.begin(), _items.end(), IsVisualMedia);
}

bool MessageWidget::isService() const {
  const auto content = ContentOf(_items.front());
  return content && Td::IsServiceMessage(*content);
}

// The message of the bubble as a whole (the menu outside the cells, swiping to reply): the one with
// the caption, as the official apps do, else the first.
MessageItem *MessageWidget::mainItem() const {
  for (const auto &item : _items) {
    if (TextOf(item)) return item;
  }
  return _items.front();
}

void MessageWidget::build() {
  ++rowBuilds;
  const auto first = _items.front();
  gtk_widget_set_visible(_row, TRUE);
  if (!first->data) {
    gtk_widget_set_visible(_row, FALSE);
    return;
  }
  auto &store = _chat.store();
  if (first->sponsored) {
    auto *line = buildSponsored(first);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(_row), line);
    _content = line;
    return;
  }
  if (isService()) {
    const auto text = Td::ServiceText(*first->data, store);
    // Some have no text (a group became a supergroup), and no line then.
    gtk_widget_set_visible(_row, !text.empty());
    auto *label = MakeTextLabel(text);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_label_set_xalign(GTK_LABEL(label), 0.5);
    gtk_widget_add_css_class(label, "service-message");
    gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(label, 4);
    gtk_widget_set_margin_bottom(label, 4);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(_row), label);
    _content = label;
    return;
  }

  const auto chat = store.chat(_chat.chat());
  const auto broadcast = chat && chat->kind == Td::ChatKind::Channel;
  const auto group = chat && chat->kind == Td::ChatKind::Group;
  const auto out = first->data->is_outgoing_ && !broadcast;
  const auto showSender = !out && group;

  auto *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(line, out ? GTK_ALIGN_END : GTK_ALIGN_START);
  gtk_widget_set_margin_start(line, 8);
  gtk_widget_set_margin_end(line, 8);
  gtk_widget_set_margin_top(line, 2);
  gtk_widget_set_margin_bottom(line, 2);

  const auto from = first->data->sender_id_.get();
  if (showSender) {
    if (const auto user = Cast<api::messageSenderUser>(from)) {
      _avatar = MakeUserAvatar(store, user->user_id_, 32);
    } else if (const auto sender = Cast<api::messageSenderChat>(from)) {
      _avatar = MakeAvatar(store, sender->chat_id_, 32);
    } else {
      _avatar = adw_avatar_new(32, nullptr, TRUE);
    }
    gtk_widget_set_valign(_avatar, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(line), _avatar);
  }

  auto *bubble = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(bubble, "bubble");
  gtk_widget_add_css_class(bubble, out ? "outgoing" : "incoming");
  // Its width mustn't depend on the row's height: see MakeHeightForWidthBin().
  gtk_box_append(GTK_BOX(line), MakeHeightForWidthBin(bubble));

  if (showSender) {
    _name = gtk_label_new(SenderName(store, from).c_str());
    gtk_label_set_xalign(GTK_LABEL(_name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(_name), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(_name, "sender-name");
    gtk_widget_add_css_class(_name, PeerColorClass(SenderColor(store, from)).c_str());
    gtk_box_append(GTK_BOX(bubble), _name);
  }
  // An album is forwarded and replies as a whole: the first message that says so speaks for it.
  for (const auto &item : _items) {
    if (auto *forwarded = buildForwarded(item)) {
      gtk_box_append(GTK_BOX(bubble), forwarded);
      break;
    }
  }
  for (const auto &item : _items) {
    if (auto *reply = buildReply(item)) {
      gtk_box_append(GTK_BOX(bubble), reply);
      break;
    }
  }

  if (isAlbumGrid()) {
    gtk_box_append(GTK_BOX(bubble), buildAlbumGrid());
    // Every caption of the album, in its order.
    for (const auto &item : _items) {
      if (const auto text = TextOf(item)) {
        gtk_box_append(GTK_BOX(bubble), MakeTextLabel(*text));
      }
    }
  } else if (_items.size() > 1) {
    // Files and music sent together: one under the other, each with its caption.
    for (const auto &item : _items) {
      auto &cell = addCell(item);
      auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
      const auto document = Cast<api::messageDocument>(ContentOf(item));
      const auto file = document && document->document_ ? document->document_.get() : nullptr;
      auto *media = (file && file->document_ && file->mime_type_.starts_with("image/"))
                        ? buildDocument(cell, *file->document_, file->file_name_, kImageIcon)
                        : buildMedia(cell, kMaxPhotoWidth);
      if (media) {
        gtk_box_append(GTK_BOX(box), media);
      }
      if (const auto text = TextOf(item)) {
        gtk_box_append(GTK_BOX(box), MakeTextLabel(*text));
      }
      cell.widget = box;
      setupGestures(box, item, false);
      gtk_box_append(GTK_BOX(bubble), box);
    }
  } else {
    auto &cell = addCell(first);
    auto *media = buildMedia(cell, kMaxPhotoWidth);
    if (media) {
      gtk_box_append(GTK_BOX(bubble), media);
    }
    if (const auto text = TextOf(first)) {
      gtk_box_append(GTK_BOX(bubble), MakeTextLabel(*text));
    } else if (!media) {
      // Something this interface can't show: a short description.
      auto *label = MakeTextLabel(Td::MessagePreview(*first->data, store));
      gtk_widget_add_css_class(label, "dim-label");
      gtk_box_append(GTK_BOX(bubble), label);
    }
    if (cell.kind == PictureKind::Sticker) {
      gtk_widget_add_css_class(bubble, "sticker");
    }
    if (auto *webpage = buildWebPage(first)) {
      gtk_box_append(GTK_BOX(bubble), webpage);
    }
  }
  _reactions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(bubble), _reactions);
  _reactionsLayout = "-";  // Never a real key: the chips are made.
  refreshReactions();
  _footer = buildFooter();
  gtk_box_append(GTK_BOX(bubble), _footer);
  setupDoubleTap(bubble);

  setupGestures(bubble, mainItem(), true);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(_row), line);
  _content = line;
}

GtkWidget *MessageWidget::buildForwarded(MessageItem *item) {
  const auto forward = item->data ? item->data->forward_info_.get() : nullptr;
  if (!forward) {
    return nullptr;
  }
  const auto origin = forward->origin_.get();
  const auto name = OriginName(_chat.store(), origin);
  auto *label = gtk_label_new(nullptr);
  gtk_label_set_markup(GTK_LABEL(label),
                       (Escape(Tr("lng_forwarded", {{"user", ""}})) + "<b>" + Escape(name) + "</b>").c_str());
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(label, "forwarded");
  auto open = std::function<void()>();
  if (const auto user = Cast<api::messageOriginUser>(origin)) {
    open = [id = user->sender_user_id_] { HandleLink("tg-user:" + std::to_string(id)); };
  } else if (const auto chat = Cast<api::messageOriginChat>(origin)) {
    open = [this, id = chat->sender_chat_id_] { _chat.openChat(id); };
  } else if (const auto channel = Cast<api::messageOriginChannel>(origin)) {
    open = [this, id = channel->chat_id_] { _chat.openChat(id); };
  }
  if (open) {
    auto *click = gtk_gesture_click_new();
    _signals.connect<void(GtkGestureClick *, int, double, double)>(
        click, "released", [=](GtkGestureClick *, int, double, double) {
          if (TapAllowed()) open();
        });
    gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(click));
  }
  return label;
}

GtkWidget *MessageWidget::buildReply(MessageItem *item) {
  const auto reply = item->data ? Cast<api::messageReplyToMessage>(item->data->reply_to_.get()) : nullptr;
  if (!reply) {
    return nullptr;
  }
  auto &store = _chat.store();
  const auto quote = (reply->quote_ && reply->quote_->text_ && !reply->quote_->text_->text_.empty())
                         ? reply->quote_->text_.get()
                         : nullptr;
  // In a group with topics every message "replies" to the start of its topic: that is no reply, unless it quotes
  // it. A topic's id is the server id of that message, which is in the upper bits of a message id.
  const auto chat = store.chat(_chat.chat());
  const auto topic = Cast<api::messageTopicForum>(item->data->topic_id_.get());
  if (chat && chat->forum && topic && !quote && reply->message_id_ == (std::int64_t(topic->forum_topic_id_) << 20)) {
    return nullptr;
  }
  const auto preview = _chat.replyPreview(*item);
  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(box, "reply-quote");
  if (preview.color >= 0) {
    gtk_widget_add_css_class(box, PeerColorClass(preview.color).c_str());
  }
  // The name a message from another chat comes with, until there is better.
  const auto name = preview.name.empty() ? OriginName(store, reply->origin_.get()) : preview.name;
  _replyPending = _replyPending || !preview.ready;
  auto markup = std::string();
  if (quote) {
    markup = ToMarkup(*quote, false, false);
  } else if (!preview.ready) {
    markup = Escape(Tr("lng_profile_loading"));
  } else if (preview.text.empty()) {
    markup = Escape(Tr("lng_deleted_message"));
  } else {
    markup = Escape(preview.text);
  }
  // A single line draws line breaks as a sign.
  std::replace(markup.begin(), markup.end(), '\n', ' ');
  auto *nameLabel = gtk_label_new(name.c_str());
  gtk_widget_add_css_class(nameLabel, "reply-name");
  gtk_label_set_xalign(GTK_LABEL(nameLabel), 0);
  gtk_label_set_ellipsize(GTK_LABEL(nameLabel), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(box), nameLabel);
  auto *textLabel = gtk_label_new(nullptr);
  gtk_label_set_markup(GTK_LABEL(textLabel), markup.c_str());
  gtk_label_set_xalign(GTK_LABEL(textLabel), 0);
  gtk_label_set_ellipsize(GTK_LABEL(textLabel), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode(GTK_LABEL(textLabel), TRUE);
  gtk_box_append(GTK_BOX(box), textLabel);
  if (const auto target = preview.target) {
    auto *click = gtk_gesture_click_new();
    _signals.connect<void(GtkGestureClick *, int, double, double)>(
        click, "released", [=, this](GtkGestureClick *, int, double, double) {
          if (TapAllowed()) _chat.showAt(target);
        });
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));
  }
  return box;
}

GtkWidget *MessageWidget::buildMedia(Cell &cell, int maxWidth) {
  const auto content = ContentOf(cell.item);
  if (!content) {
    return nullptr;
  }
  switch (content->get_id()) {
    case api::messagePhoto::ID: {
      const auto &photo = static_cast<const api::messagePhoto *>(content)->photo_;
      return photo ? buildPhoto(cell, *photo, maxWidth) : nullptr;
    }
    case api::messageSticker::ID: {
      const auto &sticker = static_cast<const api::messageSticker *>(content)->sticker_;
      return sticker ? buildSticker(cell, *sticker) : nullptr;
    }
    case api::messageVoiceNote::ID: {
      const auto &voice = static_cast<const api::messageVoiceNote *>(content)->voice_note_;
      return (voice && voice->voice_) ? buildVoice(cell, *voice->voice_, voice->duration_, {}, true) : nullptr;
    }
    case api::messageAudio::ID: {
      const auto &audio = static_cast<const api::messageAudio *>(content)->audio_;
      if (!audio || !audio->audio_) return nullptr;
      auto title = audio->title_.empty() ? audio->file_name_ : audio->title_;
      if (!audio->performer_.empty()) {
        title = audio->performer_ + " – " + title;
      }
      return buildVoice(cell, *audio->audio_, audio->duration_, title, false);
    }
    case api::messageVideo::ID:
    case api::messageAnimation::ID: {
      const auto visual = VisualOf(content);
      return visual.file ? buildVideoThumb(cell, visual.width, visual.height, visual.mini, visual.thumbnail,
                                           *visual.file, visual.duration, visual.gif, false, maxWidth)
                         : nullptr;
    }
    case api::messageVideoNote::ID: {
      const auto &note = static_cast<const api::messageVideoNote *>(content)->video_note_;
      if (!note || !note->video_) return nullptr;
      return buildVideoThumb(cell, note->length_, note->length_, note->minithumbnail_.get(), note->thumbnail_.get(),
                             *note->video_, note->duration_, false, true, kRoundVideoWidth);
    }
    case api::messageDocument::ID: {
      const auto &document = static_cast<const api::messageDocument *>(content)->document_;
      if (!document || !document->document_) return nullptr;
      if (document->mime_type_.starts_with("image/") && document->thumbnail_) {
        const auto &thumbnail = *document->thumbnail_;
        return buildVideoThumb(cell, thumbnail.width_, thumbnail.height_, document->minithumbnail_.get(), &thumbnail,
                               *document->document_, 0, false, false, maxWidth);
      }
      return buildDocument(cell, *document->document_, document->file_name_, DocumentIcon(document->mime_type_));
    }
    case api::messageContact::ID: {
      const auto &contact = static_cast<const api::messageContact *>(content)->contact_;
      return contact ? buildContact(*contact) : nullptr;
    }
    case api::messagePoll::ID: {
      const auto &poll = static_cast<const api::messagePoll *>(content)->poll_;
      return poll ? buildPoll(*poll) : nullptr;
    }
    default: return nullptr;
  }
}

GtkWidget *MessageWidget::buildContact(const api::contact &contact) {
  const auto fullName = contact.last_name_.empty() ? contact.first_name_
                                                   : contact.first_name_ + ' ' + contact.last_name_;
  auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(box), adw_avatar_new(40, fullName.c_str(), TRUE));
  auto *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  auto *name = gtk_label_new(fullName.c_str());
  gtk_widget_add_css_class(name, "heading");
  gtk_label_set_xalign(GTK_LABEL(name), 0);
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(column), name);
  auto *phone = gtk_label_new(PhoneText(contact.phone_number_).c_str());
  gtk_label_set_selectable(GTK_LABEL(phone), FALSE);
  gtk_label_set_xalign(GTK_LABEL(phone), 0);
  gtk_label_set_ellipsize(GTK_LABEL(phone), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(column), phone);
  gtk_box_append(GTK_BOX(box), column);
  return box;
}

// The question and the answers; how they were voted once the user has voted or the poll is closed, as in 0.3.1.
// Voting isn't here.
GtkWidget *MessageWidget::buildPoll(const api::poll &poll) {
  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(box, "poll");
  if (poll.question_) {
    auto *question = MakeTextLabel(*poll.question_);
    gtk_widget_add_css_class(question, "heading");
    gtk_box_append(GTK_BOX(box), question);
  }
  const auto voted = std::ranges::any_of(poll.options_, [](const auto &option) { return option && option->is_chosen_; });
  const auto results = voted || poll.is_closed_;
  for (const auto &option : poll.options_) {
    if (!option) continue;
    auto *line = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(line), option->text_ ? MakeTextLabel(*option->text_) : MakeTextLabel(std::string()));
    if (results) {
      auto *bar = gtk_progress_bar_new();
      gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), std::clamp(option->vote_percentage_, 0, 100) / 100.);
      gtk_progress_bar_set_text(GTK_PROGRESS_BAR(bar), (std::to_string(option->vote_percentage_) + "%").c_str());
      gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(bar), TRUE);
      if (option->is_chosen_) gtk_widget_add_css_class(bar, "poll-chosen");
      gtk_box_append(GTK_BOX(line), bar);
    }
    gtk_box_append(GTK_BOX(box), line);
  }
  auto *votes = gtk_label_new(TrCount("lng_polls_votes_count", poll.total_voter_count_).c_str());
  gtk_widget_add_css_class(votes, "dim-label");
  gtk_widget_add_css_class(votes, "caption");
  gtk_label_set_xalign(GTK_LABEL(votes), 0);
  gtk_box_append(GTK_BOX(box), votes);
  return box;
}

// Photos and videos sent together: Telegram's own album layout, drawn with GTK.
GtkWidget *MessageWidget::buildAlbumGrid() {
  auto sizes = std::vector<Size>();
  sizes.reserve(_items.size());
  for (const auto &item : _items) {
    sizes.push_back(VisualSize(item));
  }
  const auto layout = LayoutAlbum(sizes, kMaxPhotoWidth, kAlbumMinWidth, kAlbumSpacing);
  auto rects = std::vector<Rect>();
  auto children = std::vector<GtkWidget *>();
  for (auto i = 0; i != int(_items.size()) && i != int(layout.size()); ++i) {
    const auto &rect = layout[i];
    auto &cell = addCell(_items[i]);
    children.push_back(buildGridCell(cell, rect.width, rect.height));
    rects.push_back(rect);
  }
  const auto size = AlbumSize(rects);
  return MakeAlbumGrid(children, std::move(rects), size);
}

GtkWidget *MessageWidget::buildGridCell(Cell &cell, int width, int height) {
  const auto item = cell.item;
  const auto content = ContentOf(item);
  auto *overlay = gtk_overlay_new();
  gtk_widget_add_css_class(overlay, "album-cell");
  gtk_widget_set_overflow(overlay, GTK_OVERFLOW_HIDDEN);
  cell.picture = MakeMediaPicture(width, height);
  cell.width = width;
  cell.height = height;
  gtk_overlay_set_child(GTK_OVERLAY(overlay), cell.picture);
  cell.status = makeStatus();
  const api::file *shown = nullptr;
  if (const auto message = Cast<api::messagePhoto>(content); message && message->photo_) {
    cell.kind = PictureKind::Photo;
    SetMiniThumbnail(cell.picture, message->photo_->minithumbnail_.get());
    if (const auto largest = LargestSize(*message->photo_)) {
      shown = largest->photo_.get();
      cell.pictureFile = shown->id_;
      openOnTap(overlay, item, shown->id_);
      watchPicture(cell, shown);
    }
  } else if (const auto visual = VisualOf(content); visual.file) {
    cell.kind = PictureKind::Thumbnail;
    SetMiniThumbnail(cell.picture, visual.mini);
    shown = visual.file;
    cell.documentFile = shown->id_;
    cell.fallbackStatus = FallbackStatus(visual.duration, visual.gif);
    addPlayIcon(cell, overlay);
    openOnTap(overlay, item, shown->id_);
    if (visual.thumbnail && visual.thumbnail->file_) {
      cell.pictureFile = visual.thumbnail->file_->id_;
      watchPicture(cell, visual.thumbnail->file_.get());
    }
  }
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), cell.status);
  cell.widget = overlay;
  setupGestures(overlay, item, false);
  if (shown) updateStatus(cell, FileState(_chat.store(), *shown));
  return overlay;
}

GtkWidget *MessageWidget::makeStatus() {
  auto *status = gtk_label_new(nullptr);
  gtk_widget_add_css_class(status, "osd");
  gtk_widget_add_css_class(status, "media-status");
  gtk_widget_set_halign(status, GTK_ALIGN_START);
  gtk_widget_set_valign(status, GTK_ALIGN_START);
  gtk_widget_set_can_target(status, FALSE);
  gtk_label_set_ellipsize(GTK_LABEL(status), PANGO_ELLIPSIZE_END);
  gtk_widget_set_visible(status, FALSE);
  return status;
}

// `preview`: the picture of a link preview, which stays small; a tap on it still opens the largest size.
GtkWidget *MessageWidget::buildPhoto(Cell &cell, const api::photo &photo, int maxWidth, bool preview) {
  const auto largest = LargestSize(photo);
  const auto size = largest ? FitPhoto(largest->width_, largest->height_, maxWidth) : FitPhoto(0, 0, maxWidth);
  auto *overlay = gtk_overlay_new();
  cell.picture = MakeMediaPicture(size.width, size.height);
  cell.width = size.width;
  cell.height = size.height;
  gtk_widget_add_css_class(cell.picture, "message-photo");
  gtk_overlay_set_child(GTK_OVERLAY(overlay), MakeMediaFrame(cell.picture, size.width, size.height));
  cell.kind = PictureKind::Photo;
  SetMiniThumbnail(cell.picture, photo.minithumbnail_.get());
  cell.status = makeStatus();
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), cell.status);
  const auto shown = preview ? FittingSize(photo, size.width, size.height) : largest;
  if (shown) {
    cell.pictureFile = shown->photo_->id_;
    openOnTap(cell.picture, cell.item, largest->photo_->id_);
    watchPicture(cell, shown->photo_.get());
    updateStatus(cell, FileState(_chat.store(), *shown->photo_));
  }
  return overlay;
}

GtkWidget *MessageWidget::buildSticker(Cell &cell, const api::sticker &sticker) {
  auto width = kStickerSize, height = kStickerSize;
  if (sticker.width_ > 0 && sticker.height_ > 0) {
    if (sticker.width_ > sticker.height_) {
      height = kStickerSize * sticker.height_ / sticker.width_;
    } else {
      width = kStickerSize * sticker.width_ / sticker.height_;
    }
  }
  cell.picture = MakeMediaPicture(width, height);
  cell.width = width;
  cell.height = height;
  gtk_picture_set_content_fit(GTK_PICTURE(cell.picture), GTK_CONTENT_FIT_CONTAIN);
  cell.kind = PictureKind::Sticker;
  const auto thumbnail = (sticker.thumbnail_ && sticker.thumbnail_->file_) ? sticker.thumbnail_->file_.get() : nullptr;
  // Animated stickers stay with their thumbnail.
  const auto isStatic = Cast<api::stickerFormatWebp>(sticker.format_.get()) != nullptr;
  if (isStatic && sticker.sticker_) {
    if (thumbnail) {
      cell.thumbnailFile = thumbnail->id_;
      watchPicture(cell, thumbnail);
    }
    cell.pictureFile = sticker.sticker_->id_;
    watchPicture(cell, sticker.sticker_.get());
  } else if (thumbnail) {
    cell.pictureFile = thumbnail->id_;
    watchPicture(cell, thumbnail);
  }
  auto *frame = MakeMediaFrame(cell.picture, width, height);
  gtk_widget_set_halign(frame, GTK_ALIGN_START);
  // A tap hands the sticker itself over, as a file: an animated one plays there.
  if (sticker.sticker_) {
    cell.documentFile = sticker.sticker_->id_;
    openOnTap(frame, cell.item, sticker.sticker_->id_);
  }
  return frame;
}

GtkWidget *MessageWidget::buildVideoThumb(Cell &cell, int width, int height, const api::minithumbnail *mini,
                                          const api::thumbnail *thumbnail, const api::file &file, int duration,
                                          bool gif, bool round, int maxWidth) {
  const auto size = FitPhoto(width, height, maxWidth);
  auto *overlay = gtk_overlay_new();
  cell.picture = MakeMediaPicture(size.width, size.height);
  cell.width = size.width;
  cell.height = size.height;
  gtk_widget_add_css_class(cell.picture, "message-photo");
  if (round) {
    gtk_widget_add_css_class(cell.picture, "round-video");
  }
  gtk_overlay_set_child(GTK_OVERLAY(overlay), MakeMediaFrame(cell.picture, size.width, size.height));
  cell.kind = PictureKind::Thumbnail;
  SetMiniThumbnail(cell.picture, mini);
  cell.documentFile = file.id_;
  cell.fallbackStatus = FallbackStatus(duration, gif);
  addPlayIcon(cell, overlay);
  cell.status = makeStatus();
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), cell.status);
  openOnTap(overlay, cell.item, file.id_);
  if (thumbnail && thumbnail->file_) {
    cell.pictureFile = thumbnail->file_->id_;
    watchPicture(cell, thumbnail->file_.get());
  }
  if (round) {
    gtk_widget_set_halign(overlay, GTK_ALIGN_START);  // Stays round in a wider bubble.
  }
  updateStatus(cell, FileState(_chat.store(), file));
  return overlay;
}

void MessageWidget::addPlayIcon(Cell &cell, GtkWidget *overlay) {
  const auto image = Cast<api::messageDocument>(ContentOf(cell.item)) != nullptr;
  auto *play = gtk_image_new_from_icon_name(image ? kImageIcon : kPlayIcon);
  gtk_image_set_pixel_size(GTK_IMAGE(play), 24);
  gtk_widget_add_css_class(play, "osd");
  gtk_widget_add_css_class(play, "media-play");
  gtk_widget_set_halign(play, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(play, GTK_ALIGN_CENTER);
  gtk_widget_set_can_target(play, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), play);
  cell.play = play;
  cell.ring = MakeProgressRing();
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), cell.ring);
}

GtkWidget *MessageWidget::buildDocument(Cell &cell, const api::file &file, const std::string &name, const char *icon) {
  cell.documentFile = file.id_;
  auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(box, "document");
  cell.documentButton = gtk_button_new_from_icon_name(icon);
  g_object_set_data(G_OBJECT(cell.documentButton), kIconKey, const_cast<char *>(icon));
  gtk_widget_add_css_class(cell.documentButton, "circular");
  gtk_widget_add_css_class(cell.documentButton, "document-button");
  gtk_widget_set_valign(cell.documentButton, GTK_ALIGN_CENTER);
  openOnTap(cell.documentButton, cell.item, file.id_);
  gtk_box_append(GTK_BOX(box), cell.documentButton);
  auto *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_valign(column, GTK_ALIGN_CENTER);
  auto *nameLabel = gtk_label_new(name.empty() ? Tr("lng_in_dlg_file").c_str() : name.c_str());
  gtk_label_set_xalign(GTK_LABEL(nameLabel), 0);
  gtk_label_set_ellipsize(GTK_LABEL(nameLabel), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars(GTK_LABEL(nameLabel), 28);
  gtk_widget_add_css_class(nameLabel, "heading");
  gtk_box_append(GTK_BOX(column), nameLabel);
  cell.loadProgress = gtk_progress_bar_new();
  gtk_widget_add_css_class(cell.loadProgress, "download-progress");
  gtk_widget_set_visible(cell.loadProgress, FALSE);
  gtk_box_append(GTK_BOX(column), cell.loadProgress);
  cell.documentStatus = gtk_label_new(nullptr);
  gtk_label_set_xalign(GTK_LABEL(cell.documentStatus), 0);
  gtk_widget_add_css_class(cell.documentStatus, "dim-label");
  gtk_widget_add_css_class(cell.documentStatus, "caption");
  gtk_box_append(GTK_BOX(column), cell.documentStatus);
  gtk_box_append(GTK_BOX(box), column);
  updateDocumentState(cell, FileState(_chat.store(), file));
  return box;
}

// Voice messages and music: the button, a title for music, the bar and the length, as in 0.3.1. A tap on the button
// plays or pauses, on the bar plays from there, a drag along it seeks.
GtkWidget *MessageWidget::buildVoice(Cell &cell, const api::file &file, int duration, const std::string &title,
                                     bool voiceNote) {
  cell.documentFile = file.id_;
  cell.voice = true;
  cell.voiceNote = voiceNote;
  cell.duration = duration;
  auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(box, "voice");
  cell.documentButton = gtk_button_new_from_icon_name(kPlayIcon);
  g_object_set_data(G_OBJECT(cell.documentButton), kIconKey, const_cast<char *>(kPlayIcon));
  gtk_widget_add_css_class(cell.documentButton, "circular");
  gtk_widget_add_css_class(cell.documentButton, "suggested-action");
  gtk_widget_set_valign(cell.documentButton, GTK_ALIGN_CENTER);
  const auto raw = &cell;
  const auto play = [this, raw] {
    if (!TapAllowed()) return;
    if (raw->item->sponsored) _chat.sponsoredClicked(*raw->item, true);
    if (auto *player = _chat.player()) player->playPause(trackOf(*raw));
  };
  _taps.push_back(play);
  _signals.clicked(cell.documentButton, play);
  gtk_box_append(GTK_BOX(box), cell.documentButton);
  auto *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_valign(column, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(column, TRUE);
  if (!title.empty()) {
    auto *name = gtk_label_new(title.c_str());
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(name), 26);
    gtk_widget_add_css_class(name, "heading");
    gtk_box_append(GTK_BOX(column), name);
  }
  cell.voiceProgress = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0., 1., 0.001);
  gtk_scale_set_draw_value(GTK_SCALE(cell.voiceProgress), FALSE);
  gtk_range_set_restrict_to_fill_level(GTK_RANGE(cell.voiceProgress), FALSE);
  gtk_widget_set_focusable(cell.voiceProgress, FALSE);
  gtk_widget_add_css_class(cell.voiceProgress, "voice-progress");
  DisableControllers(cell.voiceProgress);
  setupSeeking(cell);
  gtk_box_append(GTK_BOX(column), cell.voiceProgress);
  cell.documentStatus = gtk_label_new(FormatDuration(duration).c_str());
  gtk_label_set_xalign(GTK_LABEL(cell.documentStatus), 0);
  gtk_widget_add_css_class(cell.documentStatus, "dim-label");
  gtk_widget_add_css_class(cell.documentStatus, "caption");
  gtk_box_append(GTK_BOX(column), cell.documentStatus);
  gtk_box_append(GTK_BOX(box), column);
  // The first bar makes the player: it holds no thread and no sound device until something plays.
  if (auto *player = _chat.window().player()) {
    if (!std::exchange(_playerWatched, true)) {
      player->onState([this](const AudioPlayer::State &state) { updatePlayback(state); }, _playerLifetime);
    }
  }
  updateDocumentState(cell, FileState(_chat.store(), file));
  return box;
}

AudioPlayer::Track MessageWidget::trackOf(const Cell &cell) const {
  const auto &message = *cell.item->data;
  return {message.chat_id_, message.id_, cell.documentFile, cell.duration, cell.voiceNote};
}

void MessageWidget::updatePlayback(const AudioPlayer::State &state) {
  using Status = AudioPlayer::Status;
  for (const auto &cell : _cells) {
    // While the finger or the mouse moves the position, the bar shows where it is.
    if (!cell->voiceProgress || cell->seeking) continue;
    const auto mine = state.status != Status::Stopped && state.track.same(trackOf(*cell));
    const auto sending = cell->item->data
                         && Cast<api::messageSendingStatePending>(cell->item->data->sending_state_.get());
    const auto playing = mine && state.status != Status::Paused;
    const auto icon = sending ? kClockIcon : playing ? "media-playback-pause-symbolic" : kPlayIcon;
    gtk_button_set_icon_name(GTK_BUTTON(cell->documentButton), icon);
    const auto range = GTK_RANGE(cell->voiceProgress);
    auto text = std::string();
    if (mine && state.status != Status::Loading && state.length > 0.) {
      gtk_range_set_value(range, std::clamp(state.position / state.length, 0., 1.));
      gtk_range_set_show_fill_level(range, state.buffered < 1.);
      gtk_range_set_fill_level(range, state.buffered);
      text = FormatDuration(int(state.position)) + " / " + FormatDuration(int(std::lround(state.length)));
      if (playing && state.waiting) text = LoadingText() + " " + text;
    } else {
      // Started and not playing yet: the start of the file is being downloaded.
      gtk_range_set_value(range, mine ? std::clamp(state.position / std::max(state.length, 1.), 0., 1.) : 0.);
      gtk_range_set_show_fill_level(range, FALSE);
      text = mine ? LoadingText() : FormatDuration(cell->duration);
    }
    gtk_label_set_text(GTK_LABEL(cell->documentStatus), text.c_str());
  }
}

// A tap on the bar of a voice message or a song plays it from there, a drag along it moves the position (the player
// pauses while it moves). A drag that starts vertically scrolls the chat.
void MessageWidget::setupSeeking(Cell &cell) {
  auto *bar = cell.voiceProgress;
  const auto raw = &cell;
  const auto show = [this, raw, bar](double fraction) {
    gtk_range_set_value(GTK_RANGE(bar), fraction);
    const auto *player = _chat.window().player();
    const auto length = (player && player->current(trackOf(*raw)) && player->state().length > 0.)
                            ? player->state().length
                            : double(raw->duration);
    if (length > 0.) {
      const auto text = FormatDuration(int(fraction * length)) + " / " + FormatDuration(int(std::lround(length)));
      gtk_label_set_text(GTK_LABEL(raw->documentStatus), text.c_str());
    }
  };
  const auto seek = [this, raw](double fraction) {
    if (auto *player = _chat.player()) player->seek(trackOf(*raw), fraction);
  };
  auto *click = gtk_gesture_click_new();
  _signals.connect<void(GtkGestureClick *, int, double, double)>(
      click, "released", [=](GtkGestureClick *, int, double x, double) {
        if (!TapAllowed() || raw->seeking) return;
        const auto fraction = SeekFraction(x, gtk_widget_get_width(bar));
        show(fraction);
        seek(fraction);
      });
  gtk_widget_add_controller(bar, GTK_EVENT_CONTROLLER(click));
  auto *pan = gtk_gesture_pan_new(GTK_ORIENTATION_HORIZONTAL);
  const auto position = [bar](GtkGestureDrag *drag) {
    double x = 0, y = 0, dx = 0, dy = 0;
    gtk_gesture_drag_get_start_point(drag, &x, &y);
    gtk_gesture_drag_get_offset(drag, &dx, &dy);
    return SeekFraction(x + dx, gtk_widget_get_width(bar));
  };
  _signals.connect<void(GtkGesturePan *, GtkPanDirection, double)>(
      pan, "pan", [=, this](GtkGesturePan *gesture, GtkPanDirection, double) {
        if (!raw->seeking) {
          // Wins over the swipe to reply and the scrolling.
          gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
          raw->seeking = true;
          if (auto *player = _chat.window().player()) player->startSeeking(trackOf(*raw));
        }
        show(position(GTK_GESTURE_DRAG(gesture)));
      });
  _signals.connect<void(GtkGestureDrag *, double, double)>(pan, "drag-end", [=](GtkGestureDrag *gesture, double, double) {
    if (std::exchange(raw->seeking, false)) seek(position(gesture));
  });
  _signals.connect<void(GtkGesture *, GdkEventSequence *)>(pan, "cancel", [=, this](GtkGesture *, GdkEventSequence *) {
    if (!std::exchange(raw->seeking, false)) return;
    if (auto *player = _chat.window().player()) player->cancelSeeking(trackOf(*raw));
  });
  gtk_widget_add_controller(bar, GTK_EVENT_CONTROLLER(pan));
}

// The message goes (or is built again) while its bar is being dragged: the player mustn't stay paused for it.
void MessageWidget::cancelSeeking() {
  auto *player = _chat.window().player();
  for (const auto &cell : _cells) {
    if (std::exchange(cell->seeking, false) && player) player->cancelSeeking(trackOf(*cell));
  }
}

GtkWidget *MessageWidget::buildWebPage(MessageItem *item) {
  const auto message = Cast<api::messageText>(ContentOf(item));
  const auto page = message ? message->link_preview_.get() : nullptr;
  if (!page) {
    return nullptr;
  }
  const auto photo = PreviewPhoto(page->type_.get());
  const auto description = (page->description_ && !page->description_->text_.empty()) ? page->description_.get()
                                                                                      : nullptr;
  if (page->title_.empty() && !description && !photo) {
    return nullptr;
  }
  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class(box, "webpage");
  if (!page->site_name_.empty()) {
    auto *site = gtk_label_new(page->site_name_.c_str());
    gtk_widget_add_css_class(site, "webpage-site");
    gtk_label_set_xalign(GTK_LABEL(site), 0);
    gtk_label_set_ellipsize(GTK_LABEL(site), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), site);
  }
  if (!page->title_.empty()) {
    auto *title = gtk_label_new(page->title_.c_str());
    gtk_widget_add_css_class(title, "heading");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_label_set_wrap(GTK_LABEL(title), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(title), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(title), 44);
    gtk_box_append(GTK_BOX(box), title);
  }
  if (description) {
    // A cut description goes without its entities: they count in the whole text.
    const auto cut = Utf8Offset(description->text_, kDescriptionLimit);
    gtk_box_append(GTK_BOX(box), cut < description->text_.size()
                                     ? MakeTextLabel(description->text_.substr(0, cut) + "…")
                                     : MakeTextLabel(*description));
  }
  if (photo) {
    gtk_box_append(GTK_BOX(box), buildPhoto(addCell(item), *photo, kMaxPhotoWidth - 20, true));
  }
  auto *click = gtk_gesture_click_new();
  _signals.connect<void(GtkGestureClick *, int, double, double)>(
      click, "released", [=, url = page->url_](GtkGestureClick *, int, double x, double y) {
        // Taps on the photo open the viewer, the rest opens the page.
        if (TapAllowed() && !url.empty() && !GTK_IS_PICTURE(gtk_widget_pick(box, x, y, GTK_PICK_DEFAULT))) {
          HandleLink(url);
        }
      });
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));
  return box;
}

// A sponsored message of a channel, as 0.3.1 drew it: its text, then a block like a link preview with "Ad" (or
// "Recommended"), the title, the media, the button and what the advertiser has to say. No reactions, time or
// menu. Every way of opening it is reported to TDLib (ChatView::sponsoredClicked) before the link opens.
GtkWidget *MessageWidget::buildSponsored(MessageItem *item) {
  const auto &sponsored = *item->sponsored;
  auto *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(line, GTK_ALIGN_START);
  gtk_widget_set_margin_start(line, 8);
  gtk_widget_set_margin_end(line, 8);
  gtk_widget_set_margin_top(line, 6);
  gtk_widget_set_margin_bottom(line, 2);
  auto *bubble = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(bubble, "bubble");
  gtk_widget_add_css_class(bubble, "incoming");
  gtk_widget_add_css_class(bubble, "sponsored");
  gtk_box_append(GTK_BOX(line), MakeHeightForWidthBin(bubble));
  if (const auto text = TextOf(item)) {
    gtk_box_append(GTK_BOX(bubble), MakeTextLabel(*text));
  }
  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class(box, "webpage");
  auto *site = gtk_label_new(
      Tr(sponsored.is_recommended_ ? "lng_recommended_message_title" : "lng_sponsored_message_title").c_str());
  gtk_widget_add_css_class(site, "webpage-site");
  gtk_label_set_xalign(GTK_LABEL(site), 0);
  gtk_box_append(GTK_BOX(box), site);
  if (!sponsored.title_.empty()) {
    auto *title = gtk_label_new(sponsored.title_.c_str());
    gtk_widget_add_css_class(title, "heading");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_label_set_wrap(GTK_LABEL(title), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(title), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(title), 44);
    gtk_box_append(GTK_BOX(box), title);
  }
  if (auto *media = buildMedia(addCell(item), kMaxPhotoWidth - 20)) {
    gtk_box_append(GTK_BOX(box), media);
  }
  const auto url = sponsored.sponsor_ ? sponsored.sponsor_->url_ : std::string();
  // HandleLink() may open another chat, and this row goes: the report comes first.
  const auto open = [this, item, url] {
    _chat.sponsoredClicked(*item, false);
    HandleLink(url);
  };
  if (!url.empty()) {
    auto *button = gtk_button_new_with_label(
        (sponsored.button_text_.empty() ? Tr("lng_open_link") : sponsored.button_text_).c_str());
    gtk_widget_add_css_class(button, "sponsored-button");
    gtk_widget_set_margin_top(button, 4);
    _signals.clicked(button, [open] {
      if (TapAllowed()) open();
    });
    _taps.push_back(open);
    gtk_box_append(GTK_BOX(box), button);
  }
  for (const auto *info : {&sponsored.additional_info_, sponsored.sponsor_ ? &sponsored.sponsor_->info_ : nullptr}) {
    if (!info || info->empty()) continue;
    auto *label = MakeTextLabel(*info);
    gtk_widget_add_css_class(label, "dim-label");
    gtk_widget_add_css_class(label, "caption");
    gtk_box_append(GTK_BOX(box), label);
  }
  auto *click = gtk_gesture_click_new();
  _signals.connect<void(GtkGestureClick *, int, double, double)>(
      click, "released", [=](GtkGestureClick *, int, double x, double y) {
        // The button and the media have their own taps.
        auto *target = gtk_widget_pick(box, x, y, GTK_PICK_DEFAULT);
        if (TapAllowed() && !url.empty() && !InteractiveTarget(target, box)) open();
      });
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));
  gtk_box_append(GTK_BOX(bubble), box);
  return line;
}

// The reactions belong to the message of the bubble as a whole, as in Telegram's apps.
bool MessageWidget::refreshReactions() {
  const auto item = mainItem();
  if (!_reactions || !item->data) return false;
  auto layout = ReactionsLayoutKey(_chat, *item->data);
  auto counts = ReactionsCountKey(*item->data);
  if (layout == _reactionsLayout) {
    if (counts != _reactionsCounts) {
      _reactionsCounts = std::move(counts);
      UpdateReactionsBar(gtk_widget_get_first_child(_reactions), *item->data);
    }
    return false;
  }
  _reactionsLayout = std::move(layout);
  _reactionsCounts = std::move(counts);
  ClearBox(_reactions);
  auto *bar = MakeReactionsBar(_chat, *item->data);
  if (bar) {
    gtk_box_append(GTK_BOX(_reactions), bar);
    ++reactionBarBuilds;
  }
  gtk_widget_set_visible(_reactions, bar != nullptr);
  return true;
}

std::uint64_t MessageWidget::BuildCount() {
  return rowBuilds;
}

std::uint64_t MessageWidget::RebuildCount() {
  return rowRebuilds;
}

std::uint64_t MessageWidget::ReactionBarCount() {
  return reactionBarBuilds;
}

// A double tap on a message sets the quick reaction, as in the phone apps.
void MessageWidget::setupDoubleTap(GtkWidget *bubble) {
  auto *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
  _signals.connect<void(GtkGestureClick *, int, double, double)>(
      click, "pressed", [=, this](GtkGestureClick *, int count, double x, double y) {
        if (count != 2 || !TapAllowed()) return;
        if (InteractiveTarget(gtk_widget_pick(bubble, x, y, GTK_PICK_DEFAULT), bubble)) return;
        if (const auto item = mainItem(); item->data) ToggleQuickReaction(_chat, *item->data);
      });
  gtk_widget_add_controller(bubble, GTK_EVENT_CONTROLLER(click));
}

GtkWidget *MessageWidget::buildFooter() {
  auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_halign(box, GTK_ALIGN_END);
  gtk_widget_add_css_class(box, "message-footer");
  _footerTime = gtk_label_new(nullptr);
  gtk_label_set_ellipsize(GTK_LABEL(_footerTime), PANGO_ELLIPSIZE_START);
  gtk_box_append(GTK_BOX(box), _footerTime);
  _footerStatus = gtk_image_new();
  gtk_image_set_pixel_size(GTK_IMAGE(_footerStatus), 14);
  gtk_widget_add_css_class(_footerStatus, "message-status");
  gtk_box_append(GTK_BOX(box), _footerStatus);
  _footer = box;
  refreshFooter();
  return box;
}

// An album's time is its last message's; it is sent when all of it is, read when all of it is.
void MessageWidget::refreshFooter() {
  const auto last = _items.back();
  if (!_footer || !last->data) {
    return;
  }
  auto &store = _chat.store();
  const auto chat = store.chat(_chat.chat());
  const auto broadcast = chat && chat->kind == Td::ChatKind::Channel;
  const auto any = [&](auto check) {
    return std::any_of(_items.begin(), _items.end(),
                       [&](const MessageItem *item) { return item->data && check(*item->data); });
  };
  auto text = std::string();
  if (any([](const api::message &message) { return message.edit_date_ > 0; })) {
    text = Tr("lng_edited") + ' ';
  }
  const auto info = last->data->interaction_info_.get();
  if (const auto views = info ? info->view_count_ : 0; views > 0 && broadcast) {
    text = "\xF0\x9F\x91\x81 " + ShortCount(views) + "  " + text;
  }
  if (!last->data->author_signature_.empty()) {
    text = last->data->author_signature_ + ", " + text;
  }
  text += Td::FormatTime(last->data->date_);
  gtk_label_set_text(GTK_LABEL(_footerTime), text.c_str());

  const char *icon = nullptr;
  if (last->data->is_outgoing_ && !broadcast) {
    const auto self = chat && chat->kind == Td::ChatKind::Private && chat->peerId == store.selfId();
    const auto readOutbox = chat ? chat->readOutbox : 0;
    if (any([](const api::message &message) {
          return Cast<api::messageSendingStateFailed>(message.sending_state_.get()) != nullptr;
        })) {
      icon = "dialog-error-symbolic";
    } else if (any([](const api::message &message) { return message.sending_state_ != nullptr; })) {
      icon = kClockIcon;
    } else if (!self && any([&](const api::message &message) { return message.id_ > readOutbox; })) {
      icon = "contrail-check-symbolic";
    } else {
      icon = "contrail-check-double-symbolic";
    }
  }
  gtk_widget_set_visible(_footerStatus, icon != nullptr);
  if (icon) {
    gtk_image_set_from_icon_name(GTK_IMAGE(_footerStatus), icon);
  }
}

// The file of a picture this cell shows: there it is, or it is asked for. The caller has put its id in the cell.
void MessageWidget::watchPicture(Cell &cell, const api::file *file) {
  if (!file) return;
  auto &store = _chat.store();
  WantPicture(file->id_);
  const auto state = FileState(store, *file);
  if (state.complete) {
    showPicture(cell, state);
  } else {
    store.download(file->id_, kPictureDownloadPriority);
  }
}

void MessageWidget::showPicture(Cell &cell, const Td::File &file) {
  if (!cell.picture || cell.pictureFinal || !file.complete || file.path.empty()) {
    return;
  }
  const auto quality = (file.id == cell.pictureFile) ? kQualityFull
                       : (file.id == cell.thumbnailFile) ? kQualityThumbnail
                                                         : PictureQuality(0);
  if (!quality || quality < cell.sourceQuality) return;
  // The same file again (TDLib tells about a complete file more than once): it is shown already.
  if (quality == cell.sourceQuality && file.path == cell.sourcePath) return;
  cell.pictureFinal = (quality == kQualityFull);
  cell.sourcePath = file.path;
  cell.sourceQuality = quality;
  // A decode already running lands in the picture whatever happens to the cell.
  if (_picturesNear) LoadPicture(cell.picture, file.path, quality);
}

void MessageWidget::setPicturesNear(bool near) {
  if (near == _picturesNear) return;
  _picturesNear = near;
  for (const auto &cell : _cells) {
    if (!cell->picture || cell->sourcePath.empty()) continue;
    if (near) {
      LoadPicture(cell->picture, cell->sourcePath, PictureQuality(cell->sourceQuality));
    } else {
      if (g_getenv("CONTRAIL_DEBUG")) g_debug("Picture: dropped %s", cell->sourcePath.c_str());
      DropPicture(cell->picture);
    }
  }
}

void MessageWidget::fileChanged(const Td::File &file) {
  for (const auto &cell : _cells) {
    if (file.id == cell->pictureFile || file.id == cell->thumbnailFile) {
      showPicture(*cell, file);
    }
    if (file.id == cell->documentFile) {
      updateDocumentState(*cell, file);
    }
    updateStatus(*cell, file);
  }
}

void MessageWidget::updateDocumentState(Cell &cell, const Td::File &file) {
  if (!cell.documentStatus || file.id != cell.documentFile) {
    return;
  }
  if (cell.voiceProgress) {
    if (const auto *player = _chat.window().player()) {
      updatePlayback(player->state());
    } else {
      updatePlayback({});
    }
    return;
  }
  const auto downloading = file.downloading && !file.complete;
  const auto sending = cell.item->data && Cast<api::messageSendingStatePending>(cell.item->data->sending_state_.get());
  auto icon = static_cast<const char *>(g_object_get_data(G_OBJECT(cell.documentButton), kIconKey));
  auto status = cell.voice ? FormatDuration(cell.duration) : FormatSize(file.size);
  if (sending) {
    icon = kClockIcon;
  } else if (downloading) {
    status = DownloadText(file.downloaded, file.size);
    icon = "process-stop-symbolic";
  } else if (!file.complete && !cell.voice) {
    icon = "folder-download-symbolic";
  }
  gtk_label_set_text(GTK_LABEL(cell.documentStatus), status.c_str());
  if (cell.loadProgress) {
    gtk_widget_set_visible(cell.loadProgress, downloading);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(cell.loadProgress), DownloadFraction(file.downloaded, file.size));
  }
  gtk_button_set_icon_name(GTK_BUTTON(cell.documentButton), icon);
}

// Over a picture: how much of it is downloaded; a video's length otherwise.
void MessageWidget::updateStatus(Cell &cell, const Td::File &file) {
  const auto video = cell.ring && file.id == cell.documentFile;
  const auto photo = cell.kind == PictureKind::Photo && file.id == cell.pictureFile;
  if (!video && !photo) {
    return;
  }
  const auto downloading = file.downloading && !file.complete;
  if (cell.ring) {
    // A video downloading: how much of it came instead of the play button.
    gtk_widget_set_visible(cell.ring, downloading);
    if (cell.play) gtk_widget_set_visible(cell.play, !downloading);
    if (downloading) SetRingFraction(cell.ring, DownloadFraction(file.downloaded, file.size));
  }
  if (!cell.status) return;
  auto text = std::string();
  if (video) {
    text = cell.fallbackStatus;
    if (downloading) {
      text = DownloadText(file.downloaded, file.size);
      if (_chat.opensWhenLoaded(file.id)) {
        text += "\n";
        text += Translated("Opens when downloaded", "Откроется после загрузки", "Відкриється після завантаження");
      }
    }
  } else if (downloading) {
    text = Percent(DownloadFraction(file.downloaded, file.size));
  }
  gtk_label_set_text(GTK_LABEL(cell.status), text.c_str());
  gtk_widget_set_visible(cell.status, !text.empty());
}

void MessageWidget::setFirstInGroup(bool first) {
  if (_name) {
    gtk_widget_set_visible(_name, first);
  }
  if (_avatar) {
    gtk_widget_set_opacity(_avatar, first ? 1. : 0.);
  }
  if (auto *line = _content; line && !isService()) {
    gtk_widget_set_margin_top(line, first ? 6 : 1);
  }
}

// A reply or a link to one message of an album lights up its cell only.
void MessageWidget::highlight(const MessageItem *item) {
  if (_highlightTimeout) {
    g_source_remove(_highlightTimeout);
    _highlightTimeout = 0;
    if (_highlighted) gtk_widget_remove_css_class(_highlighted, "highlighted");
  }
  _highlighted = (item && _items.size() > 1) ? widgetFor(item) : _row;
  gtk_widget_add_css_class(_highlighted, "highlighted");
  _highlightTimeout = g_timeout_add(1500, +[](gpointer data) -> gboolean {
    auto *self = static_cast<MessageWidget *>(data);
    self->_highlightTimeout = 0;
    gtk_widget_remove_css_class(self->_highlighted, "highlighted");
    self->_highlighted = nullptr;
    return G_SOURCE_REMOVE;
  }, this);
}

// A tap hands the file over. Pictures that aren't a document open as photos, whichever size of them this was.
void MessageWidget::openOnTap(GtkWidget *widget, MessageItem *item, Td::FileId file) {
  const auto open = [=, this] {
    if (!TapAllowed()) return;
    if (item->sponsored) _chat.sponsoredClicked(*item, true);
    const auto document = std::any_of(_cells.begin(), _cells.end(),
                                      [&](const auto &cell) { return cell->documentFile == file; });
    if (document) {
      _chat.openDocument(*item, file);
    } else {
      _chat.openPhoto(*item, file);
    }
  };
  _taps.push_back(open);
  if (GTK_IS_BUTTON(widget)) {
    _signals.clicked(widget, open);
    return;
  }
  auto *click = gtk_gesture_click_new();
  _signals.connect<void(GtkGestureClick *, int, double, double)>(
      click, "released", [=](GtkGestureClick *, int, double, double) { open(); });
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(click));
}

// The menu belongs to the message under the finger: a cell of an album has its own, which wins over
// the bubble's (the bubble's is for the album as a whole).
void MessageWidget::setupGestures(GtkWidget *widget, MessageItem *item, bool swipe) {
  auto *longPress = gtk_gesture_long_press_new();
  gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(longPress), TRUE);
  _signals.connect<void(GtkGestureLongPress *, double, double)>(
      longPress, "pressed", [=, this](GtkGestureLongPress *gesture, double x, double y) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        _chat.showMessageMenu(*item, widget, x, y);
      });
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(longPress));
  auto *rightClick = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rightClick), GDK_BUTTON_SECONDARY);
  _signals.connect<void(GtkGestureClick *, int, double, double)>(
      rightClick, "pressed", [=, this](GtkGestureClick *gesture, int, double x, double y) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        _chat.showMessageMenu(*item, widget, x, y);
      });
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(rightClick));
  if (!swipe) return;
  // Swipe left to reply, as in the phone apps.
  // An unrestricted drag wins the touch sequence over the parent scrolled window. Pan denies
  // vertical movement early, so touching message text still scrolls the conversation.
  auto *pan = gtk_gesture_pan_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(pan), TRUE);
  _signals.connect<void(GtkGestureDrag *, double, double)>(
      pan, "drag-end", [=, this](GtkGestureDrag *, double dx, double dy) {
        if (dx < -80 && std::abs(dy) < 40) {
          _chat.replyTo(*item);
        }
      });
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(pan));
}

namespace {

// The widths of the bubbles and their texts under `widget`: a text wider than its bubble overflows it.
void DescribeSizes(GtkWidget *widget, std::string &out) {
  if (!gtk_widget_get_visible(widget)) return;
  if (gtk_widget_has_css_class(widget, "bubble")) {
    out += " bubble=" + std::to_string(gtk_widget_get_width(widget));
  } else if (gtk_widget_has_css_class(widget, "message-text")) {
    out += " text=" + std::to_string(gtk_widget_get_width(widget));
  }
  for (auto *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    DescribeSizes(child, out);
  }
}

bool TapTextIn(GtkWidget *widget, const std::string &part) {
  if (GTK_IS_LABEL(widget) && DebugTapLabel(widget, part)) return true;
  for (auto *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    if (TapTextIn(child, part)) return true;
  }
  return false;
}

}  // namespace

std::string MessageWidget::debugDescribe() const {
  auto &store = _chat.store();
  auto out = std::string("row=") + std::to_string(gtk_widget_get_width(_row)) + "x"
             + std::to_string(gtk_widget_get_height(_row)) + " taps=" + std::to_string(_taps.size());
  if (item()->sponsored) out += " sponsored";
  if (_footerStatus && gtk_widget_get_visible(_footerStatus)) {
    const auto icon = gtk_image_get_icon_name(GTK_IMAGE(_footerStatus));
    out += std::string(" status=") + (icon ? icon : "");
  }
  DescribeSizes(_row, out);
  for (const auto &cell : _cells) {
    out += " [";
    out += cell->kind == PictureKind::Photo       ? "photo"
           : cell->kind == PictureKind::Sticker   ? "sticker"
           : cell->kind == PictureKind::Thumbnail ? "thumbnail"
                                                  : "none";
    if (const auto id = cell->documentFile) {
      const auto known = store.file(id);
      out += " file=" + std::to_string(id);
      out += known ? (known->complete ? " complete" : known->downloading ? " downloading" : " stopped") : " unknown";
      if (known) out += " " + std::to_string(known->downloaded) + "/" + std::to_string(known->size);
      if (_chat.opensWhenLoaded(id)) out += " opens";
    }
    if (cell->status && gtk_widget_get_visible(cell->status)) {
      out += std::string(" status=\"") + gtk_label_get_text(GTK_LABEL(cell->status)) + "\"";
    }
    if (cell->ring && gtk_widget_get_visible(cell->ring)) {
      out += " ring=" + std::to_string(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell->ring), "contrail-fraction")));
    }
    if (cell->documentStatus) {
      out += std::string(" document=\"") + gtk_label_get_text(GTK_LABEL(cell->documentStatus)) + "\"";
    }
    if (cell->loadProgress && gtk_widget_get_visible(cell->loadProgress)) {
      out += " progress=" + std::to_string(int(gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(cell->loadProgress)) * 100));
    }
    if (cell->voiceProgress) {
      const auto range = GTK_RANGE(cell->voiceProgress);
      out += " bar=" + std::to_string(int(std::lround(gtk_range_get_value(range) * 1000)));
      if (gtk_range_get_show_fill_level(range)) {
        out += " fill=" + std::to_string(int(std::lround(gtk_range_get_fill_level(range) * 1000)));
      }
    }
    if (cell->documentButton) {
      const auto icon = gtk_button_get_icon_name(GTK_BUTTON(cell->documentButton));
      out += std::string(" icon=") + (icon ? icon : "");
    }
    out += "]";
  }
  return out;
}

void MessageWidget::debugTap(int index) {
  if (index < 0 || index >= int(_taps.size())) {
    g_message("chat tap: the message has %zu taps", _taps.size());
    return;
  }
  const auto tap = _taps[std::size_t(index)];  // The tap may build the row again.
  tap();
}

bool MessageWidget::debugTapText(const std::string &part) {
  return TapTextIn(_row, part);
}

}  // namespace Contrail
