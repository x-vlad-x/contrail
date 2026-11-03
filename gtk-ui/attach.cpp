#include "attach.hpp"

#include "picture_decode.hpp"
#include "td/strings.hpp"

#include <adwaita.h>

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace Contrail {
namespace {

constexpr auto kPreviewSize = 312;  // A tile is 104 px wide, up to 3x.
constexpr auto kTileSize = 104;

const char *FileIcon(const AttachFile &file) {
  switch (file.type) {
    case AttachType::Music: return "audio-x-generic-symbolic";
    case AttachType::Video: return "video-x-generic-symbolic";
    case AttachType::Photo:
    case AttachType::Animation: return "image-x-generic-symbolic";
    default: return "text-x-generic-symbolic";
  }
}

std::string SizeText(std::int64_t size) {
  char buffer[32];
  if (size >= 1024 * 1024) {
    g_ascii_formatd(buffer, sizeof(buffer), "%.1f", double(size) / (1024 * 1024));
    return std::string(buffer) + " MB";
  }
  if (size >= 1024) {
    g_ascii_formatd(buffer, sizeof(buffer), "%.1f", double(size) / 1024);
    return std::string(buffer) + " KB";
  }
  return std::to_string(size) + " B";
}

// What a file is prepared from, and what it gives.
struct Job {
  std::string path;
  GdkTexture *image = nullptr;  // A pasted picture, written to `path` first.
  std::string directory;
  AttachFile file;
  GdkTexture *preview = nullptr;
  ~Job() {
    g_clear_object(&image);
    g_clear_object(&preview);
  }
};

void Prepare(Job &job) {
  if (job.image && !SaveTexturePng(job.image, job.path)) return;
  job.file = PrepareAttachFile(job.path, job.directory);
  if (job.image) job.file.name.clear();  // A pasted picture has no name of its own.
  const auto &file = job.file;
  const auto &source = file.type == AttachType::Photo     ? file.photoPath
                       : file.type == AttachType::Video   ? file.thumbnailPath
                       : file.type == AttachType::Animation ? file.path
                                                          : std::string();
  if (source.empty()) return;
  if (auto *pixbuf = DecodePictureFile(source.c_str(), kPreviewSize, kPreviewSize)) {
    job.preview = TextureFromPixbuf(pixbuf);
  }
}

}  // namespace

// --- reading the clipboard ---------------------------------------------------------------------------

ClipboardReader::Result::Result(Result &&other) noexcept
    : files(std::move(other.files)), image(std::exchange(other.image, nullptr)), text(std::move(other.text)) {}

ClipboardReader::Result &ClipboardReader::Result::operator=(Result &&other) noexcept {
  if (this != &other) {
    g_clear_object(&image);
    files = std::move(other.files);
    image = std::exchange(other.image, nullptr);
    text = std::move(other.text);
  }
  return *this;
}

ClipboardReader::Result::~Result() {
  g_clear_object(&image);
}

struct ClipboardReader::Pending {
  ClipboardReader *owner = nullptr;
  GdkClipboard *clipboard = nullptr;
  GCancellable *cancellable = nullptr;
  bool hasImage = false;
  bool hasText = false;
  std::function<void(Result &&)> done;
  Result result;

  ~Pending() {
    g_object_unref(cancellable);
    g_object_unref(clipboard);
  }
  // The reader is gone (another chat, a new paste): the answer is dropped.
  [[nodiscard]] bool abandoned() const { return !owner || g_cancellable_is_cancelled(cancellable); }
  void readFiles();
  void readImage();
  void readText();
  void finish();
};

void ClipboardReader::Pending::readFiles() {
  gdk_clipboard_read_value_async(
      clipboard, GDK_TYPE_FILE_LIST, G_PRIORITY_DEFAULT, cancellable,
      +[](GObject *source, GAsyncResult *res, gpointer data) {
        auto *self = static_cast<Pending *>(data);
        const auto *value = gdk_clipboard_read_value_finish(GDK_CLIPBOARD(source), res, nullptr);
        if (self->abandoned()) {
          delete self;
          return;
        }
        if (value) {
          auto *list = static_cast<GdkFileList *>(g_value_get_boxed(value));
          auto *files = list ? gdk_file_list_get_files(list) : nullptr;
          self->result.files = SplitPastedFiles(files);
          g_slist_free(files);
        }
        if (!self->result.files.local.empty() || !self->result.files.remote.empty()) {
          self->finish();
        } else if (self->hasImage) {
          self->readImage();
        } else {
          self->readText();
        }
      },
      this);
}

void ClipboardReader::Pending::readImage() {
  gdk_clipboard_read_texture_async(clipboard, cancellable, +[](GObject *source, GAsyncResult *res, gpointer data) {
    auto *self = static_cast<Pending *>(data);
    auto *texture = gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(source), res, nullptr);
    if (self->abandoned()) {
      g_clear_object(&texture);
      delete self;
      return;
    }
    self->result.image = texture;
    // A picture often comes with text (a spreadsheet's cells, an address): it goes to the field if the picture
    // isn't sent, as in Telegram Desktop.
    if (self->hasText || !self->result.image) {
      self->readText();
    } else {
      self->finish();
    }
  }, this);
}

