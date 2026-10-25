#pragma once

#include "app.hpp"

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_messages.h"

#include <rpl/lifetime.h>

#include <optional>
#include <set>
#include <unordered_map>

class History;
class HistoryItem;
class PeerData;
class PhotoData;
class DocumentData;

namespace Data {
class Thread;
class ForumTopic;
} // namespace Data

namespace Ui {
struct PreparedList;
class SendFilesWay;
} // namespace Ui

namespace Api {
struct SendAction;
} // namespace Api

namespace HistoryView {
class PinnedTracker;
} // namespace HistoryView

namespace Contrail {

class MainWindow;
class MessageWidget;
class AttachDialog;
class ClipboardReader;
class ChatLiveLocations;

// An open chat: the messages (loaded page by page through the core, like Telegram Desktop's
// HistoryWidget does) and the message field. A topic of a group with topics is a chat of its own:
// its messages come from the core's list of the topic (Data::RepliesList), as in RepliesWidget.
class ChatView final : public base::has_weak_ptr {
public:
  // `showAtY`: where on the screen to put the message `showAt` (the place it had before the window was
  // closed); by default a third from the top, highlighted.
  ChatView(not_null<Data::Thread *> thread, not_null<MainWindow *> window, MsgId showAt,
           std::optional<double> showAtY = std::nullopt);
  ~ChatView();

  [[nodiscard]] GtkWidget *widget() const { return _root; }
  [[nodiscard]] not_null<History *> history() const { return _history; }
  [[nodiscard]] not_null<Data::Thread *> thread() const { return _thread; }
  [[nodiscard]] not_null<MainWindow *> window() const { return _window; }

  void showAt(MsgId id);
  // The message at the top of the screen and its place there; none at the bottom of the chat.
  [[nodiscard]] std::pair<MsgId, double> scrollState() const;
  bool handleEscape();
  // Short of memory: only the rows near the screen stay.
  void releaseMemory();  // Cancels a reply or an edit; false if there was nothing to cancel.

  // Used by the messages.
  void openPeer(not_null<PeerData *> peer);
  void openPhoto(not_null<PhotoData *> photo, not_null<HistoryItem *> item);
  void openDocument(not_null<DocumentData *> document, not_null<HistoryItem *> item);
  void openLocation(not_null<HistoryItem *> item);
  void refreshMessage(FullMsgId id);
  void showMessageMenu(not_null<HistoryItem *> item, GtkWidget *widget, double x, double y);
  void replyTo(not_null<HistoryItem *> item);
  void forward(not_null<HistoryItem *> item);
  void sendText(const QString &text);  // Bot commands from links.
  void debugScroll(bool top);
  void debugCheckDraft();
  void debugHistory(const QString &action);
  void debugAttach(const QString &action);

private:
  void buildHeader();
  void buildList();
  void buildComposer();
  void refreshHeader();
  void refreshComposerState();

  void loadInitial();
  void loadOlder();
  void loadNewer();
  void requestHistory(MsgId offsetId, int addOffset, int limit, bool older, bool initial);
  void viewTopic();
  void applyTopicSlice(Data::MessagesSlice &&slice);
  [[nodiscard]] bool loadedAtTop() const;
  [[nodiscard]] bool loadedAtBottom() const;
  // The loaded messages have rows only in a window around the screen (the rest go when the list stands still):
  // a long scroll through a chat doesn't keep thousands of rows with their pictures. Coming back, the rows are
  // made again from the loaded messages, before more are asked from the server.
  [[nodiscard]] bool shownAtTop() const { return loadedAtTop() && !_windowMin; }
  [[nodiscard]] bool shownAtBottom() const { return loadedAtBottom() && !_windowMax; }
  [[nodiscard]] std::vector<not_null<HistoryItem *>> loadedItems() const;
  [[nodiscard]] std::vector<not_null<HistoryItem *>> items() const;  // In the window.
  void windowAround(MsgId id);  // Rows for the messages around this one, or the newest ones for 0.
  void showWindowAround(MsgId id);
  bool extendWindowUp();
  bool extendWindowDown();
  void trimRows(bool force = false, double keep = 0.);
  void reload(MsgId showAt);
  void sync();
  MessageWidget *addRow(std::vector<not_null<HistoryItem *>> items);
  void setRowItems(MessageWidget *row, std::vector<not_null<HistoryItem *>> items);
  void forgetRow(MessageWidget *row);
  void removeRow(const HistoryItem *item);
  MessageWidget *rowFor(const HistoryItem *item) const;
  void refreshItem(const HistoryItem *item);
  // The core says a message's data changed: only the reactions, if it was them.
  void refreshItemData(const HistoryItem *item);
  void buildTopBars();
  void refreshLiveBar();
  // The pinned message bar (HistoryWidget's PinnedTracker): the pinned message above the bottom of the screen;
  // a tap shows it, and then the one pinned before it.
  void setupPinned();
  void refreshPinnedBar();
  void updatePinnedViewer();
  void pinnedBarClicked();
  [[nodiscard]] bool pinnedHiddenByUser() const;
  void showPinnedList();
  void confirmPin(not_null<HistoryItem *> item);
  void confirmUnpin(not_null<HistoryItem *> item);
  void confirmUnpinAll();
  void confirmHidePinned();
  void readReactions();
  void pollReactions();
  void refreshTransfer(FullMsgId id);
  // The rows on the screen, top to bottom, found by their place in the list: a chat scrolled far back has
  // thousands, too many to go through at every step of the scrolling.
  [[nodiscard]] std::vector<MessageWidget *> visibleRows() const;

