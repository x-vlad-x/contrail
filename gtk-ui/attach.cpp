#include "attach.hpp"

#include "lang/lang_keys.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/chat/attach/attach_send_files_way.h"
#include "ui/text/format_values.h"

#include <QtCore/QFileInfo>

namespace Contrail {
namespace {

constexpr auto kPreviewWidth = 320;  // The thumbnails' source size, a tile is 104 px wide (up to 3x).
constexpr auto kTileSize = 104;

const char *FileIcon(const Ui::PreparedFile &file) {
  switch (file.type) {
    case Ui::PreparedFile::Type::Music: return "audio-x-generic-symbolic";
    case Ui::PreparedFile::Type::Video: return "video-x-generic-symbolic";
    case Ui::PreparedFile::Type::Photo: return "image-x-generic-symbolic";
    default: return "text-x-generic-symbolic";
  }
}

// Why a file can't be sent: the same reasons Telegram Desktop gives (Storage::PrepareMediaList).
QString RefusalReason(const QString &path, bool premium) {
  const auto info = QFileInfo(path);
  const auto size = info.size();
  if (info.isDir() || !info.exists() || !info.isFile() || !info.isReadable() || size <= 0) {
    return tr::lng_send_image_empty(tr::now, lt_name, info.fileName().isEmpty() ? path : info.fileName());
  } else if (size > kFileSizePremiumLimit || (size > kFileSizeLimit && !premium)) {
    const auto limit = premium ? kFileSizePremiumLimit : kFileSizeLimit;
    return info.fileName() + u": "_q + tr::lng_file_size_limit1(tr::now, lt_size, Ui::FormatSizeText(limit));
  }
  return QString();
}

}  // namespace

// --- reading the clipboard -------------------------------------------------------------------------

struct ClipboardReader::Pending {
  ClipboardReader *owner = nullptr;
  GdkClipboard *clipboard = nullptr;
  GCancellable *cancellable = nullptr;
  PasteKind kind = PasteKind::Text;
  bool hasImage = false;
  bool hasText = false;
  std::function<void(Result &&)> done;
  Result result;