void ClipboardReader::Pending::readText() {
  gdk_clipboard_read_text_async(clipboard, cancellable, +[](GObject *source, GAsyncResult *res, gpointer data) {
    auto *self = static_cast<Pending *>(data);
    auto *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), res, nullptr);
    if (self->abandoned()) {
      g_free(text);
      delete self;
      return;
    }
    if (text) {
      self->result.text = text;
      g_free(text);
    }
    self->finish();
  }, this);
}

void ClipboardReader::Pending::finish() {
  auto done = std::move(this->done);
  auto result = std::move(this->result);
  owner->_pending = nullptr;
  delete this;
  done(std::move(result));
}

ClipboardReader::ClipboardReader(GdkClipboard *clipboard, PasteKind kind, std::function<void(Result &&)> done)
    : _pending(new Pending()) {
  _pending->owner = this;
  _pending->clipboard = GDK_CLIPBOARD(g_object_ref(clipboard));
  _pending->cancellable = g_cancellable_new();
  _pending->done = std::move(done);
  auto *types = gdk_content_formats_union_deserialize_gtypes(gdk_content_formats_ref(gdk_clipboard_get_formats(clipboard)));
  _pending->hasImage = gdk_content_formats_contain_gtype(types, GDK_TYPE_TEXTURE);
  _pending->hasText = gdk_content_formats_contain_gtype(types, G_TYPE_STRING);
  gdk_content_formats_unref(types);
  switch (kind) {
    case PasteKind::Files: _pending->readFiles(); break;
    case PasteKind::Image: _pending->readImage(); break;
    case PasteKind::Text: _pending->readText(); break;
  }
}

ClipboardReader::~ClipboardReader() {
  if (_pending) {
    // The read pending deletes itself when GIO reports the cancellation.
    _pending->owner = nullptr;
    g_cancellable_cancel(_pending->cancellable);
  }
}

// --- the dialog ------------------------------------------------------------------------------------

struct AttachDialog::Entry {
  std::int64_t id = 0;
  std::string name;
  bool ready = false;      // Prepared.
  AttachFile file;
  GtkWidget *child = nullptr;  // In the flow box.
  GtkWidget *slot = nullptr;   // The preview, or a spinner.
};

std::weak_ptr<AttachDialog> AttachDialog::Show(Args &&args) {
  auto self = std::make_shared<AttachDialog>(std::move(args));
  auto result = std::weak_ptr<AttachDialog>(self);
  auto *dialog = self->_dialog;
  AttachData(dialog, "contrail-attach", std::make_unique<std::shared_ptr<AttachDialog>>(std::move(self)));
  if (const auto strong = result.lock(); strong && !strong->_entries.empty()) {
    adw_dialog_present(ADW_DIALOG(dialog), strong->_args.parent);
  } else {
    // Every file was refused: the window shows why, there is nothing to choose.
    g_object_ref_sink(dialog);
    g_object_unref(dialog);
  }
  return result;
}

