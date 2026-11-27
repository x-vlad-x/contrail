// The proxy servers TDLib connects through: SOCKS5, HTTP and MTProto, in the settings.
#pragma once

#include "app.hpp"
#include "td/store.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Contrail {

class MainWindow;

// The list of proxies TDLib keeps, which one is used and whether it answers; adding, editing and removing them.
class ProxyView final {
 public:
  ProxyView(Td::Store &store, MainWindow &window);
  ~ProxyView();
  ProxyView(const ProxyView &) = delete;
  ProxyView &operator=(const ProxyView &) = delete;

  [[nodiscard]] AdwNavigationPage *page() const { return _page; }

  // "proxy: use=1 [0 socks5 good.example:1080 enabled status="online"] ..." into the log.
  void debugState() const;
  // "TYPE SERVER PORT [SECRET | USER PASSWORD]" typed into the dialog of a new one (or of proxy N with edit) and saved.
  void debugAdd(const std::string &arguments, int edit = -1);
  void debugUse(bool use);
  void debugEnable(int index);
  void debugRemove(int index);

 private:
  struct Entry {
    std::int32_t id = 0;
    Td::api::object_ptr<Td::api::proxy> proxy;
    bool enabled = false;
    std::int32_t lastUsed = 0;
    std::string ping;  // Its status: checking, available or not.
  };
  class Editor;

  void reload();
  void loaded(Td::api::addedProxies &proxies);
  void rebuild();
  void refreshStatus();
  void ping(std::int32_t id);
  void enable(std::int32_t id);
  void remove(std::int32_t id);
  void useChanged(bool use);
  void edit(const Entry *entry);
  void editorClosed();
  [[nodiscard]] std::string status(const Entry &entry) const;

  Td::Store &_store;
  MainWindow &_window;
  AdwNavigationPage *_page = nullptr;
  GtkWidget *_use = nullptr;
  GtkWidget *_group = nullptr;
  std::vector<GtkWidget *> _rows;
  std::vector<Entry> _entries;
  std::unique_ptr<Editor> _editor;
  guint _editorSource = 0;
  bool _syncing = false;  // The switch set from TDLib's list, not by a tap.
  Td::Lifetime _lifetime;
};

// A tg://proxy or t.me/proxy link: a question, then the proxy is added and used.
void ConfirmAddProxy(MainWindow &window, Td::api::object_ptr<Td::api::proxy> proxy);

// "SOCKS5", "HTTP", "MTProto".
[[nodiscard]] std::string ProxyTypeName(const Td::api::ProxyType *type);

}  // namespace Contrail
