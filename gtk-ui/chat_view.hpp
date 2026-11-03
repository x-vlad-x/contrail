#pragma once

#include "app.hpp"
#include "attach_media.hpp"
#include "td/store.hpp"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Contrail {

class ChatLiveBar;
class AttachDialog;
class AudioPlayer;
class ClipboardReader;
class VoiceRecordBar;
class MainWindow;
class MessageWidget;
struct ForwardDraft;
struct MessageItem;
struct ReplyPreview;

// An open chat: its messages and the message field. Only the messages near the screen are held, each with its row:
// the ones that go are in TDLib's database and come back from there. A topic of a group with topics is a chat of
// its own: only its messages, sent into it, with its own draft and read mark.
class ChatView final {
 public:
  ChatView(Td::Store &store, Td::ChatId chat, MainWindow &window, std::int64_t showAt = 0, std::int32_t topic = 0);
  ~ChatView();

  [[nodiscard]] GtkWidget *widget() const { return _root; }
  [[nodiscard]] Td::ChatId chat() const { return _chat; }
  [[nodiscard]] std::int32_t topic() const { return _topic; }  // 0: the whole chat.
  [[nodiscard]] Td::Store &store() const { return _store; }
  [[nodiscard]] MainWindow &window() const { return _window; }
  // Ends with the view: what is asked for it is dropped then.
  [[nodiscard]] const Td::Lifetime &lifetime() const { return _lifetime; }
  // A message held near the screen, null otherwise.
  [[nodiscard]] const Td::api::message *heldMessage(std::int64_t id) const;
  // A dialog about this chat's messages (who reacted, all the reactions): it goes when the chat does.
  void holdDialog(AdwDialog *dialog);
  void showAt(std::int64_t message);
  // Cancels a reply, an edit or the messages to forward; false if there was nothing to cancel.
  bool handleEscape();
  // Other messages wait here to be forwarded, or none any more (MainWindow::setForwardDraft).
  void forwardChanged();

  // What the messages ask for.
  void openChat(Td::ChatId chat);
  // A photo opens in the viewer (photo_viewer.hpp); a picture that isn't one of a photo's sizes opens outside.
  void openPhoto(const MessageItem &item, Td::FileId file);
  // The viewer's Save: as the menu's, the photo copied to Pictures once it is downloaded.
  void saveFromViewer(std::int64_t id) { saveFile(id); }
  // A tap on a file: opens it if it is here, stops its download if it runs, starts one otherwise. Videos, GIFs,
  // stickers and pictures open by themselves once downloaded, as in 0.3.1; other files with a second tap.
  void openDocument(const MessageItem &item, Td::FileId file);
  [[nodiscard]] bool opensWhenLoaded(Td::FileId file) const;
  void showMessageMenu(const MessageItem &item, GtkWidget *widget, double x, double y);
  // The menu open on a message inside `row` goes now: the row is being destroyed or built again.
  void dropMessageMenu(GtkWidget *row);
  void replyTo(const MessageItem &item);
  [[nodiscard]] ReplyPreview replyPreview(const MessageItem &item) const;
  void sendText(const std::string &text);  // Bot commands from links.
  // The sponsored message's button, text or media was tapped (`media`): Telegram counts it.
  void sponsoredClicked(const MessageItem &item, bool media);
  // Locations: the picker sends to this chat (or topic), as a reply to the message chosen; a location's message
  // opens on a map.
  void chooseLocation();
  void openLocation(const MessageItem &item);
  void sendContent(Td::api::object_ptr<Td::api::InputMessageContent> content);
  // Who shares a live location here, under the header (null in a channel).
  [[nodiscard]] ChatLiveBar *liveBar() const { return _liveBar.get(); }
  // The window's player, told what follows in this chat: after a voice message the next one, after a song the next
  // song, as far as they are loaded. Null without an account.
  [[nodiscard]] AudioPlayer *player();
  // A voice message may be recorded here now: the field is there and nothing is being edited.
  [[nodiscard]] bool canRecordVoice() const;
  [[nodiscard]] std::int64_t replyToId() const { return _replyToId; }
  // The voice message recorded here was sent (it took the reply along) or deleted.
  void recordingEnded(bool sent);
  [[nodiscard]] VoiceRecordBar *recordBar() const { return _recordBar.get(); }
  // A tap on a file of a message still being sent: the upload stops and the message goes, as TDLib does it.
  void cancelUpload(const MessageItem &item);
  // paste (as Ctrl+V) | files PATH|PATH (as the file chooser) | copy-image PATH, copy-files PATH|PATH, copy-text TEXT
  // (the clipboard as another program would fill it) | field (the field's text) | menu file|location (an item of the
  // attach menu, as a tap on it) | anything else to the dialog (AttachDialog::debugAction).
  void debugAttach(const std::string &action);