AttachDialog::AttachDialog(Args &&args) : _args(std::move(args)), _cancellable(g_cancellable_new()) {
  _directory = AttachDirectory();
  build();
  if (_args.image) {
    prepareImage(std::exchange(_args.image, nullptr));
  } else {
    prepare(_args.paths);
  }
  refresh();
}

AttachDialog::~AttachDialog() {
  *_alive = false;
  g_cancellable_cancel(_cancellable);
  g_object_unref(_cancellable);
  g_clear_object(&_args.image);
  _signals.clear();
}

void AttachDialog::close() {
  if (_dialog && !_closing) {
    _closing = true;
    adw_dialog_force_close(ADW_DIALOG(_dialog));
  }
}

void AttachDialog::debugAction(const std::string &action) {
  if (action == "send") {
    send();
    return;
  } else if (action == "cancel") {
    close();
    return;
  } else if (action.starts_with("documents ")) {
    adw_switch_row_set_active(ADW_SWITCH_ROW(_asDocuments), action.ends_with("on"));
  } else if (action.starts_with("group ")) {
    adw_switch_row_set_active(ADW_SWITCH_ROW(_group), action.ends_with("on"));
  } else if (action.starts_with("caption ")) {
    const auto text = action.substr(8);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(_caption)), text.data(), int(text.size()));
  } else if (action.starts_with("remove ")) {
    const auto index = std::size_t(std::atoi(action.c_str() + 7));
    if (index < _entries.size()) {
      remove(_entries[index]->id);
      if (_entries.empty()) return;
    }
  }
  auto ready = 0;
  for (const auto &entry : _entries) {
    if (!entry->ready) continue;
    ++ready;
    const auto &file = entry->file;
    g_message("attach entry: type=%d size=%lld %dx%d duration=%d thumbnail=%d photo=%d name=%s", int(file.type),
              (long long)file.size, file.width, file.height, file.duration, int(!file.thumbnailPath.empty()),
              int(!file.photoPath.empty()), file.name.c_str());
  }
  g_message("attach state: entries=%zu ready=%d documents=%d documentsShown=%d group=%d groupShown=%d send=%d "
            "refused=\"%s\" title=\"%s\"",
            _entries.size(), ready, int(adw_switch_row_get_active(ADW_SWITCH_ROW(_asDocuments))),
            int(gtk_widget_get_visible(_asDocuments)), int(adw_switch_row_get_active(ADW_SWITCH_ROW(_group))),
            int(gtk_widget_get_visible(_group)), int(gtk_widget_get_sensitive(_sendButton)),
            gtk_widget_get_visible(_refused) ? gtk_label_get_text(GTK_LABEL(_refused)) : "",
            adw_dialog_get_title(ADW_DIALOG(_dialog)));
}

