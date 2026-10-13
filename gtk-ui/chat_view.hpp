#pragma once

#include "app.hpp"

#include "base/timer.h"
#include "base/weak_ptr.h"

#include <rpl/lifetime.h>

#include <set>
#include <unordered_map>

class History;
class HistoryItem;
class PeerData;
class PhotoData;
class DocumentData;

namespace Contrail {

class MainWindow;
class MessageWidget;

// An open chat: the messages (loaded page by page through the core, like Telegram Desktop's
// HistoryWidget does) and the message field.
class ChatView final : public base::has_weak_ptr {
public:
  ChatView(not_null<History *> history, not_null<MainWindow *> window, MsgId showAt);
  ~ChatView();

  [[nodiscard]] GtkWidget *widget() const { return _root; }
  [[nodiscard]] not_null<History *> history() const { return _history; }

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
  void sync();
  void addRow(not_null<HistoryItem *> item);
  void removeRow(const HistoryItem *item);
  MessageWidget *rowFor(const HistoryItem *item) const;
  void refreshItem(const HistoryItem *item);

  void scrollToBottom();
  void scrollToRow(MessageWidget *row, bool highlight);
  bool scrollToLoaded(MsgId id);
  void scrolled();
  void markAsRead();

  void send();
  void edit(not_null<HistoryItem *> item);
  void cancelReplyOrEdit();
  void showReplyBar(const QString &title, const QString &text, const char *icon);
  void chooseFiles();
  void sendFiles(const QStringList &paths);
  void startRecording();
  void finishRecording(bool send);
  void confirmDelete(not_null<HistoryItem *> item);
  [[nodiscard]] QString composerText() const;
  void setComposerText(const QString &text);

  const not_null<History *> _history;
  const not_null<MainWindow *> _window;

  GtkWidget *_root = nullptr;
  GtkWidget *_title = nullptr;
  GtkWidget *_list = nullptr;
  GtkWidget *_scroll = nullptr;
  GtkAdjustment *_adjustment = nullptr;
  GtkWidget *_toBottom = nullptr;
  GtkWidget *_loading = nullptr;

  GtkWidget *_composer = nullptr;
  GtkWidget *_composerStack = nullptr;
  GtkWidget *_replyBar = nullptr;
  GtkWidget *_replyIcon = nullptr;
  GtkWidget *_replyTitle = nullptr;
  GtkWidget *_replyText = nullptr;
  GtkWidget *_input = nullptr;
  GtkWidget *_sendButton = nullptr;
  GtkWidget *_sendStack = nullptr;
  GtkWidget *_recordTime = nullptr;
  bool _recording = false;
  rpl::lifetime _recordLifetime;
  GtkWidget *_joinButton = nullptr;

  std::unordered_map<const HistoryItem *, std::unique_ptr<MessageWidget>> _rows;
  std::set<MessageWidget *> _waitingMedia;
  std::set<DocumentData *> _openWhenLoaded;

  int _olderRequest = 0;
  int _newerRequest = 0;
  bool _initialLoaded = false;
  bool _stickToBottom = true;
  double _anchorUpper = -1;   // Keeps the position while older messages are added on top.
  double _anchorValue = 0;
  MsgId _pendingShowAt = 0;
  MsgId _unreadAfter = 0;  // The "Unread messages" bar goes before the first message after this one.
  FullMsgId _replyTo;
  FullMsgId _editing;
  QString _loadedDraft;
  bool _syncScheduled = false;
  gulong _activeHandler = 0;
  base::Timer _readTimer;
  base::Timer _statusTimer;
  rpl::lifetime _lifetime;
};

}  // namespace Contrail
