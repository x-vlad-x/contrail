#include "message.hpp"

#include "chat_view.hpp"
#include "layout.hpp"
#include "scroll_helpers.hpp"
#include "text.hpp"
#include "userpics.hpp"

#include "core/file_utilities.h"
#include "data/components/sponsored_messages.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_poll.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/data_web_page.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/view/history_view_item_preview.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/player/media_player_instance.h"
#include "ui/grouped_layout.h"
#include "ui/image/image.h"
#include "ui/text/format_values.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>

namespace Contrail {
namespace {

constexpr auto kMaxPhotoWidth = 280;
constexpr auto kMaxPhotoHeight = 320;
constexpr auto kMinPhotoSize = 100;
constexpr auto kStickerSize = 160;
constexpr auto kAlbumMinWidth = 100;  // The narrowest cell, before the album is scaled to the screen.
constexpr auto kAlbumSpacing = 2;

QSize FitPhoto(int w, int h, int maxWidth) {
  if (w <= 0 || h <= 0) {
    return {maxWidth, maxWidth * 3 / 4};
  }
  auto width = std::min(w, maxWidth);
  auto height = h * width / w;
  if (height > kMaxPhotoHeight) {
    height = kMaxPhotoHeight;
    width = std::max(w * height / h, kMinPhotoSize);
  }
  return {std::max(width, kMinPhotoSize), std::max(height, 48)};
}

void SetPictureImage(GtkWidget *picture, Image *image) {
  if (!image || image->isNull()) {
    return;
  }
  // A texture as large as the picture on the screen: drawing a downscaled full-size photo costs
  // time on every frame while scrolling, and the memory of all the loaded photos.
  auto original = image->original();
  const auto scale = std::max(gtk_widget_get_scale_factor(picture), 1);
  const auto target = QSize(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(picture), "contrail-media-width")),
                            GPOINTER_TO_INT(g_object_get_data(G_OBJECT(picture), "contrail-media-height")))
                      * scale;
  if (!target.isEmpty() && original.width() > target.width() && original.height() > target.height()) {
    original = original.scaled(target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
  }
  if (auto *texture = ToTexture(original)) {
    gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(texture));
    g_object_unref(texture);
  }
}

const char *DocumentIcon(not_null<DocumentData *> document) {
  if (document->isAudioFile() || document->isSong()) {
    return "audio-x-generic-symbolic";
  } else if (document->isVideoFile()) {
    return "video-x-generic-symbolic";
  } else if (document->isImage()) {
    return "image-x-generic-symbolic";
  }
  return "text-x-generic-symbolic";
}

QString PeerColorClass(not_null<PeerData *> peer) {
  return u"peer-color-"_q + QString::number(peer->colorIndex() % 7);
}

// A photo or a video: these make the grid of an album. Files (pictures sent as files too) and music make a
// column, as in Telegram's apps.
bool IsVisualMedia(not_null<HistoryItem *> item) {
  const auto media = item->media();
  if (!media || media->webpage()) return false;
  if (media->photo()) return true;
  const auto document = media->document();
  return document && !document->sticker() && (document->isVideoFile() || document->isAnimation());
}

QSize VisualSize(not_null<HistoryItem *> item) {
  const auto media = item->media();
  auto size = QSize();
  if (const auto photo = media ? media->photo() : nullptr) {
    size = QSize(photo->width(), photo->height());
  } else if (const auto document = media ? media->document() : nullptr) {
    size = document->dimensions;
  }
  return (size.width() > 0 && size.height() > 0) ? size : QSize(kMaxPhotoWidth, kMaxPhotoWidth);
}

// An upload going on: the part already sent.
std::optional<double> UploadProgress(not_null<HistoryItem *> item) {
  const auto media = item->media();
  const auto state = !media ? nullptr
                     : media->photo() ? media->photo()->uploadingData.get()
                     : media->document() ? media->document()->uploadingData.get()
                     : nullptr;
  if (!state || state->size <= 0) return std::nullopt;
  return std::clamp(double(state->offset) / state->size, 0., 1.);
}

QString Percent(double value) {
  return QString::number(int(value * 100)) + '%';
}

}  // namespace

MessageWidget::MessageWidget(Items items, not_null<ChatView *> chat) : _items(std::move(items)), _chat(chat) {
  Expects(!_items.empty());
  _row = gtk_list_box_row_new();
  g_object_ref_sink(_row);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(_row), FALSE);
  gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(_row), FALSE);
  gtk_widget_set_focusable(_row, FALSE);
  g_object_set_data(G_OBJECT(_row), "contrail-message", this);
  build();
}

MessageWidget::~MessageWidget() {
  _signals.clear();
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
  Expects(!items.empty());
  if (items == _items) return;
  _items = std::move(items);
  refresh();
}

GtkWidget *MessageWidget::widgetFor(const HistoryItem *item) const {
  if (_items.size() > 1) {
    for (const auto &cell : _cells) {
      if (cell->item == item && cell->widget) return cell->widget;
    }
  }
  return _row;
}

