#pragma once

#include "app.hpp"

#include "data/data_cloud_file.h"
#include "media/audio/media_audio.h"

#include <memory>
#include <vector>

class HistoryItem;
class DocumentData;
class PhotoData;

namespace Data {
class PhotoMedia;
class DocumentMedia;
class MediaLocation;
} // namespace Data

namespace Contrail {

class ChatView;

// The widgets of one message in a chat, or of an album (messages sent together, by their group id):
// one bubble with the sender, forward and reply headers, the media (a grid of photos and videos, or a
// column of files with their captions), the text, and the time with the read ticks; or a centered
// line for a service message. Each message of an album has its cell: taps, the menu, the progress
// and the highlight are its own.
class MessageWidget final {
public:
  using Items = std::vector<not_null<HistoryItem *>>;

  MessageWidget(Items items, not_null<ChatView *> chat);
  ~MessageWidget();

  [[nodiscard]] GtkWidget *row() const { return _row; }
  // The first message: the date, the sender and the place in the chat are its.
  [[nodiscard]] not_null<HistoryItem *> item() const { return _items.front(); }
  [[nodiscard]] not_null<HistoryItem *> lastItem() const { return _items.back(); }
  [[nodiscard]] const Items &items() const { return _items; }
  // The album got or lost messages (the next page, a deletion): rebuilds if they changed.
  void setItems(Items items);
  // Where a message is: its cell in an album, else the row.
  [[nodiscard]] GtkWidget *widgetFor(const HistoryItem *item) const;

  // Rebuilds everything (a message was edited, its media changed...).
  void refresh();
  // Updates the time and the read ticks only.
  void refreshFooter();
  // The reactions changed (and maybe nothing else): only their bar is rebuilt. False when they didn't change.
  bool refreshReactions();
  // A download or an upload went on: shows the pictures that arrived and the progress. True when
  // nothing is pending anymore.
  bool refreshMedia();
  // In a run of messages from the same sender only the first shows the name and the avatar.
  void setFirstInGroup(bool first);
  void updatePlayback(const Media::Player::TrackState &state);
  void highlight(const HistoryItem *item = nullptr);

private:
  enum class PictureKind { None, Photo, Sticker, Thumbnail, Map };
  // The media of one message.
  struct Cell {
    not_null<HistoryItem *> item;
    GtkWidget *widget = nullptr;  // The cell of an album.
    GtkWidget *picture = nullptr;
    bool pictureFinal = false;
    PictureKind kind = PictureKind::None;
    // Kept only while the picture is loading: the texture holds what is shown.
    std::shared_ptr<Data::PhotoMedia> photoMedia;
    std::shared_ptr<Data::DocumentMedia> documentMedia;
    PhotoData *photo = nullptr;
    DocumentData *document = nullptr;
    GtkWidget *documentButton = nullptr;
    GtkWidget *documentStatus = nullptr;
    GtkWidget *voiceProgress = nullptr;
    GtkWidget *status = nullptr;  // Upload or download progress over a picture.
    // A location: the map from Telegram's servers, and a live location's state.
    std::unique_ptr<Data::CloudImage> map;
    std::shared_ptr<QImage> mapView;
    GtkWidget *liveStatus = nullptr;
    GtkWidget *liveRemaining = nullptr;
  };

  void build();
  [[nodiscard]] bool isAlbumGrid() const;
  Cell &addCell(not_null<HistoryItem *> item);
  GtkWidget *buildReply(not_null<HistoryItem *> item);
  GtkWidget *buildForwarded(not_null<HistoryItem *> item);
  GtkWidget *buildMedia(Cell &cell, int maxWidth);
  GtkWidget *buildAlbumGrid();
  GtkWidget *buildGridCell(Cell &cell, int width, int height);
  GtkWidget *buildPhoto(Cell &cell, not_null<PhotoData *> photo, int maxWidth);
  GtkWidget *buildSticker(Cell &cell, not_null<DocumentData *> document);
  GtkWidget *buildDocument(Cell &cell, not_null<DocumentData *> document);
  GtkWidget *buildVoice(Cell &cell, not_null<DocumentData *> document);
  GtkWidget *buildVideoThumb(Cell &cell, not_null<DocumentData *> document, int maxWidth);
  GtkWidget *buildWebPage(not_null<HistoryItem *> item);
  GtkWidget *buildLocation(Cell &cell, not_null<Data::MediaLocation *> location);
  void refreshLive();
  void setupDoubleTap(GtkWidget *bubble);
  GtkWidget *buildFooter();
  GtkWidget *makeStatus();
  bool refreshPicture(Cell &cell);
  void updateDocumentState(Cell &cell);
  bool updateStatus(Cell &cell);  // True while a transfer goes on.
  void setupGestures(GtkWidget *widget, not_null<HistoryItem *> item, bool swipe);
  [[nodiscard]] not_null<HistoryItem *> mainItem() const;

  Items _items;
  const not_null<ChatView *> _chat;
  GtkWidget *_row = nullptr;
  GtkWidget *_content = nullptr;
  GtkWidget *_name = nullptr;
  GtkWidget *_avatar = nullptr;
  GtkWidget *_footer = nullptr;
  GtkWidget *_footerTime = nullptr;
  GtkWidget *_footerStatus = nullptr;
  GtkWidget *_reactions = nullptr;
  QString _reactionsKey;
  guint _liveTimer = 0;
  std::vector<std::unique_ptr<Cell>> _cells;
  GtkWidget *_highlighted = nullptr;
  guint _highlightTimeout = 0;
  SignalConnections _signals;
};

}  // namespace Contrail