  void scrollToBottom();
  void scrollToItem(not_null<const HistoryItem *> item, bool highlight);
  bool scrollToLoaded(MsgId id);
  void scrolled();
  void scheduleViewportUpdate();
  void updateViewport();
  void preloadVisibleEdges();
  [[nodiscard]] int listHeight() const;
  void setTopReserve(int reserve, double shift);
  void maintainTopReserve();
  [[nodiscard]] bool scrollIdle() const;
  [[nodiscard]] bool scrollIdleIgnoringShift() const;
  void markAsRead();
  // Telegram's sponsored messages in channels (required by the Telegram API Terms of Service, 3.3).
  void showSponsored();
  void reportSponsoredViews();

  void send();
  void edit(not_null<HistoryItem *> item);
  void cancelReplyOrEdit();
  void showReplyBar(const QString &title, const QString &text, const char *icon);
  // The messages waiting to be forwarded here (the chat's forward draft) are shown in the reply bar.
  [[nodiscard]] bool forwarding() const;
  void refreshForwardBar();
  void cancelForward();
  void showForwardMenu();
  void refreshSendButton();
  void chooseFiles();
  void chooseLocation();
  // The files from the chooser or a pasted picture, shown before sending (AttachDialog).
  void openAttach(const QStringList &paths, QImage image = QImage(), const QString &textOnCancel = QString());
  bool sendAttachments(Ui::PreparedList &list, const Ui::SendFilesWay &way, const QString &caption,
                       const Api::SendAction &action);
  void paste();
  void pasteText(const QString &text);
  void setupRecordGesture();
  void startRecording();
  void attachRecording();
  void lockRecording();
  void showRecordState();
  void dragRecording(double dx, double dy);
  void finishRecording(bool send);
  void confirmDelete(not_null<HistoryItem *> item);
  [[nodiscard]] QString composerText() const;
  void setComposerText(const QString &text);

  const not_null<Data::Thread *> _thread;
  const not_null<History *> _history;
  Data::ForumTopic *const _topic = nullptr;
  const not_null<MainWindow *> _window;

  GtkWidget *_root = nullptr;
  GtkWidget *_title = nullptr;
  GtkWidget *_liveBar = nullptr;
  GtkWidget *_liveText = nullptr;
  GtkWidget *_liveStop = nullptr;
  std::unique_ptr<ChatLiveLocations> _liveLocations;
  GtkWidget *_pinnedBar = nullptr;
  GtkWidget *_pinnedTitle = nullptr;
  GtkWidget *_pinnedText = nullptr;
  GtkWidget *_pinnedPreview = nullptr;
  GtkWidget *_pinnedList = nullptr;
  GtkWidget *_pinnedClose = nullptr;
  std::unique_ptr<HistoryView::PinnedTracker> _pinnedTracker;
  QString _pinnedTitleText;
  QString _pinnedTextText;
  QImage _pinnedImage;
  int _pinnedCount = 0;
  FullMsgId _pinnedClicked;              // Shown with a tap on the bar: the bar goes on to the one before it.
  std::optional<FullMsgId> _minPinned;   // The oldest pinned message, once asked while going through them.
  double _lastScrollValue = 0.;
  GtkWidget *_list = nullptr;
  GtkWidget *_scroll = nullptr;
  GtkAdjustment *_adjustment = nullptr;
  GtkWidget *_toBottom = nullptr;
  GtkWidget *_loading = nullptr;
  GtkWidget *_retry = nullptr;
  GtkWidget *_topSpacer = nullptr;
  GtkWidget *_lockHint = nullptr;