void MessageWidget::refresh() {
  _signals.clear();
  if (_highlightTimeout) {
    g_source_remove(base::take(_highlightTimeout));
  }
  _highlighted = nullptr;
  _cells.clear();
  _name = _avatar = _footer = _footerTime = _footerStatus = nullptr;
  build();
}

MessageWidget::Cell &MessageWidget::addCell(not_null<HistoryItem *> item) {
  _cells.push_back(std::make_unique<Cell>(Cell{.item = item}));
  return *_cells.back();
}

bool MessageWidget::isAlbumGrid() const {
  return _items.size() > 1 && std::all_of(_items.begin(), _items.end(), IsVisualMedia);
}

// The message of the bubble as a whole (the menu outside the cells, swiping to reply): the one with
// the caption, as the official apps do, else the first.
not_null<HistoryItem *> MessageWidget::mainItem() const {
  for (const auto &item : _items) {
    if (!item->originalText().text.isEmpty()) return item;
  }
  return _items.front();
}

void MessageWidget::build() {
  const auto first = _items.front();
  if (first->isService()) {
    auto *label = MakeTextLabel(first->notificationText());
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

  const auto history = first->history();
  const auto peer = history->peer;
  const auto out = first->out() && !peer->isBroadcast();
  const auto showSender = !out && !peer->isUser() && !peer->isBroadcast();

  auto *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(line, out ? GTK_ALIGN_END : GTK_ALIGN_START);
  gtk_widget_set_margin_start(line, 8);
  gtk_widget_set_margin_end(line, 8);
  gtk_widget_set_margin_top(line, 2);
  gtk_widget_set_margin_bottom(line, 2);

  const auto from = first->displayFrom() ? first->displayFrom() : first->from().get();
  if (showSender) {
    _avatar = MakeAvatar(from, 32);
    gtk_widget_set_valign(_avatar, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(line), _avatar);
  }

  auto *bubble = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(bubble, "bubble");
  gtk_widget_add_css_class(bubble, out ? "outgoing" : "incoming");
  // Its width mustn't depend on the row's height: see MakeHeightForWidthBin().
  gtk_box_append(GTK_BOX(line), MakeHeightForWidthBin(bubble));

  if (showSender) {
    _name = gtk_label_new(U8(from->name()).c_str());
    gtk_label_set_xalign(GTK_LABEL(_name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(_name), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(_name, "sender-name");
    gtk_widget_add_css_class(_name, U8(PeerColorClass(from)).c_str());
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

  auto hasMedia = false;
  if (isAlbumGrid()) {
    gtk_box_append(GTK_BOX(bubble), buildAlbumGrid());
    // Every caption of the album, in its order.
    for (const auto &item : _items) {
      if (const auto &text = item->originalText(); !text.text.isEmpty()) {
        gtk_box_append(GTK_BOX(bubble), MakeTextLabel(text));
      }
    }
    hasMedia = true;
  } else if (_items.size() > 1) {
    // Files and music sent together: one under the other, each with its caption.
    for (const auto &item : _items) {
      auto &cell = addCell(item);
      auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
      const auto document = item->media() ? item->media()->document() : nullptr;
      if (auto *media = (document && document->isImage()) ? buildDocument(cell, document) : buildMedia(cell, kMaxPhotoWidth)) {
        gtk_box_append(GTK_BOX(box), media);
      }
      if (const auto &text = item->originalText(); !text.text.isEmpty()) {
        gtk_box_append(GTK_BOX(box), MakeTextLabel(text));
      }
      cell.widget = box;
      setupGestures(box, item, false);
      gtk_box_append(GTK_BOX(bubble), box);
    }
    hasMedia = true;
  } else {
    auto &cell = addCell(first);
    auto *media = buildMedia(cell, kMaxPhotoWidth);
    if (media) {
      gtk_box_append(GTK_BOX(bubble), media);
      hasMedia = true;
    }
    const auto &text = first->originalText();
    if (!text.text.isEmpty()) {
      gtk_box_append(GTK_BOX(bubble), MakeTextLabel(text));
    } else if (!media && !first->isSponsored()) {
      // Something this interface can't show: a short description from the core.
      auto preview = first->toPreview({.hideSender = true, .generateImages = false});
      auto *label = MakeTextLabel(preview.text);
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
  (void)hasMedia;
  if (!first->isSponsored()) {
    _footer = buildFooter();
    gtk_box_append(GTK_BOX(bubble), _footer);
  }

  setupGestures(bubble, mainItem(), true);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(_row), line);
  _content = line;
}

GtkWidget *MessageWidget::buildForwarded(not_null<HistoryItem *> item) {
  const auto forwarded = item->Get<HistoryMessageForwarded>();
  if (!forwarded) {
    return nullptr;
  }
  auto name = forwarded->originalSender ? forwarded->originalSender->name()
              : forwarded->originalHiddenSenderInfo ? forwarded->originalHiddenSenderInfo->name
                                                   : QString();
  auto *label = gtk_label_new(nullptr);
  gtk_label_set_markup(GTK_LABEL(label),
                       (Escape(tr::lng_forwarded(tr::now, lt_user, QString())) + "<b>" + Escape(name) + "</b>").c_str());
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(label, "forwarded");
  if (const auto sender = forwarded->originalSender) {
    auto *click = gtk_gesture_click_new();
    _signals.connect<void(GtkGestureClick *, int, double, double)>(click, "released", [=](GtkGestureClick *, int, double, double) {
      if (TapAllowed()) _chat->openPeer(sender);
    });
    gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(click));
  }
  return label;
}

GtkWidget *MessageWidget::buildReply(not_null<HistoryItem *> item) {
  const auto reply = item->Get<HistoryMessageReply>();
  if (!reply || (!reply->messageId() && reply->fields().quote.text.isEmpty())) {
    return nullptr;
  }
  // In a group with topics every message "replies" to the start of its topic: that is no reply
  // (Element::countIsTopicRootReply), unless it quotes it.
  if (item->history()->isForum() && reply->messageId() == item->topicRootId()
      && reply->fields().quote.text.isEmpty()) {
    return nullptr;
  }
  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(box, "reply-quote");
  QString name;
  TextWithEntities text;
  if (const auto replied = reply->resolvedMessage.get()) {
    const auto from = replied->displayFrom() ? replied->displayFrom() : replied->from().get();
    name = from->name();
    gtk_widget_add_css_class(box, U8(PeerColorClass(from)).c_str());
    text = reply->fields().quote.text.isEmpty()
               ? replied->toPreview({.hideSender = true, .generateImages = false}).text
               : reply->fields().quote;
  } else {
    name = reply->fields().externalSenderName;
    text = reply->fields().quote.text.isEmpty() ? TextWithEntities{tr::lng_deleted_message(tr::now)}
                                                : reply->fields().quote;
  }
  for (auto &ch : text.text) {
    if (ch == '\n') {
      ch = ' ';
    }
  }
  auto *nameLabel = gtk_label_new(U8(name).c_str());
  gtk_widget_add_css_class(nameLabel, "reply-name");
  gtk_label_set_xalign(GTK_LABEL(nameLabel), 0);
  gtk_label_set_ellipsize(GTK_LABEL(nameLabel), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(box), nameLabel);
  auto *textLabel = gtk_label_new(nullptr);
  gtk_label_set_markup(GTK_LABEL(textLabel), ToMarkup(text, false, false).c_str());
  gtk_label_set_xalign(GTK_LABEL(textLabel), 0);
  gtk_label_set_ellipsize(GTK_LABEL(textLabel), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode(GTK_LABEL(textLabel), TRUE);
  gtk_box_append(GTK_BOX(box), textLabel);
  const auto replyId = reply->messageId();
  if (replyId) {
    auto *click = gtk_gesture_click_new();
    _signals.connect<void(GtkGestureClick *, int, double, double)>(click, "released", [=](GtkGestureClick *, int, double, double) {
      if (TapAllowed()) _chat->showAt(replyId);
    });
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));
  }
  return box;
}

GtkWidget *MessageWidget::buildMedia(Cell &cell, int maxWidth) {
  const auto media = cell.item->media();
  if (!media) {
    return nullptr;
  }
  if (media->webpage()) {
    return nullptr;  // Shown after the text.
  }
  if (const auto photo = media->photo()) {
    return buildPhoto(cell, photo, maxWidth);
  }
  if (const auto document = media->document()) {
    if (document->sticker()) {
      return buildSticker(cell, document);
    } else if (document->isVoiceMessage() || document->isAudioFile() || document->isSong()) {
      return buildVoice(cell, document);
    } else if (document->isVideoFile() || document->isAnimation() || document->isVideoMessage()) {
      return buildVideoThumb(cell, document, document->isVideoMessage() ? 200 : maxWidth);
    } else if (document->isImage() && document->hasThumbnail()) {
      return buildVideoThumb(cell, document, maxWidth);
    }
    return buildDocument(cell, document);
  }
  if (const auto poll = media->poll()) {
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    auto *question = MakeTextLabel(poll->question);
    gtk_widget_add_css_class(question, "heading");
    gtk_box_append(GTK_BOX(box), question);
    auto total = 0;
    for (const auto &answer : poll->answers) {
      total += answer.votes;
    }
    for (const auto &answer : poll->answers) {
      auto *line = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
      auto *text = MakeTextLabel(answer.text);
      gtk_box_append(GTK_BOX(line), text);
      if (poll->voted() || poll->closed()) {
        auto *bar = gtk_progress_bar_new();
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), total ? double(answer.votes) / total : 0.);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(bar),
                                  (std::to_string(total ? answer.votes * 100 / total : 0) + "%").c_str());
        gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(bar), TRUE);
        gtk_box_append(GTK_BOX(line), bar);
      }
      gtk_box_append(GTK_BOX(box), line);
    }
    auto *votes = gtk_label_new(U8(tr::lng_polls_votes_count(tr::now, lt_count, total)).c_str());
    gtk_widget_add_css_class(votes, "dim-label");
    gtk_widget_add_css_class(votes, "caption");
    gtk_label_set_xalign(GTK_LABEL(votes), 0);
    gtk_box_append(GTK_BOX(box), votes);
    return box;
  }
  if (const auto contact = media->sharedContact()) {
    auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    auto *avatar = adw_avatar_new(40, U8(contact->firstName + ' ' + contact->lastName).c_str(), TRUE);
    gtk_box_append(GTK_BOX(box), avatar);
    auto *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    auto *name = gtk_label_new(U8(contact->firstName + ' ' + contact->lastName).c_str());
    gtk_widget_add_css_class(name, "heading");
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(column), name);
    auto *phone = gtk_label_new(U8(Ui::FormatPhone(contact->phoneNumber)).c_str());
    gtk_label_set_selectable(GTK_LABEL(phone), FALSE);
    gtk_label_set_xalign(GTK_LABEL(phone), 0);
    gtk_label_set_ellipsize(GTK_LABEL(phone), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(column), phone);
    gtk_box_append(GTK_BOX(box), column);
    return box;
  }
  return nullptr;
}

