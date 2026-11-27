#pragma once

#include "app.hpp"
#include "td/store.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Contrail {

class MainWindow;

// The chat list (or the archive) in the sidebar, kept in sync with the store: the same order, pinned chats, unread
// badges and previews. Only the rows near the screen exist: an account can have thousands of chats.
class ChatList final {
 public:
  ChatList(Td::Store &store, Td::ListKind kind, MainWindow &window);
  ~ChatList();

  [[nodiscard]] AdwNavigationPage *page() const { return _page; }
  void setCurrent(Td::ChatId chat);
  void startSearch(const std::string &query = {});
  void openRow(int index);

  void debugMenu(int index);
  void debugAction(int index, const std::string &action);
  // "Delete chat" / "Leave group" with the question, as the row's menu asks it (from the chat's card).
  void askToDelete(Td::ChatId id) { confirmDelete(id); }
  void debugScroll(const std::string &where);
  void debugState();

 private:
  // Global search results go in the same list: a list view has to be the scrolled window's only child, or it would
  // keep a widget for every chat.
  enum class RowKind { Chat, Archive, Header, Found };
  struct Entry {
    RowKind kind = RowKind::Chat;
    Td::ChatId id = 0;
    bool operator==(const Entry &) const = default;
  };
  // The widgets of one list item. They show one entry at a time and get another when the list scrolls.
  struct Row {
    Entry entry;
    GtkWidget *root = nullptr;
    GtkWidget *box = nullptr;
    GtkWidget *header = nullptr;
    GtkWidget *avatar = nullptr;
    GtkWidget *name = nullptr;
    GtkWidget *muted = nullptr;
    GtkWidget *status = nullptr;
    GtkWidget *date = nullptr;
    GtkWidget *preview = nullptr;
    GtkWidget *pinned = nullptr;
    GtkWidget *badge = nullptr;
    std::string previewMarkup;
  };

  void buildPage();
  GtkWidget *buildMenu();
  void setupRow(GtkListItem *item);
  void bindRow(GtkListItem *item);
  void unbindRow(GtkListItem *item);
  void updateRow(Row &row);
  void updateChatRow(Row &row);
  void updateArchiveRow(Row &row);
  void updateFoundRow(Row &row);
  void setPreview(Row &row, const std::string &markup);
  void setBadge(Row &row, bool visible, const std::string &text, bool muted);
  void chatChanged(Td::ChatId id);
  void userChanged(Td::UserId id);
  void connectionChanged();

  [[nodiscard]] std::string chatName(const Td::Chat &chat) const;
  [[nodiscard]] bool isSelf(const Td::Chat &chat) const;
  [[nodiscard]] std::vector<Entry> wantedEntries();
  void scheduleRefresh();
  void refresh();
  void applySelection();
  void activate(const Entry &entry);
  void loadMoreIfNear();

  void showContextMenu(Td::ChatId id, GtkWidget *parent, double x, double y);
  void rowAction(Td::ChatId id, const std::string &action);
  void send(Td::FunctionPtr function);
  void toggleArchived(Td::ChatId id);
  void confirmDelete(Td::ChatId id);
  void deleteChat(Td::ChatId id, bool revoke);
  void openSavedMessages();

  void applySearch();
  void searchGlobal();
  void addFound(const Td::api::chats &chats, bool own);

  Td::Store &_store;
  const Td::ListKind _kind;
  MainWindow &_window;
  SignalConnections _connections;

  AdwNavigationPage *_page = nullptr;
  GtkWidget *_title = nullptr;
  GtkWidget *_list = nullptr;
  GListStore *_model = nullptr;  // Owned by _selection.
  GtkSingleSelection *_selection = nullptr;  // Owned by the list view.
  GtkWidget *_scroll = nullptr;
  GtkWidget *_searchBar = nullptr;
  GtkWidget *_searchEntry = nullptr;
  GtkWidget *_stack = nullptr;

  std::vector<Entry> _shown;  // What the model holds, in its order.
  std::unordered_map<Td::ChatId, Row *> _bound;  // Rows on the screen, the only ones worth updating.
  Row *_archiveRow = nullptr;
  Td::ChatId _current = 0;

  std::string _query;
  std::vector<std::string> _words;
  std::vector<Td::ChatId> _found;
  guint _searchTimer = 0;
  guint _refreshSource = 0;
  guint _loadSource = 0;

  Td::Lifetime _lifetime;
  Td::Lifetime _searchLifetime;  // Reset by every new query: an old answer must not land in the new results.
};

}  // namespace Contrail