  // tools/phone.sh td-cmd: what the list holds, and the actions of a finger.
  void debugState() const;
  void debugScroll(const std::string &where);
  void debugSend(const std::string &text);
  void debugType(const std::string &text);  // Into the field, not sent: a draft.
  void debugReply();
  void debugEdit(const std::string &text);
  void debugDelete();
  void debugEditLast();  // The up arrow in an empty field.
  void debugForward();  // The newest message, to the chats to choose from.
  void debugForwardMode(bool names);
  void debugForwardCancel();
  void debugForwardMenu();
  void debugMessageMenu(std::int64_t number);  // Of message N (its server number), 0: the newest.
  void debugPin(std::int64_t number, bool pin);  // As the menu's Pin or Unpin does: the question comes.
  void debugPinnedBar(const std::string &what);  // tap | close | list | state
  void debugPinnedList(const std::string &what);  // state | pick N | action
  // Reactions of message N (its server number), 0: the newest. A key is the emoji, or "custom:ID".
  void debugReact(const std::string &key, std::int64_t number);  // As a tap on its chip, or in the menu.
  void debugUnreact(std::int64_t number);  // Takes back the user's own.
  void debugReacted(std::int64_t number);  // Who reacted.
  void debugReactQuick(std::int64_t number);  // A double tap.
  void debugReactionsMore(std::int64_t number);  // The button with all of them in the open menu.
  void debugReactionsState(std::int64_t number);
  void debugMenuState() const;  // The items of the message menu open.
  void debugMenuDo(const std::string &action);  // As a tap on an item of it: copy, save, delete, retry...
  // Message N (its server number, 0: the newest, -1: the sponsored one): what its media show, a tap on its media
  // (or the sponsored button) `index`, a tap on a link or a spoiler of its text with `part` in it.
  void debugMedia(std::int64_t number);
  void debugTap(std::int64_t number, int index);
  void debugTapText(std::int64_t number, const std::string &part);

  // Where the screen is, to come back to it when the window is built again after the background: the first message
  // whose top is on the screen and how far down. Empty at the end of the chat, which it opens at anyway.
  struct Place {
    std::int64_t message = 0;
    double y = 0.;
  };
  [[nodiscard]] Place place() const;
  void restorePlace(Place place);
  void debugSponsored() const;
  // play N (message N's voice or song, as its button) | pause | seek FRACTION | drag FRACTION (along the bar,
  // let go there) | stop.
  void debugPlayer(const std::string &what);

 private:
  // One call of a function some time later. Started again, it waits from the start.
  class Timer final {
   public:
    explicit Timer(std::function<void()> callback) : _callback(std::move(callback)) {}
    ~Timer() { cancel(); }
    void callOnce(guint ms);
    void cancel();
    [[nodiscard]] bool active() const { return _source != 0; }

   private:
    std::function<void()> _callback;
    guint _source = 0;
  };
  enum class Page { Initial, Start, Older, Newer };
  using Items = std::vector<MessageItem *>;

  void start();
  // The chat, or the topic: what is read, unread and drafted here.
  [[nodiscard]] const Td::Chat *readState() const;
  [[nodiscard]] bool belongs(const Td::api::message &message) const;
  [[nodiscard]] Td::api::object_ptr<Td::api::MessageTopic> topicObject() const;
  void topicChanged();
  void buildHeader();
  void buildList();
  void buildComposer();
  void subscribe();
  void refreshHeader();
  void refreshComposerState();

  void loadInitial();
  void loadOlder();
  void loadNewer();
  void requestHistory(std::int64_t from, int offset, int limit, Page page);
  void applyPage(Td::api::messages &messages, Page page);
  void pageFailed(const std::string &text);
  void reload(std::int64_t showAt);
  void unload();
  void jumpUp(double gap);
  void jumpToMessage(std::int64_t id, double y);
  void jumpToStart();

  // The messages, oldest first and the ones being sent last.
  [[nodiscard]] MessageItem *find(std::int64_t id) const;
  [[nodiscard]] std::int64_t oldestId() const;
  [[nodiscard]] std::int64_t newestId() const;
  MessageItem *insert(Td::api::object_ptr<Td::api::message> message);
  void resort();
  void remove(const std::vector<std::int64_t> &ids);
  void updated(Td::api::Update &update);
  void messageSent(Td::api::object_ptr<Td::api::message> message, std::int64_t oldId);

  void sync();
  MessageWidget *addRow(Items items);
  void setRowItems(MessageWidget *row, Items items);
  void forgetRow(MessageWidget *row);
  [[nodiscard]] MessageWidget *rowFor(const MessageItem *item) const;
  void refreshItem(const MessageItem *item);
  void refreshFooters();
  void trimRows();
  void repliedLoaded(std::int64_t id);
  [[nodiscard]] std::vector<MessageWidget *> visibleRows() const;
  [[nodiscard]] static int compareRows(GtkListBoxRow *a, GtkListBoxRow *b, gpointer data);
  static void updateRowHeader(GtkListBoxRow *row, GtkListBoxRow *before, gpointer data);

