#pragma once

#include <gio/gio.h>
#include <functional>

namespace Contrail {

// The screen goes dark while the phone is held to the ear during a call. Phosh does this itself
// only for calls made through GNOME Calls, so the app claims the proximity sensor (iio-sensor-proxy
// or a compatible service) and tells when it is near. Like Phosh, the call view then covers the screen
// in black and takes the touches; the display isn't turned off, which the shell would take for the
// screen being blanked and lock the phone.
class Proximity final {
public:
  using Changed = std::function<void(bool near)>;
  explicit Proximity(Changed changed);
  ~Proximity();
  Proximity(const Proximity &) = delete;
  Proximity &operator=(const Proximity &) = delete;

  void setEnabled(bool enabled);
  [[nodiscard]] bool near() const { return _near; }
  void debugSetNear(bool near);

private:
  void connected(GDBusProxy *sensor);
  void claim();
  void release();
  void claimed(bool success);
  void propertiesChanged(GVariant *changed);
  void setNear(bool near);

  Changed _changed;
  GCancellable *_cancellable = nullptr;
  GDBusProxy *_sensor = nullptr;
  bool _connecting = false;
  bool _enabled = false;
  bool _claiming = false;
  bool _claimed = false;
  bool _near = false;
};

// Keeps the session from going idle during a call: idle blanks the screen and then locks the phone.
class IdleInhibitor final {
public:
  IdleInhibitor();
  ~IdleInhibitor();
  IdleInhibitor(const IdleInhibitor &) = delete;
  IdleInhibitor &operator=(const IdleInhibitor &) = delete;

  void set(bool inhibit);

private:
  void uninhibit();

  GCancellable *_cancellable = nullptr;
  guint _cookie = 0;
  bool _requested = false;
  bool _pending = false;
};

} // namespace Contrail