// Photos and videos sent together: Telegram's own album layout (Ui::LayoutMediaGroup), drawn with GTK.
GtkWidget *MessageWidget::buildAlbumGrid() {
  auto sizes = std::vector<QSize>();
  sizes.reserve(_items.size());
  for (const auto &item : _items) {
    sizes.push_back(VisualSize(item));
  }
  const auto layout = Ui::LayoutMediaGroup(sizes, kMaxPhotoWidth, kAlbumMinWidth, kAlbumSpacing);
  auto rects = std::vector<QRect>();
  auto children = std::vector<GtkWidget *>();
  for (auto i = 0; i != int(_items.size()) && i != int(layout.size()); ++i) {
    const auto &rect = layout[i].geometry;
    auto &cell = addCell(_items[i]);
    children.push_back(buildGridCell(cell, rect.width(), rect.height()));
    rects.push_back(rect);
  }
  const auto size = AlbumSize(rects);
  return MakeAlbumGrid(children, std::move(rects), size);
}

GtkWidget *MessageWidget::buildGridCell(Cell &cell, int width, int height) {
  const auto item = cell.item;
  const auto media = item->media();
  auto *overlay = gtk_overlay_new();
  gtk_widget_add_css_class(overlay, "album-cell");
  gtk_widget_set_overflow(overlay, GTK_OVERFLOW_HIDDEN);
  cell.picture = MakeMediaPicture(width, height);
  gtk_overlay_set_child(GTK_OVERLAY(overlay), cell.picture);
  if (const auto photo = media->photo()) {
    cell.kind = PictureKind::Photo;
    cell.photo = photo;
    cell.photoMedia = photo->createMediaView();
    cell.photoMedia->wanted(Data::PhotoSize::Large, item->fullId());
    if (!refreshPicture(cell)) {
      SetPictureImage(cell.picture, cell.photoMedia->thumbnailInline());
    }
    auto *click = gtk_gesture_click_new();
    _signals.connect<void(GtkGestureClick *, int, double, double)>(click, "released", [=](GtkGestureClick *, int, double, double) {
      if (TapAllowed()) _chat->openPhoto(photo, item);
    });
    gtk_widget_add_controller(overlay, GTK_EVENT_CONTROLLER(click));
  } else if (const auto document = media->document()) {
    cell.kind = PictureKind::Thumbnail;
    cell.document = document;
    cell.documentMedia = document->createMediaView();
    cell.documentMedia->thumbnailWanted(item->fullId());
    if (!refreshPicture(cell)) {
      SetPictureImage(cell.picture, cell.documentMedia->thumbnailInline());
    }
    auto *play = gtk_image_new_from_icon_name(document->isImage() ? "image-x-generic-symbolic" : "media-playback-start-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(play), 24);
    gtk_widget_add_css_class(play, "osd");
    gtk_widget_add_css_class(play, "media-play");
    gtk_widget_set_halign(play, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(play, GTK_ALIGN_CENTER);
    gtk_widget_set_can_target(play, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), play);
    auto *click = gtk_gesture_click_new();
    _signals.connect<void(GtkGestureClick *, int, double, double)>(click, "released", [=](GtkGestureClick *, int, double, double) {
      if (!TapAllowed()) return;
      _chat->openDocument(document, item);
      refreshMedia();
    });
    gtk_widget_add_controller(overlay, GTK_EVENT_CONTROLLER(click));
  }
  cell.status = makeStatus();
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), cell.status);
  cell.widget = overlay;
  setupGestures(overlay, item, false);
  updateStatus(cell);
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

