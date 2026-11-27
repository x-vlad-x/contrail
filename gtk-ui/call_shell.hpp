#pragma once

#include <gio/gio.h>
#include <functional>
#include <memory>

namespace Contrail {

// Whether the screen is locked: the LockedHint the shell sets on the graphical session in logind (Phosh,
// GNOME Shell). The shell's org.gnome.ScreenSaver only tells whether the screen is blanked.
class ScreenLock final {
public:
  using Changed = std::function<void(bool locked)>;
  explicit ScreenLock(Changed changed);
  ~ScreenLock();
  ScreenLock(const ScreenLock &) = delete;
  ScreenLock &operator=(const ScreenLock &) = delete;

  [[nodiscard]] bool locked() const { return _locked; }

private:
  void watch(const char *session);
  void set(bool locked);

  Changed _changed;
  GDBusConnection *_bus = nullptr;
  GCancellable *_cancellable = nullptr;
  guint _signal = 0;
  bool _locked = false;
};

// The phone's own ringtone for an incoming call, as GNOME Calls rings: feedbackd's phone-incoming-call
// event, looped until the call is answered or over. It follows the phone's sound settings and profile
// (silent, vibration only) and plays on the loudspeaker, apart from where the call will be heard.
class Ringer final {
public:
  Ringer();
  ~Ringer();
  Ringer(const Ringer &) = delete;
  Ringer &operator=(const Ringer &) = delete;

  void start();
  void stop();
  [[nodiscard]] guint32 eventId() const { return _id; }

private:
  void started(guint32 id);

  GDBusConnection *_bus = nullptr;
  std::shared_ptr<char> _alive = std::make_shared<char>();
  guint32 _id = 0;
  bool _pending = false;
  bool _wanted = false;
};

// Whether feedbackd already owns its bus name: the call view uses the phone's ringtone then.
bool SystemRingtone();

} // namespace Contrail