  GtkWidget *_composer = nullptr;
  GtkWidget *_composerStack = nullptr;
  GtkWidget *_replyBar = nullptr;
  GtkWidget *_replyIcon = nullptr;
  GtkWidget *_replyTitle = nullptr;
  GtkWidget *_replyText = nullptr;
  GtkWidget *_input = nullptr;
  GtkWidget *_sendButton = nullptr;
  GtkWidget *_sendStack = nullptr;
  GtkWidget *_mic = nullptr;
  GtkWidget *_recordTime = nullptr;
  GtkWidget *_recordDot = nullptr;
  GtkWidget *_recordHint = nullptr;
  GtkWidget *_recordCancel = nullptr;
  GtkWidget *_recordSend = nullptr;
  GtkWidget *_recordMicIndicator = nullptr;
  bool _recording = false;
  bool _recordHolding = false;  // The finger is on the microphone button.
  bool _recordLocked = false;   // Slid up: recording goes on without holding.
  crl::time _recordStarted = 0;
  rpl::lifetime _recordLifetime;
  GtkWidget *_joinButton = nullptr;

  // A row is a message, or an album (the messages of a group): every message maps to its row.
  std::unordered_map<MessageWidget *, std::unique_ptr<MessageWidget>> _rows;
  std::unordered_map<const HistoryItem *, MessageWidget *> _itemRows;
  std::set<MessageWidget *> _waitingMedia;
  std::set<DocumentData *> _openWhenLoaded;
  // Sponsored messages are sorted after the regular message they follow in the history.
  std::unordered_map<const HistoryItem *, MsgId> _sponsoredAfter;
  std::set<const HistoryItem *> _sponsoredViewed;
  bool _sponsoredMediaLoading = false;

  MsgId _windowMin = 0;  // The oldest message with a row; 0: from the oldest loaded.
  MsgId _windowMax = 0;  // The newest one; 0: up to the newest loaded, with the messages being sent.
  int _olderRequest = 0;
  int _newerRequest = 0;
  // The part of a topic shown: a slice of its messages around a position, growing as it scrolls.
  Data::MessagePosition _topicAround;
  int _topicLimitBefore = 0;
  int _topicLimitAfter = 0;
  bool _topicOlderWaiting = false;
  bool _topicNewerWaiting = false;
  std::vector<FullMsgId> _topicIds;
  std::optional<int> _topicSkippedBefore;
  std::optional<int> _topicSkippedAfter;
  rpl::lifetime _topicViewer;
  bool _initialLoaded = false;
  bool _stickToBottom = true;
  bool _adjustingScroll = false;
  bool _applyingPage = false;
  bool _loadFailed = false;
  bool _emptyReloadTried = false;
  int _loadEpoch = 0;
  guint _viewportSource = 0;
  FullMsgId _scrollTarget;
  bool _scrollTargetHighlight = false;
  std::optional<double> _scrollTargetY;  // Where the target goes on the screen, by default a third from the top.
  int _topReserve = 0;        // Empty room above the messages, filled by older pages (setTopReserve).
  double _pendingShift = 0;   // Applied to the scroll position when the content height changes.
  double _shiftBase = 0;
  bool _touchActive = false;
  MsgId _pendingShowAt = 0;
  MsgId _unreadAfter = 0;  // The "Unread messages" bar goes before the first message after this one.
  FullMsgId _replyTo;
  FullMsgId _editing;
  FullMsgId _loadedReplyTo;
  FullMsgId _beforeEditReplyTo;
  QString _loadedDraft;
  QString _beforeEditText;
  bool _editPending = false;
  bool _forwardShown = false;
  bool _settingComposerText = false;
  int _editGeneration = 0;
  bool _syncScheduled = false;
  base::weak_ptr<AttachDialog> _attach;
  std::unique_ptr<ClipboardReader> _pasteReader;
  SignalConnections _signals;
  base::Timer _readTimer;
  base::Timer _statusTimer;
  base::Timer _reserveTimer;
  rpl::lifetime _lifetime;
};

}  // namespace Contrail
