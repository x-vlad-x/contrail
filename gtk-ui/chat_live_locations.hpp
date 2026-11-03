#pragma once

#include "app.hpp"
#include "td/store.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Contrail {

class MainWindow;

// The live locations shared in a chat now, by everyone (this account's own too): what the chat's bar and the shared
// map show. TDLib lists the recent ones (searchChatRecentLocationMessages, as the phone apps ask); then they come
// as new messages and their edits, and end with their time.
class ChatLiveLocations final {
 public:
  ChatLiveLocations(Td::Store &store, Td::ChatId chat);
  ~ChatLiveLocations();

  // One message per sender (the newest), this account's own first, then by when they were updated.
  [[nodiscard]] const std::vector<const Td::api::message *> &active() const { return _active; }
  [[nodiscard]] const Td::api::message *mine() const;
  [[nodiscard]] bool isMine(const Td::api::message &message) const;
  void onChanged(std::function<void()> handler) { _changed = std::move(handler); }
  // Asks for the chat's live locations again: their new places too, which don't always come as updates (edits in a
  // large group). The shared map asks every half a minute.
  void request();

 private:
  void updated(Td::api::Update &update);
  void consider(const Td::api::message &message);
  void refresh();

  Td::Store &_store;
  const Td::ChatId _chat;
  std::vector<Td::api::object_ptr<Td::api::message>> _known;  // Every live location's message seen, active or not.
  std::vector<const Td::api::message *> _active;
  std::function<void()> _changed;
  guint _expireTimer = 0;
  Td::Lifetime _lifetime;
  Td::Lifetime _requestLifetime;
};

// "Alice, Bob and you" for the chat's live location bar.
[[nodiscard]] std::string LiveLocationNames(const Td::Store &store, const ChatLiveLocations &locations);

// The bar under a chat's header while somebody shares a live location there: who, and a tap opens the shared map.
class ChatLiveBar final {
 public:
  ChatLiveBar(MainWindow &window, Td::Store &store, Td::ChatId chat);
  ~ChatLiveBar();

  [[nodiscard]] GtkWidget *widget() const { return _revealer; }
  [[nodiscard]] std::string debugState() const;
  void debugTap();

 private:
  void refresh();

  MainWindow &_window;
  Td::Store &_store;
  const Td::ChatId _chat;
  std::unique_ptr<ChatLiveLocations> _locations;
  GtkWidget *_revealer = nullptr;
  GtkWidget *_title = nullptr;
  GtkWidget *_text = nullptr;
  GtkWidget *_stop = nullptr;
  Td::Lifetime _lifetime;
  SignalConnections _signals;
};

}  // namespace Contrail