  ~Pending() {
    g_object_unref(cancellable);
    g_object_unref(clipboard);
  }
  // The reader is gone (another chat, a new paste): drop the answer.
  [[nodiscard]] bool abandoned() const { return !owner || g_cancellable_is_cancelled(cancellable); }
  void readFiles();
  void readImage();
  void readText();
  void finish();
};

void ClipboardReader::Pending::readFiles() {
  gdk_clipboard_read_value_async(clipboard, GDK_TYPE_FILE_LIST, G_PRIORITY_DEFAULT, cancellable,
      +[](GObject *source, GAsyncResult *res, gpointer data) {
        auto *self = static_cast<Pending *>(data);
        const auto value = gdk_clipboard_read_value_finish(GDK_CLIPBOARD(source), res, nullptr);
        if (self->abandoned()) { delete self; return; }
        if (value) {
          auto *list = static_cast<GdkFileList *>(g_value_get_boxed(value));
          auto *files = list ? gdk_file_list_get_files(list) : nullptr;
          self->result.files = SplitPastedFiles(files);
          g_slist_free(files);
        }
        if (!self->result.files.local.isEmpty() || !self->result.files.remote.isEmpty()) {
          self->finish();
        } else if (self->hasImage) {
          self->readImage();
        } else {
          self->readText();
        }
      }, this);
}

void ClipboardReader::Pending::readImage() {
  gdk_clipboard_read_texture_async(clipboard, cancellable, +[](GObject *source, GAsyncResult *res, gpointer data) {
    auto *self = static_cast<Pending *>(data);
    auto *texture = gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(source), res, nullptr);
    if (self->abandoned()) {
      if (texture) g_object_unref(texture);
      delete self;
      return;
    }
    if (texture) {
      self->result.image = TextureToImage(texture);
      g_object_unref(texture);
    }
    // A picture often comes with text (a spreadsheet's cells, an address): it goes to the field if the
    // picture isn't sent, as in Telegram Desktop.
    if (self->hasText || self->result.image.isNull()) {
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
      self->result.text = Q(text);
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
  RunCoreEvent([&] { done(std::move(result)); });
}

ClipboardReader::ClipboardReader(GdkClipboard *clipboard, PasteKind kind, std::function<void(Result &&)> done)
    : _pending(new Pending()) {
  _pending->owner = this;
  _pending->clipboard = GDK_CLIPBOARD(g_object_ref(clipboard));
  _pending->cancellable = g_cancellable_new();
  _pending->kind = kind;
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
    // The pending read deletes itself when GIO reports the cancellation.
    _pending->owner = nullptr;
    g_cancellable_cancel(_pending->cancellable);
  }
}

// --- the dialog ------------------------------------------------------------------------------------

struct AttachDialog::Entry {
  int64 id = 0;
  QString name;
  std::unique_ptr<Ui::PreparedFile> file;  // Empty while it is prepared.
  GtkWidget *child = nullptr;              // In the flow box.
  GtkWidget *slot = nullptr;               // The preview, or a spinner.
};

base::weak_ptr<AttachDialog> AttachDialog::Show(Args &&args) {
  auto *self = new AttachDialog(std::move(args));
  AttachData(self->_dialog, "contrail-attach", std::unique_ptr<AttachDialog>(self));
  auto result = base::make_weak(self);
  adw_dialog_present(ADW_DIALOG(self->_dialog), self->_args.parent);
  return result;
}

AttachDialog::AttachDialog(Args &&args) : _args(std::move(args)) {
  build();
  if (!_args.image.isNull()) {
    prepareImage(std::move(_args.image));
  } else {
    prepare(_args.paths);
  }
  refresh();
}

AttachDialog::~AttachDialog() {
  invalidate_weak_ptrs(this);
  _signals.clear();
}

void AttachDialog::close() {
  if (_dialog) adw_dialog_force_close(ADW_DIALOG(_dialog));
}

void AttachDialog::debugAction(const QString &action) {
  if (action == u"send"_q) {
    send();
  } else if (action == u"cancel"_q) {
    close();
  } else if (action.startsWith(u"documents "_q)) {
    adw_switch_row_set_active(ADW_SWITCH_ROW(_asDocuments), action.endsWith(u"on"_q));
  } else if (action.startsWith(u"group "_q)) {
    adw_switch_row_set_active(ADW_SWITCH_ROW(_group), action.endsWith(u"on"_q));
  }
  auto prepared = 0;
  for (const auto &entry : _entries) {
    if (entry->file) {
      ++prepared;
      g_message("Attach entry: type=%d size=%lld preview=%dx%d name=%s", int(entry->file->type),
                (long long)entry->file->size, entry->file->preview.width(), entry->file->preview.height(),
                U8(entry->file->displayName).c_str());
    }
  }
  g_message("Attach state: entries=%d prepared=%d documents=%d group=%d send=%d", int(_entries.size()), prepared,
            int(adw_switch_row_get_active(ADW_SWITCH_ROW(_asDocuments))),
            int(adw_switch_row_get_active(ADW_SWITCH_ROW(_group))), int(gtk_widget_get_sensitive(_sendButton)));
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
  auto *cancel = gtk_button_new_with_label(U8(tr::lng_cancel(tr::now)).c_str());
  _signals.clicked(cancel, [this] { close(); });
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
  _sendButton = gtk_button_new_with_label(U8(tr::lng_send_button(tr::now)).c_str());
  gtk_widget_add_css_class(_sendButton, "suggested-action");
  _signals.clicked(_sendButton, [this] { send(); });
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), _sendButton);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  SetMarginAll(box, 12);
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
  _signals.connect<void(GObject *, GParamSpec *)>(_asDocuments, "notify::active", [this](GObject *, GParamSpec *) {
    refresh();
  });
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(options), _asDocuments);
  _group = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_group), U8(tr::lng_send_grouped(tr::now)).c_str());
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
  gtk_text_view_set_input_hints(GTK_TEXT_VIEW(_caption), GtkInputHints(GTK_INPUT_HINT_SPELLCHECK | GTK_INPUT_HINT_WORD_COMPLETION
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
  const auto placeholderText = tr::lng_photo_caption(tr::now);
  auto *placeholder = gtk_label_new(U8(placeholderText).c_str());
  gtk_label_set_width_chars(GTK_LABEL(placeholder), std::min(int(placeholderText.size()), 24));
  gtk_widget_add_css_class(placeholder, "dim-label");
  gtk_widget_set_can_target(placeholder, FALSE);
  gtk_label_set_xalign(GTK_LABEL(placeholder), 0);
  gtk_text_view_add_overlay(GTK_TEXT_VIEW(_caption), placeholder, 0, 0);
  auto *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(_caption));
  _signals.connect<void(GtkTextBuffer *)>(buffer, "changed", [placeholder](GtkTextBuffer *buffer) {
    gtk_widget_set_visible(placeholder, gtk_text_buffer_get_char_count(buffer) == 0);
  });
  const auto caption = _args.caption.toUtf8();
  gtk_text_buffer_set_text(buffer, caption.constData(), caption.size());
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

  _signals.connect<void(AdwDialog *)>(_dialog, "closed", [this](AdwDialog *) {
    if (!_sent && _args.cancelled) {
      // The callback may destroy the chat, and the dialog with it: keep it alive meanwhile.
      auto cancelled = std::move(_args.cancelled);
      cancelled();
    }
  });
}