void AttachDialog::build() {
  _dialog = GTK_WIDGET(adw_dialog_new());
  adw_dialog_set_content_width(ADW_DIALOG(_dialog), 480);
  adw_dialog_set_content_height(ADW_DIALOG(_dialog), 560);
  gtk_widget_add_css_class(_dialog, "attach-dialog");
  auto *view = adw_toolbar_view_new();
  auto *header = adw_header_bar_new();
  adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
  adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
  auto *cancel = gtk_button_new_with_label(Tr("lng_cancel").c_str());
  _signals.clicked(cancel, [this] { close(); });
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
  _sendButton = gtk_button_new_with_label(Tr("lng_send_button").c_str());
  gtk_widget_add_css_class(_sendButton, "suggested-action");
  _signals.clicked(_sendButton, [this] { send(); });
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), _sendButton);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  // Shown here rather than as a toast: the dialog would cover it.
  _refused = gtk_label_new(nullptr);
  gtk_label_set_wrap(GTK_LABEL(_refused), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(_refused), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign(GTK_LABEL(_refused), 0);
  gtk_widget_add_css_class(_refused, "error");
  gtk_widget_set_visible(_refused, FALSE);
  gtk_box_append(GTK_BOX(box), _refused);
  _flow = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(_flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(_flow), TRUE);
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(_flow), 2);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(_flow), 6);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(_flow), 6);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(_flow), 6);
  gtk_widget_set_valign(_flow, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box), _flow);

  auto *options = adw_preferences_group_new();
  _asDocuments = adw_switch_row_new();
  _signals.connect<void(GObject *, GParamSpec *)>(_asDocuments, "notify::active",
                                                  [this](GObject *, GParamSpec *) { refresh(); });
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(options), _asDocuments);
  _group = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_group), Tr("lng_send_grouped").c_str());
  adw_switch_row_set_active(ADW_SWITCH_ROW(_group), TRUE);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(options), _group);
  gtk_box_append(GTK_BOX(box), options);

  auto *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroll);

  // The caption, at the bottom as the message field is: the keyboard comes up under it.
  auto *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(bottom, "composer");
  _caption = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(_caption), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(_caption), FALSE);
  gtk_text_view_set_input_hints(GTK_TEXT_VIEW(_caption),
                                GtkInputHints(GTK_INPUT_HINT_SPELLCHECK | GTK_INPUT_HINT_WORD_COMPLETION
                                              | GTK_INPUT_HINT_EMOJI | GTK_INPUT_HINT_UPPERCASE_SENTENCES));
  gtk_widget_add_css_class(_caption, "message-input");
  auto *captionScroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(captionScroll), GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(captionScroll), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(captionScroll), 120);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(captionScroll), 32);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(captionScroll), _caption);
  gtk_widget_set_hexpand(captionScroll, TRUE);
  gtk_widget_add_css_class(captionScroll, "message-input-frame");
  gtk_box_append(GTK_BOX(bottom), captionScroll);
  const auto placeholderText = Tr("lng_photo_caption");
  auto *placeholder = gtk_label_new(placeholderText.c_str());
  gtk_label_set_width_chars(GTK_LABEL(placeholder), std::min(int(g_utf8_strlen(placeholderText.c_str(), -1)), 24));
  gtk_widget_add_css_class(placeholder, "dim-label");
  gtk_widget_set_can_target(placeholder, FALSE);
  gtk_label_set_xalign(GTK_LABEL(placeholder), 0);
  gtk_text_view_add_overlay(GTK_TEXT_VIEW(_caption), placeholder, 0, 0);
  auto *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(_caption));
  _signals.connect<void(GtkTextBuffer *)>(buffer, "changed", [placeholder](GtkTextBuffer *changed) {
    gtk_widget_set_visible(placeholder, gtk_text_buffer_get_char_count(changed) == 0);
  });
  gtk_text_buffer_set_text(buffer, _args.caption.data(), int(_args.caption.size()));
  // Enter sends, Shift+Enter starts a new line, as in the message field.
  auto *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  _signals.connect<gboolean(GtkEventControllerKey *, guint, guint, GdkModifierType)>(
      keys, "key-pressed", [this](GtkEventControllerKey *, guint key, guint, GdkModifierType state) -> gboolean {
        if ((key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) && !(state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK))) {
          if (gtk_widget_get_sensitive(_sendButton)) send();
          return TRUE;
        }
        return FALSE;
      });
  gtk_widget_add_controller(_caption, keys);
  adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(view), bottom);
  adw_dialog_set_child(ADW_DIALOG(_dialog), view);
  adw_dialog_set_focus(ADW_DIALOG(_dialog), _caption);

  // Not through _signals, which holds what it connects to: the dialog holds this, and would never go.
  Connect<void(AdwDialog *)>(_dialog, "closed", [this](AdwDialog *) {
    _closing = true;
    g_cancellable_cancel(_cancellable);
    if (!_sent && _args.cancelled) {
      // The callback may destroy the chat, and the dialog with it: it is moved out first.
      auto cancelled = std::move(_args.cancelled);
      cancelled();
    }
  });
}

