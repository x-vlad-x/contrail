#pragma once

#include "app.hpp"

#include "base/timer.h"
#include "base/weak_ptr.h"

#include <rpl/lifetime.h>

#include <unordered_map>

class History;

namespace Main {
class Session;
} // namespace Main

namespace Data {
class Folder;
} // namespace Data

namespace Dialogs {
class Entry;
} // namespace Dialogs

namespace Contrail {

class MainWindow;

// The chat list (or the archive) in the sidebar, kept in sync with Telegram Desktop's own list
// (Data::Session::chatsList): the same order, pinned chats, unread badges and previews.
class ChatList final : public base::has_weak_ptr {
public:
  ChatList(not_null<Main::Session *> session, Data::Folder *folder, not_null<MainWindow *> window);
  ~ChatList();

  [[nodiscard]] AdwNavigationPage *page() const { return _page; }
  void setCurrent(History *history);
  void startSearch(const QString &query = QString());
  void openRow(int index);

private:
  struct Row {
    Dialogs::Entry *entry = nullptr;
    GtkWidget *row = nullptr;
    GtkWidget *avatar = nullptr;
    GtkWidget *name = nullptr;
    GtkWidget *muted = nullptr;
    GtkWidget *status = nullptr;
    GtkWidget *date = nullptr;
    GtkWidget *preview = nullptr;
    GtkWidget *pinned = nullptr;
    GtkWidget *badge = nullptr;
  };

  void buildPage();
  GtkWidget *buildMenu();
  void refreshRows();
  void scheduleRefresh();
  Row &createRow(not_null<Dialogs::Entry *> entry);
  void updateRow(Row &row);
  void updateEntry(Dialogs::Entry *entry);
  void activated(GtkListBoxRow *row);
  void showContextMenu(Row &row, double x, double y);
  void loadMore();
  void applySearch();
  void searchGlobal(const QString &query);

  const not_null<Main::Session *> _session;
  Data::Folder *const _folder;
  const not_null<MainWindow *> _window;

  AdwNavigationPage *_page = nullptr;
  GtkWidget *_list = nullptr;
  GtkWidget *_scroll = nullptr;
  GtkWidget *_searchBar = nullptr;
  GtkWidget *_searchEntry = nullptr;
  GtkWidget *_globalHeader = nullptr;
  GtkWidget *_globalList = nullptr;
  GtkWidget *_stack = nullptr;
  GtkWidget *_connecting = nullptr;
  QString _query;
  mtpRequestId _globalRequest = 0;

  std::unordered_map<Dialogs::Entry *, std::unique_ptr<Row>> _rows;
  History *_current = nullptr;
  bool _refreshScheduled = false;
  base::Timer _searchTimer;
  rpl::lifetime _lifetime;
};

}  // namespace Contrail