GtkWidget *MessageWidget::buildPhoto(Cell &cell, not_null<PhotoData *> photo, int maxWidth) {
  const auto size = FitPhoto(photo->width(), photo->height(), maxWidth);
  const auto item = cell.item;
  auto *overlay = gtk_overlay_new();
  cell.picture = MakeMediaPicture(size.width(), size.height());
  gtk_widget_add_css_class(cell.picture, "message-photo");
  gtk_overlay_set_child(GTK_OVERLAY(overlay), MakeMediaFrame(cell.picture, size.width(), size.height()));
  cell.kind = PictureKind::Photo;
  cell.photo = photo;
  cell.photoMedia = photo->createMediaView();
  cell.photoMedia->wanted(Data::PhotoSize::Large, item->fullId());
  if (!refreshPicture(cell)) {
    SetPictureImage(cell.picture, cell.photoMedia->thumbnailInline());
  }
  auto *click = gtk_gesture_click_new();
  _signals.connect<void(GtkGestureClick *, int, double, double)>(click, "released", [=](GtkGestureClick *, int, double, double) {
    if (TapAllowed()) _chat->openPhoto(photo, item);
  });
  gtk_widget_add_controller(cell.picture, GTK_EVENT_CONTROLLER(click));
  cell.status = makeStatus();
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), cell.status);
  updateStatus(cell);
  return overlay;
}

