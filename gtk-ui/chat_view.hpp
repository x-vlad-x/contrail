#pragma once

#include "app.hpp"
#include "td/store.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace Contrail {

class MainWindow;
class MessageWidget;
struct MessageItem;
struct ReplyPreview;

// An open chat: its messages and the message field. Only the messages near the screen are held, each with its row:
// the ones that go are in TDLib's database and come back from there.
class ChatView final {
 public:
  ChatView(Td::Store &store, Td::ChatId chat, MainWindow &window, std::int64_t showAt = 0);
  ~ChatView();

  [[nodiscard]] GtkWidget *widget() const { return _root; }
  [[nodiscard]] Td::ChatId chat() const { return _chat; }
  [[nodiscard]] Td::Store &store() const { return _store; }
  void showAt(std::int64_t message);
  // Cancels a reply or an edit; false if there was nothing to cancel.
  bool handleEscape();

  // What the messages ask for.
  void openChat(Td::ChatId chat);
  void openPhoto(const MessageItem &item, Td::FileId file);
  // A tap on a file: opens it if it is here, stops its download if it runs, starts one otherwise.
  void openDocument(const MessageItem &item, Td::FileId file);
  [[nodiscard]] bool opensWhenLoaded(Td::FileId file) const;
  void showMessageMenu(const MessageItem &item, GtkWidget *widget, double x, double y);
  void replyTo(const MessageItem &item);
  [[nodiscard]] ReplyPreview replyPreview(const MessageItem &item) const;
  void sendText(const std::string &text);  // Bot commands from links.

  // tools/phone.sh td-cmd: what the list holds, and the actions of a finger.
  void debugState() const;
  void debugScroll(const std::string &where);
  void debugSend(const std::string &text);
  void debugReply();
  void debugEdit(const std::string &text);
  void debugDelete();

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

  Td::Store &_store;
  const Td::ChatId _chat;
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
  SignalConnections _signals;

  std::vector<std::unique_ptr<MessageItem>> _items;
  std::unordered_map<MessageWidget *, std::unique_ptr<MessageWidget>> _rows;
  std::unordered_map<const MessageItem *, MessageWidget *> _itemRows;
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

  std::int64_t _replyToId = 0;
  std::int64_t _editing = 0;
  bool _editPending = false;
  std::string _beforeEditText;
  std::int64_t _beforeEditReplyTo = 0;
  bool _settingComposerText = false;
  std::string _savedDraft;
  std::int64_t _savedDraftReply = 0;
  gint64 _lastTyping = 0;

  Timer _readTimer;
  Timer _statusTimer;
  Timer _reserveTimer;
  Timer _jumpTimer;
  Timer _draftTimer;
};

}  // namespace Contrail
