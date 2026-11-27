// New messages as the system's notifications (org.freedesktop.Notifications), as Telegram Desktop sent them for 0.3.1.
#pragma once

#include "td/store.hpp"

#include <gio/gio.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Contrail {

class MainWindow;

// TDLib decides what to notify about: muted chats and their exceptions, mentions, messages read elsewhere taking
// their notifications away. This shows its notification groups, one notification for each chat, with the newest
// messages in it: a tap opens the chat at the newest one, "Mark as read" reads it, and where the notification server
// can take text, an answer is sent from the notification itself.
class Notifier final {
 public:
  Notifier(Td::Store &store, MainWindow &window);
  ~Notifier();
  Notifier(const Notifier &) = delete;
  Notifier &operator=(const Notifier &) = delete;

  // The settings changed: turned off, the notifications shown go.
  void settingsChanged();

  // The process starts again in the background: the notifications on the screen stay there, and this tells the next
  // one which they are ("group:id,..."). Nothing is shown any more. TakeOver() gives the list to the next Notifier.
  [[nodiscard]] std::string handOver();
  static void TakeOver(const std::string &list);
  // A notification waits for the server's answer: it would be left without an owner.
  [[nodiscard]] bool busy() const;

  // "notify: group=1 chat=5 shown=12 count=2 ..." into the log.
  void debugState() const;
  // As the server would report a tap on notification of the chat, or a reply typed into it.
  void debugAction(Td::ChatId chat, const std::string &action, const std::string &text);

 private:
  struct Note {
    std::int32_t id = 0;
    std::int64_t message = 0;
    std::int32_t topic = 0;
    std::string sender;  // In groups; empty in private chats and channels.
    std::string text;
    bool preview = true;  // TDLib's setting of the chat: false shows only who wrote.
    bool silent = false;
  };
  struct Group {
    std::int32_t id = 0;
    Td::ChatId chat = 0;
    std::int64_t sound = 0;  // 0: TDLib says silently.
    std::vector<Note> notes;  // Oldest first.
    std::uint32_t shown = 0;  // The server's notification, 0 if none.
    bool pending = false;  // A Notify call waits for its answer.
    bool again = false;  // Changed while it waited: shown once more after the answer.
    bool againSound = false;
    bool adopted = false;  // From before this start: not shown until something new comes.
  };

  void listen();  // The server's signals and what it can do.
  void updated(Td::api::Update &update);
  void groupUpdated(Td::api::updateNotificationGroup &update);
  void noteUpdated(Td::api::updateNotification &update);
  void adopt(Td::api::updateActiveNotifications &update);
  [[nodiscard]] Note makeNote(const Td::api::notification &notification) const;
  [[nodiscard]] bool wanted(const Group &group) const;
  void show(Group &group, bool sound);
  void shown(std::int32_t groupId, std::uint32_t id);
  void close(Group &group);
  void closeAll();
  void signal(const char *name, GVariant *parameters);
  void activated(std::uint32_t id, const std::string &action);
  void open(const Group &group);
  void markRead(const Group &group);
  void reply(const Group &group, const std::string &text);
  [[nodiscard]] Group *byShown(std::uint32_t id);
  // A notification of the process before this one (TakeOver), by its group's id.
  struct Handed {
    std::uint32_t shown = 0;
    Td::ChatId chat = 0;
    std::int32_t topic = 0;
    std::int64_t message = 0;  // The newest.
  };
  static std::map<std::int32_t, Handed> &HandedOver();
  void takeHandedOver(Group &group);
  [[nodiscard]] std::optional<Group> handedGroup(std::uint32_t id) const;

  Td::Store &_store;
  MainWindow &_window;
  Td::Lifetime _lifetime;
  GDBusConnection *_bus = nullptr;
  GCancellable *_cancellable = nullptr;
  guint _subscription = 0;
  std::map<std::int32_t, Group> _groups;
  std::string _activationToken;  // For the window to come to the front from a tap, on Wayland.
  bool _canReply = false;
  bool _markup = false;
};

}  // namespace Contrail