GtkWidget *MessageWidget::buildSticker(Cell &cell, not_null<DocumentData *> document) {
  auto width = kStickerSize, height = kStickerSize;
  if (document->dimensions.width() > 0 && document->dimensions.height() > 0) {
    const auto &d = document->dimensions;
    if (d.width() > d.height()) {
      height = kStickerSize * d.height() / d.width();
    } else {
      width = kStickerSize * d.width() / d.height();
    }
  }
  cell.picture = MakeMediaPicture(width, height);
  gtk_picture_set_content_fit(GTK_PICTURE(cell.picture), GTK_CONTENT_FIT_CONTAIN);
  cell.kind = PictureKind::Sticker;
  cell.documentMedia = document->createMediaView();
  cell.documentMedia->thumbnailWanted(cell.item->fullId());
  if ((document->sticker() && document->sticker()->isStatic())) {
    cell.documentMedia->automaticLoad(cell.item->fullId(), cell.item);
  }
  refreshPicture(cell);
  auto *frame = MakeMediaFrame(cell.picture, width, height);
  gtk_widget_set_halign(frame, GTK_ALIGN_START);
  return frame;
}

GtkWidget *MessageWidget::buildVideoThumb(Cell &cell, not_null<DocumentData *> document, int maxWidth) {
  const auto size = FitPhoto(document->dimensions.width(), document->dimensions.height(), maxWidth);
  const auto item = cell.item;
  auto *overlay = gtk_overlay_new();
  cell.picture = MakeMediaPicture(size.width(), size.height());
  gtk_widget_add_css_class(cell.picture, "message-photo");
  if (document->isVideoMessage()) {
    gtk_widget_add_css_class(cell.picture, "round-video");
  }
  gtk_overlay_set_child(GTK_OVERLAY(overlay), MakeMediaFrame(cell.picture, size.width(), size.height()));
  cell.kind = PictureKind::Thumbnail;
  cell.documentMedia = document->createMediaView();
  cell.documentMedia->thumbnailWanted(item->fullId());
  if (!refreshPicture(cell)) {
    SetPictureImage(cell.picture, cell.documentMedia->thumbnailInline());
  }
  cell.document = document;
  auto *play = gtk_image_new_from_icon_name(document->isImage() ? "image-x-generic-symbolic" : "media-playback-start-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(play), 24);
  gtk_widget_add_css_class(play, "osd");
  gtk_widget_add_css_class(play, "media-play");
  gtk_widget_set_halign(play, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(play, GTK_ALIGN_CENTER);
  gtk_widget_set_can_target(play, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), play);
  cell.status = makeStatus();
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), cell.status);
  auto *click = gtk_gesture_click_new();
  _signals.connect<void(GtkGestureClick *, int, double, double)>(click, "released", [=](GtkGestureClick *, int, double, double) {
    if (!TapAllowed()) return;
    _chat->openDocument(document, item);
    refreshMedia();
  });
  gtk_widget_add_controller(overlay, GTK_EVENT_CONTROLLER(click));
  if (document->isVideoMessage()) {
    gtk_widget_set_halign(overlay, GTK_ALIGN_START);  // Stays round in a wider bubble.
  }
  updateStatus(cell);
  return overlay;
}

