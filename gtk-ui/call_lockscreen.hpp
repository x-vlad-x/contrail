#pragma once

#include "app.hpp"

class PeerData;

namespace Contrail {

// A call over the lock screen, as the phone's dialer shows it: the whole screen with the caller, Accept
// and Decline, then Mute, the loudspeaker and End Call. Phosh draws its lock screen as a layer shell
// surface; this one goes on the overlay layer above it. It shows only the call: nothing behind the lock
// screen can be reached from it.
class LockScreenCall final {
public:
  struct Callbacks {
    std::function<void()> accept;
    std::function<void()> hangup;
    std::function<void()> mute;
    std::function<void()> speaker;
  };
  explicit LockScreenCall(Callbacks callbacks);
  ~LockScreenCall();
  LockScreenCall(const LockScreenCall &) = delete;
  LockScreenCall &operator=(const LockScreenCall &) = delete;

  // Built with gtk4-layer-shell, and the compositor has the layer shell.
  [[nodiscard]] static bool Supported();

  // Without a peer (a preview), initials of the name.
  void show(PeerData *peer, const QString &name);
  void update(const QString &status, bool incoming, bool muted, bool speaker);
  // Black while the phone is held to the ear.
  void setShield(bool near);
  void hide();
  [[nodiscard]] bool shown() const { return _shown; }

private:
  void build();

  Callbacks _callbacks;
  GtkWidget *_window = nullptr;
  GtkWidget *_avatarBox = nullptr;
  GtkWidget *_name = nullptr;
  GtkWidget *_status = nullptr;
  GtkWidget *_incoming = nullptr;
  GtkWidget *_ongoing = nullptr;
  GtkWidget *_mute = nullptr;
  GtkWidget *_speaker = nullptr;
  GtkWidget *_shield = nullptr;
  SignalConnections _signals;
  bool _shown = false;
};

} // namespace Contrail