  void scrollToBottom();
  void scrollToItem(const MessageItem *item, bool highlight);
  bool scrollToLoaded(std::int64_t id);
  void scrolled();
  void scheduleViewportUpdate();
  void updateViewport();
  void preloadVisibleEdges();
  void updateNearPictures();
  [[nodiscard]] int listHeight() const;
  void setTopReserve(int reserve, double shift);
  void maintainTopReserve();
  [[nodiscard]] bool scrollIdle() const;
  [[nodiscard]] bool scrollIdleIgnoringShift() const;
  [[nodiscard]] bool atBottom() const;
  void markAsRead();

  void send();
  void applyEdit(const std::string &text);
  void edit(const MessageItem &item);
  void editLast();
  void cancelReplyOrEdit();
  void showReplyBar(const std::string &title, const std::string &text, const char *icon);
  void refreshSendButton();
  void setComposerText(const std::string &text);
  [[nodiscard]] std::string composerText() const;
  void composerChanged();
  void saveDraft();
  void draftChanged();
  void confirmDelete(std::int64_t id, bool forAll, bool onlyForSelf);
  // The menu's Save: the file (or the photo) copied to Downloads (Pictures), once it is downloaded.
  void saveFile(std::int64_t id);
  [[nodiscard]] MessageItem *debugItem(std::int64_t number) const;

  // Telegram's sponsored messages in channels and bots (required by the Telegram API Terms of Service, 3.3): asked
  // for once the end of the chat is shown, put after the newest message once their media is downloaded (TDLib asks
  // for that), and told to TDLib once seen whole.
  void showSponsored();
  void placeSponsored();
  void reportSponsoredView();

  [[nodiscard]] const ForwardDraft *forwardDraft() const;
  [[nodiscard]] bool forwarding() const { return forwardDraft() != nullptr; }
  void forward(const MessageItem &item);
  void refreshForwardBar();
  void cancelForward();
  void showForwardMenu();
  void setForwardNames(bool names);
  void sendForward();

  // The pinned message bar under the header (Telegram Desktop's PinnedTracker): the newest message pinned above the
  // bottom of the screen; a tap shows it, and then the one pinned before it. Only the ids of the pinned messages are
  // held, and the one the bar shows.
  void buildPinnedBar();
  void loadPinned();
  void pinnedPage(Td::api::foundChatMessages &found, bool first);
  void pinnedChanged(std::int64_t id, bool pinned);
  void updatePinnedViewer();
  void setPinnedAround(std::int64_t around);
  void fetchPinnedMessage();
  void refreshPinnedBar();
  void setPinnedPreview();
  void showPinnedPicture(const std::string &path);
  void pinnedBarClicked();
  void pinnedCloseClicked();
  [[nodiscard]] bool canPinMessages() const;
  [[nodiscard]] bool pinnedHiddenByUser() const;
  [[nodiscard]] int pinnedIndex() const;
  void showPinnedList();
  void confirmPin(std::int64_t id);
  void confirmUnpin(std::int64_t id);
  void confirmUnpinAll();
  void confirmHidePinned();

  Td::Store &_store;
  const Td::ChatId _chat;
  const std::int32_t _topic;
  std::shared_ptr<void> _topicHold;  // The store keeps the group's topics while this is open.
  bool _topicKnown = false;
  MainWindow &_window;
  Td::Lifetime _lifetime;
  Td::Lifetime _pageLifetime;  // Ends with what the running requests for pages were for.

  GtkWidget *_root = nullptr;
  GtkWidget *_title = nullptr;
  GtkWidget *_list = nullptr;
  GtkWidget *_topSpacer = nullptr;
  GtkWidget *_scroll = nullptr;
  GtkWidget *_toBottom = nullptr;
  GtkWidget *_loading = nullptr;
  GtkWidget *_retry = nullptr;
  GtkAdjustment *_adjustment = nullptr;
  GtkWidget *_composerStack = nullptr;
  GtkWidget *_input = nullptr;
  GtkWidget *_sendButton = nullptr;
  GtkWidget *_joinButton = nullptr;
  GtkWidget *_replyBar = nullptr;
  GtkWidget *_replyIcon = nullptr;
  GtkWidget *_replyTitle = nullptr;
  GtkWidget *_replyText = nullptr;
  GtkWidget *_pinnedBar = nullptr;
  GtkWidget *_pinnedTitle = nullptr;
  GtkWidget *_pinnedText = nullptr;
  GtkWidget *_pinnedPreview = nullptr;
  GtkWidget *_pinnedListButton = nullptr;
  GtkWidget *_pinnedClose = nullptr;
  GtkWidget *_pinnedDialog = nullptr;  // The list of them while it is open.
  std::vector<GtkWidget *> _dialogs;  // holdDialog()
  std::unique_ptr<ChatLiveBar> _liveBar;
  SignalConnections _signals;

