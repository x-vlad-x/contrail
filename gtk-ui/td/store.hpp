// What the interface knows about the account, kept up to date from TDLib's updates. One source of truth: views read
// it and subscribe to changes, they don't keep copies. Only the fields the interface shows are kept, not TDLib's
// objects: chats and users stay cheap on an account with thousands of them. Messages are not here: only an open chat
// holds them, near the screen, and TDLib's database has the rest.
#pragma once

#include "td/client.hpp"

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace Contrail::Td {

using ChatId = std::int64_t;
using UserId = std::int64_t;
using FileId = std::int32_t;

enum class ChatKind { Private, Group, Channel, Secret };
enum class ListKind { Main, Archive };

struct Position {
  std::int64_t order = 0;  // 0: not in this list.
  bool pinned = false;
};

struct Chat {
  ChatId id = 0;
  ChatKind kind = ChatKind::Private;
  std::int64_t peerId = 0;  // The user of a private chat, the group of a group or channel.
  std::string title;
  FileId photo = 0;         // Small picture.
  int accentColor = 0;
  Position main;
  Position archive;
  std::string preview;      // The last message as a line of text.
  std::int32_t lastDate = 0;
  std::int64_t lastMessageId = 0;
  bool lastOutgoing = false;
  int unread = 0;
  int unreadMentions = 0;
  bool markedUnread = false;
  bool muted = false;
  std::int64_t readInbox = 0;
  std::int64_t readOutbox = 0;
  std::string draft;
  bool forum = false;
};

struct User {
  UserId id = 0;
  std::string firstName;
  std::string lastName;
  std::string username;
  FileId photo = 0;
  int accentColor = 0;
  bool bot = false;
  bool deleted = false;
  bool premium = false;
  // Status: online until this time; 0 when offline, -1 for "recently" and the like.
  std::int32_t onlineUntil = 0;
  std::int32_t lastSeen = 0;
};

struct File {
  FileId id = 0;
  std::int64_t size = 0;
  std::int64_t downloaded = 0;
  std::string path;   // Set when the download is complete.
  bool downloading = false;
  bool complete = false;
};

class Store final {
 public:
  using ChatHandler = std::function<void(ChatId id)>;
  using ListHandler = std::function<void(ListKind list)>;
  using UserHandler = std::function<void(UserId id)>;
  using FileHandler = std::function<void(const File &file)>;
  // The text of a last message, from message_text.cpp; a test can replace it.
  using Preview = std::function<std::string(const api::message &message)>;

  Store(Client &client, Preview preview);

  [[nodiscard]] Client &client() { return _client; }
  [[nodiscard]] const Chat *chat(ChatId id) const;
  [[nodiscard]] const User *user(UserId id) const;
  [[nodiscard]] const File *file(FileId id) const;
  [[nodiscard]] UserId selfId() const { return _selfId; }
  // In Telegram's order: pinned first, then newest first.
  [[nodiscard]] const std::vector<ChatId> &list(ListKind kind);
  [[nodiscard]] bool listComplete(ListKind kind) const { return _lists[int(kind)].complete; }
  [[nodiscard]] int unreadChats(ListKind kind) const { return _lists[int(kind)].unreadChats; }
  [[nodiscard]] const api::ConnectionState *connection() const { return _connection.get(); }

  // Asks for the next chats of a list. Quietly does nothing while a request runs or when the list is complete.
  void loadMore(ListKind kind, int limit = 50);
  // Asks for a file; progress comes through onFile. Priority 1 to 32, higher goes first.
  void download(FileId id, int priority = 1);

  void onChat(ChatHandler handler, const Lifetime &lifetime);
  void onList(ListHandler handler, const Lifetime &lifetime);
  void onUser(UserHandler handler, const Lifetime &lifetime);
  void onFile(FileHandler handler, const Lifetime &lifetime);
  void onConnection(std::function<void()> handler, const Lifetime &lifetime);

 private:
  struct ListState {
    std::vector<ChatId> ids;
    bool dirty = false;
    bool loading = false;
    bool complete = false;
    int unreadChats = 0;
  };
  template <typename Handler>
  struct Subscriber {
    Handler handler;
    std::weak_ptr<void> token;
  };

  void updated(api::Update &update);
  void addChat(api::chat &chat);
  void addUser(api::user &user);
  void setPositions(Chat &chat, std::vector<api::object_ptr<api::chatPosition>> &positions);
  void setPosition(Chat &chat, api::chatPosition &position);
  void setLastMessage(Chat &chat, api::message *message);
  void addFile(const api::file &file);
  void chatChanged(ChatId id);
  void listChanged(ListKind kind);
  template <typename Handler, typename... Arguments>
  void notify(std::vector<Subscriber<Handler>> &subscribers, const Arguments &...arguments);

  Client &_client;
  Preview _preview;
  Lifetime _lifetime;
  std::unordered_map<ChatId, Chat> _chats;
  std::unordered_map<UserId, User> _users;
  std::unordered_map<FileId, File> _files;
  ListState _lists[2];
  UserId _selfId = 0;
  api::object_ptr<api::ConnectionState> _connection;
  std::vector<Subscriber<ChatHandler>> _chatHandlers;
  std::vector<Subscriber<ListHandler>> _listHandlers;
  std::vector<Subscriber<UserHandler>> _userHandlers;
  std::vector<Subscriber<FileHandler>> _fileHandlers;
  std::vector<Subscriber<std::function<void()>>> _connectionHandlers;
};

// "Name Surname" of a user, the title of a deleted account's stand-in when there is none.
[[nodiscard]] std::string DisplayName(const User &user);

}  // namespace Contrail::Td
