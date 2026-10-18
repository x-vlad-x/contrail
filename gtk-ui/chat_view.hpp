#pragma once

#include "app.hpp"

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_messages.h"

#include <rpl/lifetime.h>

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

namespace Contrail {

class MainWindow;
class MessageWidget;

// An open chat: the messages (loaded page by page through the core, like Telegram Desktop's
// HistoryWidget does) and the message field. A topic of a group with topics is a chat of its own:
// its messages come from the core's list of the topic (Data::RepliesList), as in RepliesWidget.
class ChatView final : public base::has_weak_ptr {
public:
  ChatView(not_null<Data::Thread *> thread, not_null<MainWindow *> window, MsgId showAt);
  ~ChatView();

  [[nodiscard]] GtkWidget *widget() const { return _root; }
  [[nodiscard]] not_null<History *> history() const { return _history; }
  [[nodiscard]] not_null<Data::Thread *> thread() const { return _thread; }

  void showAt(MsgId id);
  bool handleEscape();  // Cancels a reply or an edit; false if there was nothing to cancel.

  // Used by the messages.
  void openPeer(not_null<PeerData *> peer);
  void openPhoto(not_null<PhotoData *> photo, not_null<HistoryItem *> item);
  void openDocument(not_null<DocumentData *> document, not_null<HistoryItem *> item);
  void showMessageMenu(not_null<HistoryItem *> item, GtkWidget *widget, double x, double y);
  void replyTo(not_null<HistoryItem *> item);
  void sendText(const QString &text);  // Bot commands from links.
  void debugScroll(bool top);
  void debugCheckDraft();
  void debugHistory(const QString &action);

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
  [[nodiscard]] std::vector<not_null<HistoryItem *>> items() const;
  void reload(MsgId showAt);
  void sync();
  void addRow(not_null<HistoryItem *> item);
  void removeRow(const HistoryItem *item);
  MessageWidget *rowFor(const HistoryItem *item) const;
  void refreshItem(const HistoryItem *item);

  void scrollToBottom();
  void scrollToRow(MessageWidget *row, bool highlight);
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
  void chooseFiles();
  void sendFiles(const QStringList &paths);
  void setupRecordGesture();
  void startRecording();
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

  std::unordered_map<const HistoryItem *, std::unique_ptr<MessageWidget>> _rows;
  std::set<MessageWidget *> _waitingMedia;
  std::set<DocumentData *> _openWhenLoaded;
  // Sponsored messages are sorted after the regular message they follow in the history.
  std::unordered_map<const HistoryItem *, MsgId> _sponsoredAfter;
  std::set<const HistoryItem *> _sponsoredViewed;
  bool _sponsoredMediaLoading = false;

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
  bool _settingComposerText = false;
  int _editGeneration = 0;
  bool _syncScheduled = false;
  SignalConnections _signals;
  base::Timer _readTimer;
  base::Timer _statusTimer;
  base::Timer _reserveTimer;
  rpl::lifetime _lifetime;
};

}  // namespace Contrail
