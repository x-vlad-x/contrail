#pragma once

#include "app.hpp"
#include "media/audio_player.hpp"
#include "td/store.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Contrail {

class ChatView;

// A message near the screen. Its address stays while TDLib swaps the object, as updateMessageSendSucceeded does:
// the row of a message being sent survives the new id.
struct MessageItem {
  Td::api::object_ptr<Td::api::message> data;
  // A sponsored message of a channel (ChatView::showSponsored): `data` is made from it and holds its content; this
  // keeps the rest. It goes right after the message `sponsoredAfter`.
  Td::api::object_ptr<Td::api::sponsoredMessage> sponsored;
  std::int64_t sponsoredAfter = 0;
  [[nodiscard]] std::int64_t id() const { return data->id_; }
  [[nodiscard]] bool local() const { return data->sending_state_ != nullptr; }  // Sending, or failed to.
};

// The photo of a message (or of its link preview) that has `file` among its sizes, null when there is none.
[[nodiscard]] const Td::api::photo *PhotoWithFile(const Td::api::MessageContent *content, Td::FileId file);

// What the player plays of a voice message or a song, nothing for other messages.
[[nodiscard]] std::optional<AudioPlayer::Track> PlayerTrack(const Td::api::message &message);

// What a reply shows of the message it answers.
struct ReplyPreview {
  bool ready = false;     // False: "Loading..." until the message comes.
  std::string name;
  std::string text;
  int color = -1;
  std::int64_t target = 0;  // In this chat; 0 when it can't be jumped to.
};

// The widgets of one message in a chat, or of an album (messages sent together, by their album id):
// one bubble with the sender, forward and reply headers, the media (a grid of photos and videos, or a
// column of files with their captions), the text, and the time with the read ticks; or a centered
// line for a service message. Each message of an album has its cell: taps, the menu, the progress
// and the highlight are its own.
class MessageWidget final {
 public:
  using Items = std::vector<MessageItem *>;

  MessageWidget(Items items, ChatView &chat);
  ~MessageWidget();

  [[nodiscard]] GtkWidget *row() const { return _row; }
  // The first message: the date, the sender and the place in the chat are its.
  [[nodiscard]] MessageItem *item() const { return _items.front(); }
  [[nodiscard]] MessageItem *lastItem() const { return _items.back(); }
  [[nodiscard]] const Items &items() const { return _items; }
  // The album got or lost messages (the next page, a deletion): rebuilds if they changed.
  void setItems(Items items);
  // Where a message is: its cell in an album, else the row.
  [[nodiscard]] GtkWidget *widgetFor(const MessageItem *item) const;

  // Rebuilds everything (a message was edited, its media changed...).
  void refresh();
  // A reply shows "Loading..." until the message it replies to comes.
  [[nodiscard]] bool replyPending() const { return _replyPending; }
  // Updates the time and the read ticks only.
  void refreshFooter();
  // Updates the reactions under the message only: their numbers in place, or the chips made again when they
  // changed otherwise. True when the chips were made again.
  bool refreshReactions();
  // For the debug state: how many times rows were built, built again (refresh()), and their chips of reactions
  // made.
  [[nodiscard]] static std::uint64_t BuildCount();
  [[nodiscard]] static std::uint64_t RebuildCount();
  [[nodiscard]] static std::uint64_t ReactionBarCount();
  // A file this row shows was downloaded further.
  void fileChanged(const Td::File &file);
  // In a run of messages from the same sender only the first shows the name and the avatar.
  void setFirstInGroup(bool first);
  void highlight(const MessageItem *item = nullptr);
  // Only the rows near the screen keep their pictures decoded: far off they show the blurred thumbnail, and the
  // texture and its memory go. The downloads and the sizes stay. A new row of a chat has its pictures decoded
  // once the chat finds it near.
  void setPicturesNear(bool near);
  // For the debug commands: what the cells show (their files, downloads, statuses), the sizes of the bubble and its
  // texts; a tap on the media (or the button) `index`, as a finger does it; a tap on a link or spoiler with `part`.
  [[nodiscard]] std::string debugDescribe() const;
  void debugTap(int index);
  [[nodiscard]] bool debugTapText(const std::string &part);

