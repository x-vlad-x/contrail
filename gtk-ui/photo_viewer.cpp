#include "photo_viewer.hpp"

#include "app.hpp"
#include "chat_view.hpp"
#include "main_window.hpp"
#include "picture_decode.hpp"
#include "td/strings.hpp"
#include "zoom_view.hpp"

#include <adwaita.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

namespace Contrail {
namespace {

namespace api = Td::api;

constexpr auto kDownloadPriority = 32;

// The viewer's textures alive, for the debug state: none once it is closed.
std::atomic<int> TexturesAlive = 0;

struct PhotoSize {
  Td::FileId file = 0;
  int width = 0;
  int height = 0;
  std::string path;  // Known complete from the message itself; the store knows the rest.
};

struct Viewer {
  Td::Store *store = nullptr;  // Outlives the window.
  ChatView *chat = nullptr;    // Only while chatAlive: the chat can go during the closing animation.
  std::weak_ptr<void> chatAlive;
  std::int64_t message = 0;
  std::vector<PhotoSize> sizes;  // Smallest first.
  std::string mini;              // The blurred JPEG of a few hundred bytes.
  GtkWidget *dialog = nullptr;
  GtkWidget *zoom = nullptr;
  GtkWidget *save = nullptr;
  int shown = -1;     // The size shown; -1 nothing, or the blurred thumbnail.
  int decoding = -1;  // The size being decoded.
  bool placed = false;  // The view started from the whole photo already: better pictures keep its place.
  bool closed = false;
  GCancellable *cancellable = nullptr;
  Td::Lifetime lifetime;
  ~Viewer() {
    if (cancellable) {
      g_cancellable_cancel(cancellable);
      g_object_unref(cancellable);
    }
  }
};

Viewer *Shown = nullptr;  // The viewer open, for the debug actions.

GdkTexture *Counted(GdkTexture *texture) {
  if (!texture) return nullptr;
  ++TexturesAlive;
  g_object_weak_ref(G_OBJECT(texture), +[](gpointer, GObject *) { --TexturesAlive; }, nullptr);
  return texture;
}

void SetTexture(Viewer &viewer, GdkTexture *texture) {
  const auto &largest = viewer.sizes.back();
  if (!viewer.placed) {
    viewer.placed = true;
    ZoomViewSetPhoto(viewer.zoom, texture, largest.width ? largest.width : gdk_texture_get_width(texture),
                     largest.height ? largest.height : gdk_texture_get_height(texture));
  } else {
    ZoomViewSetTexture(viewer.zoom, texture);
  }
}

std::string CompletePath(const Viewer &viewer, const PhotoSize &size) {
  if (const auto known = viewer.store->file(size.file); known && known->complete) return known->path;
  return size.path;
}

void ShowBest(Viewer &viewer);

void Decoded(GObject *source, GAsyncResult *result, gpointer) {
  auto *texture = static_cast<GdkTexture *>(g_task_propagate_pointer(G_TASK(result), nullptr));
  auto *viewer = static_cast<Viewer *>(g_object_get_data(source, "contrail-viewer"));
  if (!viewer || viewer->closed) {
    g_clear_object(&texture);
    return;
  }
  const auto index = std::exchange(viewer->decoding, -1);
  if (texture && index > viewer->shown) {
    viewer->shown = index;
    SetTexture(*viewer, texture);
  }
  g_clear_object(&texture);
  ShowBest(*viewer);  // A larger size may have come meanwhile.
}

// The best size downloaded, decoded off the main thread at its own size (a 2560 px photo is ~20 MB of texture):
// the GPU scales it.
void ShowBest(Viewer &viewer) {
  if (viewer.closed || viewer.decoding >= 0) return;
  for (auto index = int(viewer.sizes.size()) - 1; index > viewer.shown; --index) {
    const auto path = CompletePath(viewer, viewer.sizes[index]);
    if (path.empty()) continue;
    viewer.decoding = index;
    auto *task = g_task_new(viewer.zoom, viewer.cancellable, Decoded, nullptr);
    g_task_set_task_data(task, g_strdup(path.c_str()), g_free);
    g_task_run_in_thread(task, +[](GTask *task, gpointer, gpointer data, GCancellable *cancellable) {
      auto options = DecodeOptions();
      options.cancellable = cancellable;
      auto *pixbuf = DecodePictureFile(static_cast<const char *>(data), 0, 0, options);
      g_task_return_pointer(task, pixbuf ? Counted(TextureFromPixbuf(pixbuf)) : nullptr, g_object_unref);
    });
    g_object_unref(task);
    return;
  }
}

void Close(Viewer &viewer) {
  if (viewer.closed) return;
  viewer.closed = true;
  if (Shown == &viewer) Shown = nullptr;
  viewer.lifetime.reset();
  g_cancellable_cancel(viewer.cancellable);
  // The full size photo is the largest thing the window holds: it goes now, not when GTK finalizes the dialog.
  ZoomViewSetTexture(viewer.zoom, nullptr);
}

}  // namespace

void ShowPhotoViewer(ChatView &chat, std::int64_t message, const api::photo &photo) {
  auto data = std::make_unique<Viewer>();
  auto &viewer = *data;
  viewer.store = &chat.store();
  viewer.chat = &chat;
  viewer.chatAlive = chat.lifetime().token();
  viewer.message = message;
  for (const auto &size : photo.sizes_) {
    if (!size || !size->photo_) continue;
    const auto &file = *size->photo_;
    const auto complete = file.local_ && file.local_->is_downloading_completed_;
    viewer.sizes.push_back({file.id_, size->width_, size->height_, complete ? file.local_->path_ : std::string()});
  }
  if (viewer.sizes.empty()) return;
  std::stable_sort(viewer.sizes.begin(), viewer.sizes.end(), [](const PhotoSize &a, const PhotoSize &b) {
    return std::int64_t(a.width) * a.height < std::int64_t(b.width) * b.height;
  });
  if (photo.minithumbnail_) viewer.mini = photo.minithumbnail_->data_;
  viewer.cancellable = g_cancellable_new();

  auto *dialog = GTK_WIDGET(adw_dialog_new());
  viewer.dialog = dialog;
  adw_dialog_set_content_width(ADW_DIALOG(dialog), 1200);
  adw_dialog_set_content_height(ADW_DIALOG(dialog), 1200);
  adw_dialog_set_presentation_mode(ADW_DIALOG(dialog), ADW_DIALOG_FLOATING);
  auto *view = adw_toolbar_view_new();
  gtk_widget_add_css_class(view, "photo-viewer");
  adw_toolbar_view_set_extend_content_to_top_edge(ADW_TOOLBAR_VIEW(view), TRUE);
  auto *header = adw_header_bar_new();
  gtk_widget_add_css_class(header, "osd");
  auto *save = MakeIconButton("document-save-symbolic", Tr("lng_mediaview_save_as").c_str());
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), save);
  viewer.save = save;
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
  viewer.zoom = ZoomViewNew();
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), viewer.zoom);
  adw_dialog_set_child(ADW_DIALOG(dialog), view);
  // On the zoom view: a decode finishing after the dialog went finds nothing (GTask keeps the widget alive).
  auto *raw = AttachData(viewer.zoom, "contrail-viewer", std::move(data));

  // The blurred thumbnail at once, in the photo's own size, so that the view keeps its place for the rest.
  if (!raw->mini.empty()) {
    auto *bytes = g_bytes_new(raw->mini.data(), raw->mini.size());
    if (auto *texture = Counted(gdk_texture_new_from_bytes(bytes, nullptr))) {
      SetTexture(*raw, texture);
      g_object_unref(texture);
    }
    g_bytes_unref(bytes);
  }
  auto &store = chat.store();
  const auto largest = raw->sizes.back();
  if (CompletePath(*raw, largest).empty()) {
    store.onFile([raw](const Td::File &file) {
      if (!file.complete) return;
      for (const auto &size : raw->sizes) {
        if (size.file == file.id) {
          ShowBest(*raw);
          return;
        }
      }
    }, raw->lifetime);
    store.download(largest.file, kDownloadPriority);
  }
  ShowBest(*raw);

  OnClicked(save, [raw] {
    if (!raw->closed && !raw->chatAlive.expired()) raw->chat->saveFromViewer(raw->message);
  });
  Connect<void(AdwDialog *)>(dialog, "closed", [raw](AdwDialog *) { Close(*raw); });
  Shown = raw;
  chat.holdDialog(ADW_DIALOG(dialog));
  adw_dialog_present(ADW_DIALOG(dialog), chat.window().widget());
}

void PhotoViewerDebug(MainWindow &, const std::string &action) {
  if (action == "state") {
    if (!Shown) {
      g_message("viewer: open=0 textures=%d", TexturesAlive.load());
      return;
    }
    const auto &viewer = *Shown;
    const auto shown = viewer.shown >= 0 ? viewer.sizes[viewer.shown] : PhotoSize();
    g_message("viewer: open=1 message=%lld shown=%d/%d size=%dx%d loading=%d textures=%d %s",
              (long long)viewer.message, viewer.shown, int(viewer.sizes.size()), shown.width, shown.height,
              int(viewer.shown + 1 < int(viewer.sizes.size())), TexturesAlive.load(),
              ZoomViewState(viewer.zoom).c_str());
  } else if (action == "save") {
    if (Shown) g_signal_emit_by_name(Shown->save, "clicked");
  } else if (action == "close") {
    if (Shown) adw_dialog_close(ADW_DIALOG(Shown->dialog));
  } else if (Shown) {
    ZoomViewDebug(Shown->zoom, action);
  }
}

}  // namespace Contrail