  std::vector<std::unique_ptr<MessageItem>> _items;
  std::unordered_map<MessageWidget *, std::unique_ptr<MessageWidget>> _rows;
  std::unordered_map<const MessageItem *, MessageWidget *> _itemRows;
  std::unordered_set<MessageWidget *> _nearRows;  // Those with their pictures decoded (updateNearPictures).
  // The messages replies point to, when they aren't near the screen. Null: there is no such message any more.
  std::unordered_map<std::int64_t, Td::api::object_ptr<Td::api::message>> _replied;
  mutable std::set<std::int64_t> _repliedAsked;
  std::set<std::int64_t> _viewed;

  std::int64_t _unreadAfter = 0;  // The last message read when the chat opened.
  MessageWidget *_unreadRow = nullptr;  // The bar goes before it.
  std::int64_t _pendingShowAt = 0;
  std::int64_t _scrollTarget = 0;
  std::optional<double> _scrollTargetY;
  bool _scrollTargetHighlight = false;
  bool _reachedTop = false;
  bool _reachedBottom = false;
  bool _initialLoaded = false;
  bool _loadFailed = false;
  bool _loadingOlder = false;
  bool _loadingNewer = false;
  bool _applyingPage = false;
  bool _stickToBottom = true;
  bool _adjustingScroll = false;
  bool _touchActive = false;
  int _topReserve = 0;
  double _pendingShift = 0.;
  double _shiftBase = 0.;
  guint _viewportSource = 0;
  guint _closeSource = 0;

  std::int64_t _replyToId = 0;
  std::unique_ptr<VoiceRecordBar> _recordBar;
  // Files to send: chosen, or pasted (a paste is read in the background first).
  void chooseFiles();
  void paste();
  void pasteText(const std::string &text);
  void openAttach(const std::vector<std::string> &paths, GdkTexture *image = nullptr, const std::string &textOnCancel = {});
  bool sendAttachments(const std::vector<AttachFile> &files, bool compress, bool group, const std::string &caption,
                       std::int64_t replyTo);
  std::unique_ptr<ClipboardReader> _pasteReader;
  std::weak_ptr<AttachDialog> _attach;
  bool _forwardShown = false;  // The bar shows the messages to forward.
  std::int64_t _editing = 0;
  bool _editPending = false;
  std::string _beforeEditText;
  std::int64_t _beforeEditReplyTo = 0;
  bool _settingComposerText = false;
  std::string _savedDraft;
  std::int64_t _savedDraftReply = 0;
  gint64 _lastTyping = 0;

  Td::Lifetime _pinnedLifetime;         // Ends with the search of the pinned messages it was for.
  std::vector<std::int64_t> _pinnedIds;  // Oldest first.
  int _pinnedTotal = 0;
  bool _pinnedLoaded = false;
  std::int64_t _pinnedAround = std::numeric_limits<std::int64_t>::max();  // The bar shows the last one before it.
  std::int64_t _pinnedCurrent = 0;
  std::int64_t _pinnedClicked = 0;  // Shown with a tap on the bar: the bar goes on to the one before it.
  Td::api::object_ptr<Td::api::message> _pinnedMessage;  // The one the bar shows, once it came.
  Td::FileId _pinnedFile = 0;  // Its picture, being downloaded.
  double _lastScrollValue = 0.;
  std::uint64_t _interactionUpdates = 0;  // Of held messages, for the debug state.
  GtkWidget *_messageMenu = nullptr;  // Open on a message (a weak pointer).

  // True: opens by itself once downloaded (a video); false: only if the answer finds it here already.
  std::unordered_map<Td::FileId, bool> _openWhenLoaded;
  struct SaveTarget {
    std::string name;  // Empty: the downloaded file's.
    bool photo = false;
  };
  std::unordered_map<Td::FileId, SaveTarget> _saveWhenLoaded;

  std::unique_ptr<MessageItem> _sponsored;
  std::set<Td::FileId> _sponsoredWaiting;  // Its media, being downloaded before it is shown.
  bool _sponsoredAsked = false;
  bool _sponsoredPlaced = false;
  bool _sponsoredViewed = false;
  int _sponsoredClicks = 0;

  Timer _readTimer;
  Timer _statusTimer;
  Timer _reserveTimer;
  Timer _jumpTimer;
  Timer _draftTimer;
  Timer _pinnedTimer;  // Pins changed in a group with topics: which of them are this topic's is asked again.
};

}  // namespace Contrail