void AttachDialog::prepare(const std::vector<std::string> &paths) {
  auto refused = std::vector<std::string>();
  for (const auto &path : paths) {
    if (auto reason = AttachRefusal(path, _args.premium); !reason.empty()) {
      refused.push_back(std::move(reason));
      continue;
    }
    startPreparing(path, nullptr);
  }
  if (refused.empty()) return;
  if (_entries.empty()) {
    // Nothing to show: the window shows why.
    for (const auto &reason : refused) ShowToast(reason);
    return;
  }
  auto text = std::string();
  for (const auto &reason : refused) text += (text.empty() ? "" : "\n") + reason;
  gtk_label_set_text(GTK_LABEL(_refused), text.c_str());
  gtk_widget_set_visible(_refused, TRUE);
}

void AttachDialog::prepareImage(GdkTexture *image) {
  auto *name = g_strdup_printf("pasted-%lld.png", (long long)g_get_real_time());
  auto *path = g_build_filename(_directory.c_str(), name, nullptr);
  startPreparing(path, image);
  g_free(path);
  g_free(name);
}

void AttachDialog::startPreparing(const std::string &path, GdkTexture *image) {
  const auto id = ++_nextId;
  addEntry(id, image ? std::string() : path.substr(path.rfind('/') + 1));
  auto *job = new Job{.path = path, .image = image, .directory = _directory};
  struct Context {
    std::weak_ptr<bool> alive;
    AttachDialog *self = nullptr;
    std::int64_t id = 0;
  };
  auto *task = g_task_new(nullptr, _cancellable, +[](GObject *, GAsyncResult *result, gpointer data) {
    auto *context = static_cast<Context *>(data);
    auto *job = static_cast<Job *>(g_task_propagate_pointer(G_TASK(result), nullptr));
    if (job && !context->alive.expired() && *context->alive.lock()) {
      context->self->prepared(context->id, std::move(job->file), std::exchange(job->preview, nullptr));
    }
    delete job;
    delete context;
  }, new Context{_alive, this, id});
  g_task_set_task_data(task, job, nullptr);
  g_task_run_in_thread(task, +[](GTask *task, gpointer, gpointer data, GCancellable *cancellable) {
    auto *job = static_cast<Job *>(data);
    if (!g_cancellable_is_cancelled(cancellable)) Prepare(*job);
    // The cancelled ones go too: the callback frees the job.
    g_task_return_pointer(task, job, +[](gpointer pointer) { delete static_cast<Job *>(pointer); });
  });
  g_object_unref(task);
}

void AttachDialog::addEntry(std::int64_t id, const std::string &name) {
  auto entry = std::make_unique<Entry>();
  entry->id = id;
  entry->name = name;
  auto *overlay = gtk_overlay_new();
  gtk_widget_set_size_request(overlay, kTileSize, kTileSize);
  gtk_widget_add_css_class(overlay, "attach-tile");
  gtk_widget_set_overflow(overlay, GTK_OVERFLOW_HIDDEN);
  entry->slot = adw_bin_new();
  auto *spinner = adw_spinner_new();
  gtk_widget_set_size_request(spinner, 24, 24);
  adw_bin_set_child(ADW_BIN(entry->slot), spinner);
  gtk_overlay_set_child(GTK_OVERLAY(overlay), entry->slot);
  auto *remove = MakeIconButton("window-close-symbolic", Tr("lng_box_remove").c_str(), "osd");
  gtk_widget_add_css_class(remove, "circular");
  gtk_widget_add_css_class(remove, "attach-remove");
  gtk_widget_set_halign(remove, GTK_ALIGN_END);
  gtk_widget_set_valign(remove, GTK_ALIGN_START);
  _signals.clicked(remove, [this, id] { this->remove(id); });
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), remove);
  gtk_flow_box_append(GTK_FLOW_BOX(_flow), overlay);
  entry->child = gtk_widget_get_parent(overlay);
  gtk_widget_set_focusable(entry->child, FALSE);
  _entries.push_back(std::move(entry));
}

void AttachDialog::prepared(std::int64_t id, AttachFile file, GdkTexture *preview) {
  const auto i = std::find_if(_entries.begin(), _entries.end(), [&](const auto &e) { return e->id == id; });
  if (i == _entries.end() || _closing) {  // Removed meanwhile.
    g_clear_object(&preview);
    return;
  }
  (*i)->file = std::move(file);
  (*i)->ready = true;
  fillTile(**i, preview);
  g_clear_object(&preview);
  refresh();
}