 private:
  enum class PictureKind { None, Photo, Sticker, Thumbnail };
  // The media of one message.
  struct Cell {
    MessageItem *item = nullptr;
    GtkWidget *widget = nullptr;  // The cell of an album.
    GtkWidget *picture = nullptr;
    PictureKind kind = PictureKind::None;
    int width = 0;  // The size the picture is shown at.
    int height = 0;
    Td::FileId pictureFile = 0;
    Td::FileId thumbnailFile = 0;  // A sticker's, until the sticker itself comes.
    bool pictureFinal = false;
    std::string sourcePath;  // The downloaded file the picture shows, decoded again when it comes near.
    int sourceQuality = 0;
    Td::FileId documentFile = 0;
    GtkWidget *documentButton = nullptr;
    GtkWidget *documentStatus = nullptr;
    GtkWidget *loadProgress = nullptr;  // A file's download.
    GtkWidget *status = nullptr;        // Download progress over a picture.
    GtkWidget *play = nullptr;          // Over a video: its play button, or while it downloads the ring of its progress.
    GtkWidget *ring = nullptr;
    bool voice = false;
    int duration = 0;
    // A voice message's or a song's bar: the position, and how much has come while it streams (its fill level).
    GtkWidget *voiceProgress = nullptr;
    bool voiceNote = false;
    bool seeking = false;  // A finger or the mouse moves the position: the bar shows where it is.
    std::string fallbackStatus;  // A video's length, "GIF": what stands there without a download.
  };

  void build();
  [[nodiscard]] bool isAlbumGrid() const;
  [[nodiscard]] bool isService() const;
  Cell &addCell(MessageItem *item);
  GtkWidget *buildReply(MessageItem *item);
  GtkWidget *buildForwarded(MessageItem *item);
  GtkWidget *buildMedia(Cell &cell, int maxWidth);
  GtkWidget *buildAlbumGrid();
  GtkWidget *buildGridCell(Cell &cell, int width, int height);
  GtkWidget *buildPhoto(Cell &cell, const Td::api::photo &photo, int maxWidth, bool preview = false);
  GtkWidget *buildSticker(Cell &cell, const Td::api::sticker &sticker);
  GtkWidget *buildDocument(Cell &cell, const Td::api::file &file, const std::string &name, const char *icon);
  GtkWidget *buildVoice(Cell &cell, const Td::api::file &file, int duration, const std::string &title, bool voiceNote);
  [[nodiscard]] AudioPlayer::Track trackOf(const Cell &cell) const;
  void setupSeeking(Cell &cell);
  void cancelSeeking();
  void updatePlayback(const AudioPlayer::State &state);
  GtkWidget *buildVideoThumb(Cell &cell, int width, int height, const Td::api::minithumbnail *mini,
                             const Td::api::thumbnail *thumbnail, const Td::api::file &file, int duration, bool gif,
                             bool round, int maxWidth);
  GtkWidget *buildContact(const Td::api::contact &contact);
  GtkWidget *buildPoll(const Td::api::poll &poll);
  GtkWidget *buildWebPage(MessageItem *item);
  GtkWidget *buildSponsored(MessageItem *item);
  GtkWidget *buildFooter();
  void setupDoubleTap(GtkWidget *bubble);
  GtkWidget *makeStatus();
  void addPlayIcon(Cell &cell, GtkWidget *overlay);
  void watchPicture(Cell &cell, const Td::api::file *file);
  void showPicture(Cell &cell, const Td::File &file);
  void updateDocumentState(Cell &cell, const Td::File &file);
  void updateStatus(Cell &cell, const Td::File &file);
  void setupGestures(GtkWidget *widget, MessageItem *item, bool swipe);
  void openOnTap(GtkWidget *widget, MessageItem *item, Td::FileId file);
  void clearFiles();
  [[nodiscard]] MessageItem *mainItem() const;

  Items _items;
  ChatView &_chat;
  GtkWidget *_row = nullptr;
  GtkWidget *_content = nullptr;
  GtkWidget *_name = nullptr;
  GtkWidget *_avatar = nullptr;
  GtkWidget *_footer = nullptr;
  GtkWidget *_footerTime = nullptr;
  GtkWidget *_footerStatus = nullptr;
  GtkWidget *_reactions = nullptr;
  std::string _reactionsLayout;  // What the chips show (ReactionsLayoutKey), and their numbers.
  std::string _reactionsCounts;
  bool _replyPending = false;
  bool _picturesNear = false;
  std::vector<std::unique_ptr<Cell>> _cells;
  // What a tap on each of the row's media does, in the order they were made (debugTap()).
  std::vector<std::function<void()>> _taps;
  GtkWidget *_highlighted = nullptr;
  guint _highlightTimeout = 0;
  SignalConnections _signals;
  Td::Lifetime _playerLifetime;  // While a cell plays through the player: made again with the cells.
  bool _playerWatched = false;
};

}  // namespace Contrail
