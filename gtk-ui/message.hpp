#pragma once

#include "app.hpp"

#include "media/audio/media_audio.h"

#include <memory>

class HistoryItem;
class DocumentData;
class PhotoData;

namespace Data {
class PhotoMedia;
class DocumentMedia;
} // namespace Data

namespace Contrail {

class ChatView;

// The widgets of one message in a chat: a bubble with the sender, forward and reply headers, media,
// text and the time with the read ticks; or a centered line for a service message.
class MessageWidget final {
public:
  MessageWidget(not_null<HistoryItem *> item, not_null<ChatView *> chat);
  ~MessageWidget();

  [[nodiscard]] GtkWidget *row() const { return _row; }
  [[nodiscard]] not_null<HistoryItem *> item() const { return _item; }

  // Rebuilds everything (the message was edited, its media changed...).
  void refresh();
  // Updates the time and the read ticks only.
  void refreshFooter();
  // A download finished: shows the pictures that arrived. True when nothing is pending anymore.
  bool refreshMedia();
  // In a run of messages from the same sender only the first shows the name and the avatar.
  void setFirstInGroup(bool first);
  void updatePlayback(const Media::Player::TrackState &state);
  void highlight();

private:
  void build();
  GtkWidget *buildReply();
  GtkWidget *buildForwarded();
  GtkWidget *buildMedia();
  GtkWidget *buildPhoto(not_null<PhotoData *> photo, int maxWidth);
  GtkWidget *buildSticker(not_null<DocumentData *> document);
  GtkWidget *buildDocument(not_null<DocumentData *> document);
  GtkWidget *buildVoice(not_null<DocumentData *> document);
  GtkWidget *buildVideoThumb(not_null<DocumentData *> document, int maxWidth);
  GtkWidget *buildWebPage();
  GtkWidget *buildFooter();
  void updateDocumentState();
  void setupGestures(GtkWidget *bubble);

  const not_null<HistoryItem *> _item;
  const not_null<ChatView *> _chat;
  GtkWidget *_row = nullptr;
  GtkWidget *_content = nullptr;
  GtkWidget *_name = nullptr;
  GtkWidget *_avatar = nullptr;
  GtkWidget *_footer = nullptr;
  GtkWidget *_footerTime = nullptr;
  GtkWidget *_footerStatus = nullptr;

  // Pictures waiting for a download.
  std::shared_ptr<Data::PhotoMedia> _photoMedia;
  std::shared_ptr<Data::DocumentMedia> _documentMedia;
  GtkWidget *_picture = nullptr;
  bool _pictureFinal = false;
  enum class PictureKind { None, Photo, Sticker, Thumbnail } _pictureKind = PictureKind::None;

  // Files and voice messages.
  DocumentData *_document = nullptr;
  GtkWidget *_documentButton = nullptr;
  GtkWidget *_documentStatus = nullptr;
  GtkWidget *_voiceProgress = nullptr;
  bool _playing = false;
};

}  // namespace Contrail