void AttachDialog::fillTile(Entry &entry, GdkTexture *preview) {
  const auto &file = entry.file;
  GtkWidget *content = nullptr;
  if (preview) {
    auto *picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(preview));
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
    gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
    if (file.type == AttachType::Video) {
      auto *overlay = gtk_overlay_new();
      gtk_overlay_set_child(GTK_OVERLAY(overlay), picture);
      auto *play = gtk_image_new_from_icon_name("media-playback-start-symbolic");
      gtk_widget_add_css_class(play, "osd");
      gtk_widget_add_css_class(play, "media-play");
      gtk_widget_set_halign(play, GTK_ALIGN_CENTER);
      gtk_widget_set_valign(play, GTK_ALIGN_CENTER);
      gtk_overlay_add_overlay(GTK_OVERLAY(overlay), play);
      content = overlay;
    } else {
      content = picture;
    }
  } else {
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    auto *icon = gtk_image_new_from_icon_name(FileIcon(file));
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
    gtk_box_append(GTK_BOX(box), icon);
    auto *name = gtk_label_new(file.name.empty() ? Tr("lng_in_dlg_file").c_str() : file.name.c_str());
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(name), 10);
    gtk_widget_add_css_class(name, "caption");
    gtk_box_append(GTK_BOX(box), name);
    auto *size = gtk_label_new(SizeText(file.size).c_str());
    gtk_widget_add_css_class(size, "caption");
    gtk_widget_add_css_class(size, "dim-label");
    gtk_box_append(GTK_BOX(box), size);
    content = box;
  }
  adw_bin_set_child(ADW_BIN(entry.slot), content);
}

void AttachDialog::remove(std::int64_t id) {
  const auto i = std::find_if(_entries.begin(), _entries.end(), [&](const auto &e) { return e->id == id; });
  if (i == _entries.end()) return;
  gtk_flow_box_remove(GTK_FLOW_BOX(_flow), (*i)->child);
  _entries.erase(i);
  if (_entries.empty()) {
    close();
    return;
  }
  refresh();
}

void AttachDialog::refresh() {
  if (_entries.empty()) return;  // Every file was refused: Show() doesn't present it.
  auto ready = true, media = false, images = true;
  for (const auto &entry : _entries) {
    if (!entry->ready) {
      ready = false;
      images = false;
      continue;
    }
    const auto type = entry->file.type;
    media = media || type == AttachType::Photo || type == AttachType::Video || type == AttachType::Animation;
    images = images && type == AttachType::Photo;
  }
  const auto count = int(_entries.size());
  const auto asDocuments = adw_switch_row_get_active(ADW_SWITCH_ROW(_asDocuments));
  adw_dialog_set_title(ADW_DIALOG(_dialog),
                       TrCount((images && !asDocuments) ? "lng_send_images_selected" : "lng_send_files_selected", count)
                           .c_str());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_asDocuments),
                                Tr(count > 1 ? "lng_send_as_documents" : "lng_send_as_documents_one").c_str());
  gtk_widget_set_visible(_asDocuments, media);
  gtk_widget_set_visible(_group, count > 1);
  gtk_widget_set_sensitive(_sendButton, ready);
}

std::string AttachDialog::caption() const {
  auto *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(_caption));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  auto *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  auto result = std::string(text);
  g_free(text);
  const auto first = result.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  return result.substr(first, result.find_last_not_of(" \t\r\n") - first + 1);
}

void AttachDialog::send() {
  if (_sent || _entries.empty()) return;
  auto files = std::vector<AttachFile>();
  for (const auto &entry : _entries) {
    if (!entry->ready) return;
    files.push_back(entry->file);
  }
  const auto compress = !adw_switch_row_get_active(ADW_SWITCH_ROW(_asDocuments));
  const auto group = adw_switch_row_get_active(ADW_SWITCH_ROW(_group));
  if (_args.send && _args.send(files, compress, group, caption())) {
    _sent = true;
    close();
  }
  // Not sent: an error was shown, the files stay here.
}

}  // namespace Contrail
