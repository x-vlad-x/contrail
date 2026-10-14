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

}  // namespace

MessageWidget::MessageWidget(not_null<HistoryItem *> item, not_null<ChatView *> chat) : _item(item), _chat(chat) {
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

void MessageWidget::refresh() {
  _signals.clear();
  _photoMedia = nullptr;
  _documentMedia = nullptr;
  _picture = nullptr;
  _pictureFinal = false;
  _pictureKind = PictureKind::None;
  _document = nullptr;
  _documentButton = _documentStatus = _voiceProgress = nullptr;
  _name = _avatar = _footer = _footerTime = _footerStatus = nullptr;
  build();
}

void MessageWidget::build() {
  if (_item->isService()) {
    auto *label = MakeTextLabel(_item->notificationText());
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

  const auto history = _item->history();
  const auto peer = history->peer;
  const auto out = _item->out() && !peer->isBroadcast();
  const auto showSender = !out && !peer->isUser() && !peer->isBroadcast();

  auto *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(line, out ? GTK_ALIGN_END : GTK_ALIGN_START);
  gtk_widget_set_margin_start(line, 8);
  gtk_widget_set_margin_end(line, 8);
  gtk_widget_set_margin_top(line, 2);
  gtk_widget_set_margin_bottom(line, 2);

  if (showSender) {
    const auto from = _item->displayFrom() ? _item->displayFrom() : _item->from().get();
    _avatar = MakeAvatar(from, 32);
    gtk_widget_set_valign(_avatar, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(line), _avatar);
  }

  auto *bubble = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(bubble, "bubble");
  gtk_widget_add_css_class(bubble, out ? "outgoing" : "incoming");
  gtk_box_append(GTK_BOX(line), bubble);

  if (showSender) {
    const auto from = _item->displayFrom() ? _item->displayFrom() : _item->from().get();
    _name = gtk_label_new(U8(from->name()).c_str());
    gtk_label_set_xalign(GTK_LABEL(_name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(_name), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(_name, "sender-name");
    gtk_widget_add_css_class(_name, U8(PeerColorClass(from)).c_str());
    gtk_box_append(GTK_BOX(bubble), _name);
  }
  if (auto *forwarded = buildForwarded()) {
    gtk_box_append(GTK_BOX(bubble), forwarded);
  }
  if (auto *reply = buildReply()) {
    gtk_box_append(GTK_BOX(bubble), reply);
  }
  auto *media = buildMedia();
  if (media) {
    gtk_box_append(GTK_BOX(bubble), media);
  }
  const auto &text = _item->originalText();
  if (!text.text.isEmpty()) {
    gtk_box_append(GTK_BOX(bubble), MakeTextLabel(text));
  } else if (!media && !_item->isSponsored()) {
    // Something this interface can't show: a short description from the core.
    auto preview = _item->toPreview({.hideSender = true, .generateImages = false});
    auto *label = MakeTextLabel(preview.text);
    gtk_widget_add_css_class(label, "dim-label");
    gtk_box_append(GTK_BOX(bubble), label);
  }
  if (_pictureKind == PictureKind::Sticker) {
    gtk_widget_add_css_class(bubble, "sticker");
  }
  if (auto *webpage = buildWebPage()) {
    gtk_box_append(GTK_BOX(bubble), webpage);
  }
  if (!_item->isSponsored()) {
    _footer = buildFooter();
    gtk_box_append(GTK_BOX(bubble), _footer);
  }

  setupGestures(bubble);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(_row), line);
  _content = line;
}

GtkWidget *MessageWidget::buildForwarded() {
  const auto forwarded = _item->Get<HistoryMessageForwarded>();
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

GtkWidget *MessageWidget::buildReply() {
  const auto reply = _item->Get<HistoryMessageReply>();
  if (!reply || (!reply->messageId() && reply->fields().quote.text.isEmpty())) {
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

GtkWidget *MessageWidget::buildMedia() {
  const auto media = _item->media();
  if (!media) {
    return nullptr;
  }
  if (media->webpage()) {
    return nullptr;  // Shown after the text.
  }
  if (const auto photo = media->photo()) {
    return buildPhoto(photo, kMaxPhotoWidth);
  }
  if (const auto document = media->document()) {
    if (document->sticker()) {
      return buildSticker(document);
    } else if (document->isVoiceMessage() || document->isAudioFile() || document->isSong()) {
      return buildVoice(document);
    } else if (document->isVideoFile() || document->isAnimation() || document->isVideoMessage()) {
      return buildVideoThumb(document, document->isVideoMessage() ? 200 : kMaxPhotoWidth);
    } else if (document->isImage() && document->hasThumbnail()) {
      return buildVideoThumb(document, kMaxPhotoWidth);
    }
    return buildDocument(document);
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

GtkWidget *MessageWidget::buildPhoto(not_null<PhotoData *> photo, int maxWidth) {
  const auto size = FitPhoto(photo->width(), photo->height(), maxWidth);
  _picture = MakeMediaPicture(size.width(), size.height());
  gtk_widget_add_css_class(_picture, "message-photo");
  _pictureKind = PictureKind::Photo;
  _photoMedia = photo->createMediaView();
  _photoMedia->wanted(Data::PhotoSize::Large, _item->fullId());
  if (!refreshMedia()) {
    SetPictureImage(_picture, _photoMedia->thumbnailInline());
  }
  auto *click = gtk_gesture_click_new();
  _signals.connect<void(GtkGestureClick *, int, double, double)>(click, "released", [=](GtkGestureClick *, int, double, double) {
    if (TapAllowed()) _chat->openPhoto(photo, _item);
  });
  gtk_widget_add_controller(_picture, GTK_EVENT_CONTROLLER(click));
  return MakeMediaFrame(_picture, size.width(), size.height());
}

GtkWidget *MessageWidget::buildSticker(not_null<DocumentData *> document) {
  auto width = kStickerSize, height = kStickerSize;
  if (document->dimensions.width() > 0 && document->dimensions.height() > 0) {
    const auto &d = document->dimensions;
    if (d.width() > d.height()) {
      height = kStickerSize * d.height() / d.width();
    } else {
      width = kStickerSize * d.width() / d.height();
    }
  }
  _picture = MakeMediaPicture(width, height);
  gtk_picture_set_content_fit(GTK_PICTURE(_picture), GTK_CONTENT_FIT_CONTAIN);
  _pictureKind = PictureKind::Sticker;
  _documentMedia = document->createMediaView();
  _documentMedia->thumbnailWanted(_item->fullId());
  if ((document->sticker() && document->sticker()->isStatic())) {
    _documentMedia->automaticLoad(_item->fullId(), _item);
  }
  refreshMedia();
  auto *frame = MakeMediaFrame(_picture, width, height);
  gtk_widget_set_halign(frame, GTK_ALIGN_START);
  return frame;
}

GtkWidget *MessageWidget::buildVideoThumb(not_null<DocumentData *> document, int maxWidth) {
  const auto size = FitPhoto(document->dimensions.width(), document->dimensions.height(), maxWidth);
  auto *overlay = gtk_overlay_new();
  _picture = MakeMediaPicture(size.width(), size.height());
  gtk_widget_add_css_class(_picture, "message-photo");
  if (document->isVideoMessage()) {
    gtk_widget_add_css_class(_picture, "round-video");
  }
  gtk_overlay_set_child(GTK_OVERLAY(overlay), MakeMediaFrame(_picture, size.width(), size.height()));
  _pictureKind = PictureKind::Thumbnail;
  _documentMedia = document->createMediaView();
  _documentMedia->thumbnailWanted(_item->fullId());
  if (!refreshMedia()) {
    SetPictureImage(_picture, _documentMedia->thumbnailInline());
  }
  _document = document;
  auto *play = gtk_image_new_from_icon_name(document->isImage() ? "image-x-generic-symbolic" : "media-playback-start-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(play), 24);
  gtk_widget_add_css_class(play, "osd");
  gtk_widget_add_css_class(play, "media-play");
  gtk_widget_set_halign(play, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(play, GTK_ALIGN_CENTER);
  gtk_widget_set_can_target(play, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), play);
  _documentStatus = gtk_label_new(nullptr);
  gtk_widget_add_css_class(_documentStatus, "osd");
  gtk_widget_add_css_class(_documentStatus, "media-status");
  gtk_widget_set_halign(_documentStatus, GTK_ALIGN_START);
  gtk_widget_set_valign(_documentStatus, GTK_ALIGN_START);
  gtk_widget_set_can_target(_documentStatus, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), _documentStatus);
  auto *click = gtk_gesture_click_new();
  _signals.connect<void(GtkGestureClick *, int, double, double)>(click, "released", [=](GtkGestureClick *, int, double, double) {
    if (!TapAllowed()) return;
    _chat->openDocument(document, _item);
    updateDocumentState();
  });
  gtk_widget_add_controller(overlay, GTK_EVENT_CONTROLLER(click));
  if (document->isVideoMessage()) {
    gtk_widget_set_halign(overlay, GTK_ALIGN_START);  // Stays round in a wider bubble.
  }
  updateDocumentState();
  return overlay;
}

GtkWidget *MessageWidget::buildDocument(not_null<DocumentData *> document) {
  _document = document;
  auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(box, "document");
  _documentButton = gtk_button_new_from_icon_name(DocumentIcon(document));
  gtk_widget_add_css_class(_documentButton, "circular");
  gtk_widget_add_css_class(_documentButton, "document-button");
  gtk_widget_set_valign(_documentButton, GTK_ALIGN_CENTER);
  _signals.clicked(_documentButton, [=] {
    if (!TapAllowed()) return;
    _chat->openDocument(document, _item);
    updateDocumentState();
  });
  gtk_box_append(GTK_BOX(box), _documentButton);
  auto *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_valign(column, GTK_ALIGN_CENTER);
  auto *name = gtk_label_new(U8(document->filename().isEmpty() ? tr::lng_in_dlg_file(tr::now) : document->filename()).c_str());
  gtk_label_set_xalign(GTK_LABEL(name), 0);
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars(GTK_LABEL(name), 28);
  gtk_widget_add_css_class(name, "heading");
  gtk_box_append(GTK_BOX(column), name);
  _documentStatus = gtk_label_new(nullptr);
  gtk_label_set_xalign(GTK_LABEL(_documentStatus), 0);
  gtk_widget_add_css_class(_documentStatus, "dim-label");
  gtk_widget_add_css_class(_documentStatus, "caption");
  gtk_box_append(GTK_BOX(column), _documentStatus);
  gtk_box_append(GTK_BOX(box), column);
  updateDocumentState();
  return box;
}

GtkWidget *MessageWidget::buildVoice(not_null<DocumentData *> document) {
  _document = document;
  auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(box, "voice");
  _documentButton = gtk_button_new_from_icon_name("media-playback-start-symbolic");
  gtk_widget_add_css_class(_documentButton, "circular");
  gtk_widget_add_css_class(_documentButton, "suggested-action");
  gtk_widget_set_valign(_documentButton, GTK_ALIGN_CENTER);
  _signals.clicked(_documentButton, [=] {
    if (!TapAllowed()) return;
    const auto type = document->isVoiceMessage() ? AudioMsgId::Type::Voice : AudioMsgId::Type::Song;
    (void)type;
    Media::Player::instance()->playPause(AudioMsgId(document, _item->fullId()));
  });
  gtk_box_append(GTK_BOX(box), _documentButton);
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
  _voiceProgress = gtk_progress_bar_new();
  gtk_widget_add_css_class(_voiceProgress, "voice-progress");
  gtk_box_append(GTK_BOX(column), _voiceProgress);
  _documentStatus = gtk_label_new(U8(Ui::FormatDurationText(document->duration() / 1000)).c_str());
  gtk_label_set_xalign(GTK_LABEL(_documentStatus), 0);
  gtk_widget_add_css_class(_documentStatus, "dim-label");
  gtk_widget_add_css_class(_documentStatus, "caption");
  gtk_box_append(GTK_BOX(column), _documentStatus);
  gtk_box_append(GTK_BOX(box), column);
  updatePlayback(Media::Player::instance()->getState(document->isVoiceMessage() ? AudioMsgId::Type::Voice
                                                                                : AudioMsgId::Type::Song));
  return box;
}

GtkWidget *MessageWidget::buildWebPage() {
  const auto media = _item->media();
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
  if (page->photo && !_picture) {
    gtk_box_append(GTK_BOX(box), buildPhoto(page->photo, kMaxPhotoWidth - 20));
  }
  auto url = page->url;
  auto open = Fn<void()>();
  if (_item->isSponsored()) {
    // A sponsored message (see ChatView::showSponsored): the core reports the click, the button names the action.
    gtk_widget_add_css_class(box, "sponsored");
    auto &sponsored = _item->history()->session().sponsoredMessages();
    const auto details = sponsored.lookupDetails(_item->fullId());
    if (!details.link.isEmpty()) url = details.link;
    const auto session = &_item->history()->session();
    const auto id = _item->fullId();
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

void MessageWidget::refreshFooter() {
  if (!_footer) {
    return;
  }
  QString text;
  if (_item->Has<HistoryMessageEdited>() && !_item->hideEditedBadge()) {
    text = tr::lng_edited(tr::now) + ' ';
  }
  if (const auto views = _item->viewsCount(); views > 0 && _item->history()->peer->isBroadcast()) {
    text = QString::fromUtf8("\xF0\x9F\x91\x81 ") + Lang::FormatCountToShort(views).string + u"  "_q + text;
  }
  if (const auto signature = _item->Get<HistoryMessageSigned>(); signature && !signature->author.isEmpty()) {
    text = signature->author + u", "_q + text;
  }
  text += FormatTime(_item->date());
  gtk_label_set_text(GTK_LABEL(_footerTime), U8(text).c_str());

  const char *icon = nullptr;
  const auto history = _item->history();
  if (_item->out() && !history->peer->isBroadcast()) {
    if (_item->hasFailed()) {
      icon = "dialog-error-symbolic";
    } else if (_item->isSending()) {
      icon = "contrail-clock-symbolic";
    } else if (!history->peer->isSelf() && _item->unread(history)) {
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
  updateDocumentState();
  if (!_picture || _pictureFinal) {
    return true;
  }
  Image *image = nullptr;
  switch (_pictureKind) {
    case PictureKind::Photo: image = _photoMedia ? _photoMedia->image(Data::PhotoSize::Large) : nullptr; break;
    case PictureKind::Sticker:
      if (_documentMedia) {
        if (_item->media() && _item->media()->document() && _item->media()->document()->sticker() && _item->media()->document()->sticker()->isStatic()) {
          _documentMedia->checkStickerLarge();
          image = _documentMedia->getStickerLarge();
        }
        if (!image) {
          if (auto *thumb = _documentMedia->thumbnail()) {
            SetPictureImage(_picture, thumb);
          }
          // Animated stickers stay with their thumbnail.
          return !(_item->media() && _item->media()->document() && _item->media()->document()->sticker() && _item->media()->document()->sticker()->isStatic());
        }
      }
      break;
    case PictureKind::Thumbnail: image = _documentMedia ? _documentMedia->thumbnail() : nullptr; break;
    case PictureKind::None: break;
  }
  if (!image) {
    return false;
  }
  SetPictureImage(_picture, image);
  _pictureFinal = true;
  return true;
}

void MessageWidget::updateDocumentState() {
  if (!_document || !_documentStatus || _voiceProgress) {
    return;
  }
  const auto document = _document;
  QString status;
  const char *icon = DocumentIcon(document);
  if (document->loading()) {
    status = Ui::FormatDownloadText(document->loadOffset(), document->size);
    icon = "process-stop-symbolic";
  } else if (!document->filepath(true).isEmpty()) {
    status = Ui::FormatSizeText(document->size);
  } else {
    status = Ui::FormatSizeText(document->size);
    icon = "folder-download-symbolic";
  }
  if (document->isVideoFile() || document->isAnimation() || document->isVideoMessage()) {
    if (document->loading()) {
      status = Ui::FormatDownloadText(document->loadOffset(), document->size);
    } else if (const auto duration = document->duration(); duration > 0) {
      status = Ui::FormatDurationText(duration / 1000);
    } else if (document->isAnimation()) {
      status = u"GIF"_q;
    }
  }
  gtk_label_set_text(GTK_LABEL(_documentStatus), U8(status).c_str());
  if (_documentButton) {
    gtk_button_set_icon_name(GTK_BUTTON(_documentButton), icon);
  }
}

void MessageWidget::updatePlayback(const Media::Player::TrackState &state) {
  if (!_voiceProgress || !_document) {
    return;
  }
  const auto mine = (state.id.audio() == _document && state.id.contextId() == _item->fullId());
  const auto playing = mine && Media::Player::ShowPauseIcon(state.state);
  gtk_button_set_icon_name(GTK_BUTTON(_documentButton), playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");
  if (mine && !Media::Player::IsStoppedOrStopping(state.state) && state.length > 0) {
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(_voiceProgress), double(state.position) / state.length);
    const auto frequency = state.frequency ? state.frequency : Media::Player::kDefaultFrequency;
    gtk_label_set_text(GTK_LABEL(_documentStatus),
                       U8(Ui::FormatDurationText(state.position / frequency) + u" / "_q
                          + Ui::FormatDurationText(state.length / frequency))
                           .c_str());
  } else {
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(_voiceProgress), 0.);
    gtk_label_set_text(GTK_LABEL(_documentStatus), U8(Ui::FormatDurationText(_document->duration() / 1000)).c_str());
  }
}

void MessageWidget::setFirstInGroup(bool first) {
  if (_name) {
    gtk_widget_set_visible(_name, first);
  }
  if (_avatar) {
    gtk_widget_set_opacity(_avatar, first ? 1. : 0.);
  }
  if (auto *line = _content; line && !_item->isService()) {
    gtk_widget_set_margin_top(line, first ? 6 : 1);
  }
}

void MessageWidget::highlight() {
  gtk_widget_add_css_class(_row, "highlighted");
  if (_highlightTimeout) {
    g_source_remove(_highlightTimeout);
  }
  _highlightTimeout = g_timeout_add(1500, +[](gpointer data) -> gboolean {
    auto *self = static_cast<MessageWidget *>(data);
    self->_highlightTimeout = 0;
    gtk_widget_remove_css_class(self->_row, "highlighted");
    return G_SOURCE_REMOVE;
  }, this);
}

void MessageWidget::setupGestures(GtkWidget *bubble) {
  auto *longPress = gtk_gesture_long_press_new();
  gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(longPress), TRUE);
  _signals.connect<void(GtkGestureLongPress *, double, double)>(longPress, "pressed", [=](GtkGestureLongPress *g, double x, double y) {
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    _chat->showMessageMenu(_item, bubble, x, y);
  });
  gtk_widget_add_controller(bubble, GTK_EVENT_CONTROLLER(longPress));
  auto *rightClick = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rightClick), GDK_BUTTON_SECONDARY);
  _signals.connect<void(GtkGestureClick *, int, double, double)>(rightClick, "pressed", [=](GtkGestureClick *, int, double x, double y) {
    _chat->showMessageMenu(_item, bubble, x, y);
  });
  gtk_widget_add_controller(bubble, GTK_EVENT_CONTROLLER(rightClick));
  // Swipe left to reply, as in the phone apps.
  // An unrestricted drag wins the touch sequence over the parent scrolled window. Pan denies
  // vertical movement early, so touching message text still scrolls the conversation.
  auto *swipe = gtk_gesture_pan_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(swipe), TRUE);
  _signals.connect<void(GtkGestureDrag *, double, double)>(swipe, "drag-end", [=](GtkGestureDrag *, double dx, double dy) {
    if (dx < -80 && std::abs(dy) < 40) {
      _chat->replyTo(_item);
    }
  });
  gtk_widget_add_controller(bubble, GTK_EVENT_CONTROLLER(swipe));
}

}  // namespace Contrail
