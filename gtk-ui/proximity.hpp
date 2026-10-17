#pragma once

#include <gio/gio.h>
#include <functional>

namespace Contrail {

// The screen goes dark while the phone is held to the ear during a call. Phosh does this itself
// only for calls made through GNOME Calls, so the app claims the proximity sensor (iio-sensor-proxy
// or a compatible service) and turns the display off through the shell's DisplayConfig.
class Proximity final {
public:
  using Changed = std::function<void(bool near)>;
  explicit Proximity(Changed changed);
  ~Proximity();
  Proximity(const Proximity &) = delete;
  Proximity &operator=(const Proximity &) = delete;

  void setEnabled(bool enabled);
  [[nodiscard]] bool near() const { return _near; }

private:
  void connected(GDBusProxy *sensor);
  void claim();
  void release();
  void claimed(bool success);
  void propertiesChanged(GVariant *changed);
  void setNear(bool near);
  void blank(bool off);

  Changed _changed;
  GCancellable *_cancellable = nullptr;
  GDBusProxy *_sensor = nullptr;
  bool _connecting = false;
  bool _enabled = false;
  bool _claiming = false;
  bool _claimed = false;
  bool _near = false;
  bool _blanked = false;
};

} // namespace Contrail