void AttachDialog::prepare(const QStringList &paths) {
  auto refused = QStringList();
  for (const auto &path : paths) {
    if (const auto reason = RefusalReason(path, _args.premium); !reason.isEmpty()) {
      refused.push_back(reason);
      continue;
    }
    auto file = std::make_unique<Ui::PreparedFile>(path);
    file->size = QFileInfo(path).size();
    startPreparing(std::move(file));
  }
  if (refused.isEmpty()) {
  } else if (_entries.empty()) {
    // Nothing to show: the window shows why.
    for (const auto &reason : refused) ShowToast(reason);
  } else {
    gtk_label_set_text(GTK_LABEL(_refused), U8(refused.join('\n')).c_str());
    gtk_widget_set_visible(_refused, TRUE);
  }
}

void AttachDialog::prepareImage(QImage &&image) {
  auto file = std::make_unique<Ui::PreparedFile>(QString());
  file->information = std::make_unique<Ui::PreparedFileInformation>();
  file->id = ++_nextId;
  addEntry(file->id, QString());
  const auto id = file->id;
  const auto sideLimit = PhotoSideLimit();  // Read on the main thread.
  // The picture becomes a photo to send (encoding, the preview) in the background.
  crl::async([weak = base::make_weak(this), id, sideLimit, image = std::move(image), file = std::move(file)]() mutable {
    FileLoadTask::FillImageInformation(std::move(image), false, file->information);
    Storage::PrepareDetails(*file, kPreviewWidth, sideLimit);
    crl::on_main([weak, id, file = std::move(file)]() mutable {
      if (const auto strong = weak.get()) strong->prepared(id, std::move(file));
    });
  });
}

void AttachDialog::startPreparing(std::unique_ptr<Ui::PreparedFile> file) {
  file->id = ++_nextId;
  addEntry(file->id, file->displayName);
  const auto id = file->id;
  const auto sideLimit = PhotoSideLimit();  // Read on the main thread.
  crl::async([weak = base::make_weak(this), id, sideLimit, file = std::move(file)]() mutable {
    Storage::PrepareDetails(*file, kPreviewWidth, sideLimit);
    crl::on_main([weak, id, file = std::move(file)]() mutable {
      if (const auto strong = weak.get()) strong->prepared(id, std::move(file));
    });
  });
}