GtkWidget *MessageWidget::buildDocument(Cell &cell, not_null<DocumentData *> document) {
  const auto item = cell.item;
  cell.document = document;
  auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(box, "document");
  cell.documentButton = gtk_button_new_from_icon_name(DocumentIcon(document));
  gtk_widget_add_css_class(cell.documentButton, "circular");
  gtk_widget_add_css_class(cell.documentButton, "document-button");
  gtk_widget_set_valign(cell.documentButton, GTK_ALIGN_CENTER);
  _signals.clicked(cell.documentButton, [=] {
    if (!TapAllowed()) return;
    _chat->openDocument(document, item);
    refreshMedia();
  });
  gtk_box_append(GTK_BOX(box), cell.documentButton);
  auto *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_valign(column, GTK_ALIGN_CENTER);
  auto *name = gtk_label_new(U8(document->filename().isEmpty() ? tr::lng_in_dlg_file(tr::now) : document->filename()).c_str());
  gtk_label_set_xalign(GTK_LABEL(name), 0);
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars(GTK_LABEL(name), 28);
  gtk_widget_add_css_class(name, "heading");
  gtk_box_append(GTK_BOX(column), name);
  cell.documentStatus = gtk_label_new(nullptr);
  gtk_label_set_xalign(GTK_LABEL(cell.documentStatus), 0);
  gtk_widget_add_css_class(cell.documentStatus, "dim-label");
  gtk_widget_add_css_class(cell.documentStatus, "caption");
  gtk_box_append(GTK_BOX(column), cell.documentStatus);
  gtk_box_append(GTK_BOX(box), column);
  updateDocumentState(cell);
  return box;
}

GtkWidget *MessageWidget::buildVoice(Cell &cell, not_null<DocumentData *> document) {
  const auto item = cell.item;
  cell.document = document;
  auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(box, "voice");
  cell.documentButton = gtk_button_new_from_icon_name("media-playback-start-symbolic");
  gtk_widget_add_css_class(cell.documentButton, "circular");
  gtk_widget_add_css_class(cell.documentButton, "suggested-action");
  gtk_widget_set_valign(cell.documentButton, GTK_ALIGN_CENTER);
  _signals.clicked(cell.documentButton, [=] {
    if (!TapAllowed()) return;
    Media::Player::instance()->playPause(AudioMsgId(document, item->fullId()));
  });
  gtk_box_append(GTK_BOX(box), cell.documentButton);
  auto *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_valign(column, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(column, TRUE);
  if (const auto song = document->song(); song && !document->isVoiceMessage()) {
    auto title = song->title.isEmpty() ? document->filename() : song->title;
    if (!song->performer.isEmpty()) {
      title = song->performer + u" – "_q + title;
    }
    auto *name = gtk_label_new(U8(title).c_str());
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(name), 26);
    gtk_widget_add_css_class(name, "heading");
    gtk_box_append(GTK_BOX(column), name);
  }
  cell.voiceProgress = gtk_progress_bar_new();
  gtk_widget_add_css_class(cell.voiceProgress, "voice-progress");
  gtk_box_append(GTK_BOX(column), cell.voiceProgress);
  cell.documentStatus = gtk_label_new(U8(Ui::FormatDurationText(document->duration() / 1000)).c_str());
  gtk_label_set_xalign(GTK_LABEL(cell.documentStatus), 0);
  gtk_widget_add_css_class(cell.documentStatus, "dim-label");
  gtk_widget_add_css_class(cell.documentStatus, "caption");
  gtk_box_append(GTK_BOX(column), cell.documentStatus);
  gtk_box_append(GTK_BOX(box), column);
  updatePlayback(Media::Player::instance()->getState(document->isVoiceMessage() ? AudioMsgId::Type::Voice
                                                                                : AudioMsgId::Type::Song));
  return box;
}

