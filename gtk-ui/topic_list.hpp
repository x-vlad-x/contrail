#pragma once

#include "app.hpp"
#include "td/store.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Contrail {

class MainWindow;

// The topics of a group with topics, a page of the sidebar over the chat list. A topic opens as a chat of its own.
// A group has a few topics, rarely more than a hundred: every row exists, in a list box.
class TopicList final {
 public:
  TopicList(Td::Store &store, Td::ChatId chat, MainWindow &window);
  ~TopicList();

  [[nodiscard]] AdwNavigationPage *page() const { return _page; }
  [[nodiscard]] Td::ChatId chat() const { return _chat; }
  void setCurrent(std::int32_t topic);
  void openRow(int index);

  void debugMenu(int index);
  void debugAction(int index, const std::string &action);
  void debugState() const;

 private:
  struct Row {
    std::int32_t id = 0;
    GtkWidget *row = nullptr;
    GtkWidget *avatar = nullptr;
    GtkWidget *name = nullptr;
    GtkWidget *muted = nullptr;
    GtkWidget *closed = nullptr;
    GtkWidget *status = nullptr;
    GtkWidget *date = nullptr;
    GtkWidget *preview = nullptr;
    GtkWidget *pinned = nullptr;
    GtkWidget *badge = nullptr;
    std::string previewMarkup;
  };

  void buildPage();
  Row &addRow(std::int32_t id);
  void updateRow(Row &row);
  void topicChanged(std::int32_t id);
  void refresh();
  void chatChanged();
  void loadMoreIfNear();
  [[nodiscard]] std::int32_t topicAt(int index) const;
  void showContextMenu(std::int32_t id, GtkWidget *parent, double x, double y);
  void rowAction(std::int32_t id, const std::string &action);
  [[nodiscard]] static int compareRows(GtkListBoxRow *a, GtkListBoxRow *b, gpointer data);
  [[nodiscard]] static gboolean filterRow(GtkListBoxRow *row, gpointer data);

  Td::Store &_store;
  const Td::ChatId _chat;
  MainWindow &_window;
  std::shared_ptr<void> _hold;  // Keeps the store's topics of this group.
  SignalConnections _connections;

  AdwNavigationPage *_page = nullptr;
  GtkWidget *_title = nullptr;
  GtkWidget *_list = nullptr;
  GtkWidget *_scroll = nullptr;
  GtkWidget *_stack = nullptr;
  GtkWidget *_searchEntry = nullptr;

  std::unordered_map<std::int32_t, std::unique_ptr<Row>> _rows;
  std::unordered_map<std::int32_t, int> _position;  // In the store's order.
  std::vector<std::string> _words;
  std::int32_t _current = 0;
  guint _closeSource = 0;
  guint _loadSource = 0;

  Td::Lifetime _lifetime;
};

}  // namespace Contrail