void AttachDialog::addEntry(int64 id, const QString &name) {
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
  auto *remove = MakeIconButton("window-close-symbolic", U8(tr::lng_box_remove(tr::now)).c_str(), "osd");
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

void AttachDialog::prepared(int64 id, std::unique_ptr<Ui::PreparedFile> file) {
  const auto i = std::find_if(_entries.begin(), _entries.end(), [&](const auto &e) { return e->id == id; });
  if (i == _entries.end()) return;  // Removed meanwhile.
  (*i)->file = std::move(file);
  fillTile(**i);
  refresh();
}

void AttachDialog::fillTile(Entry &entry) {
  const auto &file = *entry.file;
  GtkWidget *content = nullptr;
  if (!file.preview.isNull()) {
    auto *picture = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
    gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
    if (auto *texture = ToTexture(file.preview)) {
      gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(texture));
      g_object_unref(texture);
    }
    if (file.type == Ui::PreparedFile::Type::Video) {
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
    auto *name = gtk_label_new(U8(file.displayName.isEmpty() ? tr::lng_in_dlg_file(tr::now) : file.displayName).c_str());
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(name), 10);
    gtk_widget_add_css_class(name, "caption");
    gtk_box_append(GTK_BOX(box), name);
    auto *size = gtk_label_new(U8(Ui::FormatSizeText(file.size)).c_str());
    gtk_widget_add_css_class(size, "caption");
    gtk_widget_add_css_class(size, "dim-label");
    gtk_box_append(GTK_BOX(box), size);
    content = box;
  }
  adw_bin_set_child(ADW_BIN(entry.slot), content);
}

void AttachDialog::remove(int64 id) {
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
  if (_entries.empty()) {
    // Every file was refused.
    crl::on_main(this, [=] { close(); });
    return;
  }
  auto ready = true, media = false, images = true;
  for (const auto &entry : _entries) {
    if (!entry->file) {
      ready = false;
      images = false;
      continue;
    }
    using Type = Ui::PreparedFile::Type;
    media = media || entry->file->type == Type::Photo || entry->file->type == Type::Video;
    images = images && entry->file->type == Type::Photo;
  }
  const auto count = int(_entries.size());
  const auto asDocuments = adw_switch_row_get_active(ADW_SWITCH_ROW(_asDocuments));
  adw_dialog_set_title(ADW_DIALOG(_dialog), U8((images && !asDocuments) ? tr::lng_send_images_selected(tr::now, lt_count, count)
                                                                        : tr::lng_send_files_selected(tr::now, lt_count, count)).c_str());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(_asDocuments),
      U8(count > 1 ? tr::lng_send_as_documents(tr::now) : tr::lng_send_as_documents_one(tr::now)).c_str());
  gtk_widget_set_visible(_asDocuments, media);
  gtk_widget_set_visible(_group, count > 1);
  gtk_widget_set_sensitive(_sendButton, ready);
}

void AttachDialog::send() {
  if (_sent || _entries.empty()) return;
  for (const auto &entry : _entries) {
    if (!entry->file) return;
  }
  auto list = Ui::PreparedList();
  for (auto &entry : _entries) {
    list.files.push_back(std::move(*entry->file));
  }
  auto way = Ui::SendFilesWay();
  way.setSendImagesAsPhotos(!adw_switch_row_get_active(ADW_SWITCH_ROW(_asDocuments)));
  way.setGroupFiles(adw_switch_row_get_active(ADW_SWITCH_ROW(_group)));
  auto *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(_caption));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  auto *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  const auto caption = Q(text).trimmed();
  g_free(text);
  if (_args.send && _args.send(list, way, caption)) {
    _sent = true;
    close();
    return;
  }
  // Not sent (an error was shown): the files stay here.
  for (auto i = 0; i != int(_entries.size()) && i != int(list.files.size()); ++i) {
    *_entries[i]->file = std::move(list.files[i]);
  }
}

}  // namespace Contrail