GtkWidget *MessageWidget::buildWebPage(not_null<HistoryItem *> item) {
  const auto media = item->media();
  const auto page = media ? media->webpage() : nullptr;
  if (!page || page->pendingTill || page->failed || (page->title.isEmpty() && page->description.text.isEmpty() && !page->photo)) {
    return nullptr;
  }
  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_add_css_class(box, "webpage");
  if (!page->siteName.isEmpty()) {
    auto *site = gtk_label_new(U8(page->siteName).c_str());
    gtk_widget_add_css_class(site, "webpage-site");
    gtk_label_set_xalign(GTK_LABEL(site), 0);
    gtk_label_set_ellipsize(GTK_LABEL(site), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), site);
  }
  if (!page->title.isEmpty()) {
    auto *title = gtk_label_new(U8(page->title).c_str());
    gtk_widget_add_css_class(title, "heading");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_label_set_wrap(GTK_LABEL(title), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(title), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(title), 44);
    gtk_box_append(GTK_BOX(box), title);
  }
  if (!page->description.text.isEmpty()) {
    auto description = page->description;
    if (description.text.size() > 300) {
      description = Ui::Text::Mid(description, 0, 300);
      description.text += QChar(0x2026);
    }
    gtk_box_append(GTK_BOX(box), MakeTextLabel(description));
  }
  if (page->photo && std::none_of(_cells.begin(), _cells.end(), [](const auto &cell) { return cell->picture != nullptr; })) {
    gtk_box_append(GTK_BOX(box), buildPhoto(addCell(item), page->photo, kMaxPhotoWidth - 20));
  }
  auto url = page->url;
  auto open = Fn<void()>();
  if (item->isSponsored()) {
    // A sponsored message (see ChatView::showSponsored): the core reports the click, the button names the action.
    gtk_widget_add_css_class(box, "sponsored");
    auto &sponsored = item->history()->session().sponsoredMessages();
    const auto details = sponsored.lookupDetails(item->fullId());
    if (!details.link.isEmpty()) url = details.link;
    const auto session = &item->history()->session();
    const auto id = item->fullId();
    open = [=] {
      session->sponsoredMessages().clicked(id, false, false);
      HandleLink(url);
    };
    if (!url.isEmpty()) {
      auto *button = gtk_button_new_with_label(
          U8(details.buttonText.isEmpty() ? tr::lng_open_link(tr::now) : details.buttonText).c_str());
      gtk_widget_add_css_class(button, "sponsored-button");
      gtk_widget_set_margin_top(button, 4);
      _signals.connect<void(GtkButton *)>(button, "clicked", [=](GtkButton *) { open(); });
      gtk_box_append(GTK_BOX(box), button);
    }
    for (const auto &info : details.info) {
      auto *label = MakeTextLabel(info);
      gtk_widget_add_css_class(label, "dim-label");
      gtk_widget_add_css_class(label, "caption");
      gtk_box_append(GTK_BOX(box), label);
    }
  } else {
    open = [=] { HandleLink(url); };
  }
  auto *click = gtk_gesture_click_new();
  _signals.connect<void(GtkGestureClick *, int, double, double)>(click, "released", [=](GtkGestureClick *g, int, double, double) {
    // Taps on the photo open the viewer, the rest opens the page.
    if (TapAllowed() && !url.isEmpty()) open();
  });
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));
  return box;
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
  if (!_footer) {
    return;
  }
  const auto last = _items.back();
  QString text;
  if (std::any_of(_items.begin(), _items.end(), [](not_null<HistoryItem *> item) {
        return item->Has<HistoryMessageEdited>() && !item->hideEditedBadge();
      })) {
    text = tr::lng_edited(tr::now) + ' ';
  }
  if (const auto views = last->viewsCount(); views > 0 && last->history()->peer->isBroadcast()) {
    text = QString::fromUtf8("\xF0\x9F\x91\x81 ") + Lang::FormatCountToShort(views).string + u"  "_q + text;
  }
  if (const auto signature = last->Get<HistoryMessageSigned>(); signature && !signature->author.isEmpty()) {
    text = signature->author + u", "_q + text;
  }
  text += FormatTime(last->date());
  gtk_label_set_text(GTK_LABEL(_footerTime), U8(text).c_str());

  const char *icon = nullptr;
  const auto history = last->history();
  const auto any = [&](auto check) { return std::any_of(_items.begin(), _items.end(), check); };
  if (last->out() && !history->peer->isBroadcast()) {
    if (any([](not_null<HistoryItem *> item) { return item->hasFailed(); })) {
      icon = "dialog-error-symbolic";
    } else if (any([](not_null<HistoryItem *> item) { return item->isSending(); })) {
      icon = "contrail-clock-symbolic";
    } else if (!history->peer->isSelf() && any([&](not_null<HistoryItem *> item) { return item->unread(history); })) {
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

bool MessageWidget::refreshMedia() {
  auto done = true;
  for (const auto &cell : _cells) {
    updateDocumentState(*cell);
    // A download or an upload going on is followed until it ends.
    if (updateStatus(*cell)) done = false;
    if (!refreshPicture(*cell)) done = false;
  }
  return done;
}

bool MessageWidget::refreshPicture(Cell &cell) {
  if (!cell.picture || cell.pictureFinal) {
    return true;
  }
  Image *image = nullptr;
  const auto document = cell.item->media() ? cell.item->media()->document() : nullptr;
  const auto staticSticker = document && document->sticker() && document->sticker()->isStatic();
  switch (cell.kind) {
    case PictureKind::Photo: image = cell.photoMedia ? cell.photoMedia->image(Data::PhotoSize::Large) : nullptr; break;
    case PictureKind::Sticker:
      if (cell.documentMedia) {
        if (staticSticker) {
          cell.documentMedia->checkStickerLarge();
          image = cell.documentMedia->getStickerLarge();
        }
        if (!image) {
          if (auto *thumb = cell.documentMedia->thumbnail()) {
            SetPictureImage(cell.picture, thumb);
          }
          // Animated stickers stay with their thumbnail.
          return !staticSticker;
        }
      }
      break;
    case PictureKind::Thumbnail: image = cell.documentMedia ? cell.documentMedia->thumbnail() : nullptr; break;
    case PictureKind::None: break;
  }
  if (!image) {
    return false;
  }
  SetPictureImage(cell.picture, image);
  cell.pictureFinal = true;
  // The texture holds what is shown: the decoded image (up to 2560 px) needn't stay in memory.
  cell.photoMedia = nullptr;
  cell.documentMedia = nullptr;
  return true;
}

void MessageWidget::updateDocumentState(Cell &cell) {
  const auto document = cell.document;
  if (!document || !cell.documentStatus || cell.voiceProgress) {
    return;
  }
  QString status;
  const char *icon = DocumentIcon(document);
  if (const auto upload = UploadProgress(cell.item)) {
    status = Percent(*upload);
    icon = "contrail-clock-symbolic";
  } else if (document->loading()) {
    status = Ui::FormatDownloadText(document->loadOffset(), document->size);
    icon = "process-stop-symbolic";
  } else if (!document->filepath(true).isEmpty()) {
    status = Ui::FormatSizeText(document->size);
  } else {
    status = Ui::FormatSizeText(document->size);
    icon = "folder-download-symbolic";
  }
  gtk_label_set_text(GTK_LABEL(cell.documentStatus), U8(status).c_str());
  if (cell.documentButton) {
    gtk_button_set_icon_name(GTK_BUTTON(cell.documentButton), icon);
  }
}

// Over a picture: how much of it is uploaded or downloaded; a video's length otherwise.
bool MessageWidget::updateStatus(Cell &cell) {
  const auto upload = UploadProgress(cell.item);
  const auto transfer = upload || (cell.document && cell.document->loading())
                        || (cell.photo && !cell.pictureFinal && cell.photo->displayLoading());
  if (!cell.status) return transfer;
  QString text;
  if (upload) {
    text = Percent(*upload);
  } else if (const auto document = cell.document) {
    if (document->loading()) {
      text = Ui::FormatDownloadText(document->loadOffset(), document->size);
    } else if (const auto duration = document->duration(); duration > 0) {
      text = Ui::FormatDurationText(duration / 1000);
    } else if (document->isAnimation()) {
      text = u"GIF"_q;
    }
  } else if (const auto photo = cell.photo; photo && !cell.pictureFinal && photo->displayLoading()) {
    text = Percent(photo->progress());
  }
  gtk_label_set_text(GTK_LABEL(cell.status), U8(text).c_str());
  gtk_widget_set_visible(cell.status, !text.isEmpty());
  return transfer;
}

void MessageWidget::updatePlayback(const Media::Player::TrackState &state) {
  for (const auto &cell : _cells) {
    if (!cell->voiceProgress || !cell->document) {
      continue;
    }
    const auto document = cell->document;
    const auto mine = (state.id.audio() == document && state.id.contextId() == cell->item->fullId());
    const auto playing = mine && Media::Player::ShowPauseIcon(state.state);
    gtk_button_set_icon_name(GTK_BUTTON(cell->documentButton), playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");
    if (mine && !Media::Player::IsStoppedOrStopping(state.state) && state.length > 0) {
      gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(cell->voiceProgress), double(state.position) / state.length);
      const auto frequency = state.frequency ? state.frequency : Media::Player::kDefaultFrequency;
      gtk_label_set_text(GTK_LABEL(cell->documentStatus),
                         U8(Ui::FormatDurationText(state.position / frequency) + u" / "_q
                            + Ui::FormatDurationText(state.length / frequency))
                             .c_str());
    } else {
      gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(cell->voiceProgress), 0.);
      gtk_label_set_text(GTK_LABEL(cell->documentStatus), U8(Ui::FormatDurationText(document->duration() / 1000)).c_str());
    }
  }
}

void MessageWidget::setFirstInGroup(bool first) {
  if (_name) {
    gtk_widget_set_visible(_name, first);
  }
  if (_avatar) {
    gtk_widget_set_opacity(_avatar, first ? 1. : 0.);
  }
  if (auto *line = _content; line && !_items.front()->isService()) {
    gtk_widget_set_margin_top(line, first ? 6 : 1);
  }
}

// A reply or a link to one message of an album lights up its cell only.
void MessageWidget::highlight(const HistoryItem *item) {
  if (_highlightTimeout) {
    g_source_remove(_highlightTimeout);
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

// The menu belongs to the message under the finger: a cell of an album has its own, which wins over
// the bubble's (the bubble's is for the album as a whole).
void MessageWidget::setupGestures(GtkWidget *widget, not_null<HistoryItem *> item, bool swipe) {
  auto *longPress = gtk_gesture_long_press_new();
  gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(longPress), TRUE);
  _signals.connect<void(GtkGestureLongPress *, double, double)>(longPress, "pressed", [=](GtkGestureLongPress *g, double x, double y) {
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    _chat->showMessageMenu(item, widget, x, y);
  });
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(longPress));
  auto *rightClick = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rightClick), GDK_BUTTON_SECONDARY);
  _signals.connect<void(GtkGestureClick *, int, double, double)>(rightClick, "pressed", [=](GtkGestureClick *g, int, double x, double y) {
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    _chat->showMessageMenu(item, widget, x, y);
  });
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(rightClick));
  if (!swipe) return;
  // Swipe left to reply, as in the phone apps.
  // An unrestricted drag wins the touch sequence over the parent scrolled window. Pan denies
  // vertical movement early, so touching message text still scrolls the conversation.
  auto *pan = gtk_gesture_pan_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(pan), TRUE);
  _signals.connect<void(GtkGestureDrag *, double, double)>(pan, "drag-end", [=](GtkGestureDrag *, double dx, double dy) {
    if (dx < -80 && std::abs(dy) < 40) {
      _chat->replyTo(item);
    }
  });
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(pan));
}

}  // namespace Contrail
