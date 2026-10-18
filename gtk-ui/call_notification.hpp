#pragma once

#include <gio/gio.h>
#include <QtCore/QString>
#include <QtGui/QImage>
#include <functional>

namespace Contrail {

// A call in the shell's notifications, where a phone's own dialer puts it: an incoming call with
// Accept and Decline, then the call in progress with End Call. Phosh shows these buttons on the lock
// screen too, since the desktop entry lists them (X-Phosh-Lockscreen-Actions=call-), so a call can be
// answered without unlocking the phone and opening the app.
class CallNotification final {
public:
  struct Callbacks {
    std::function<void()> accept;
    std::function<void()> decline;
    std::function<void()> hangup;
    std::function<void()> open;
  };
  explicit CallNotification(Callbacks callbacks);
  ~CallNotification();
  CallNotification(const CallNotification &) = delete;
  CallNotification &operator=(const CallNotification &) = delete;

  void showIncoming(const QString &name, const QString &status, const QImage &userpic);
  // Only takes the place of the incoming call's notification, a call started here gets none.
  void showOngoing(const QString &name, const QString &status);
  void close();

private:
  enum class State { None, Incoming, Ongoing };
  void send(const QString &name, const QString &status);
  void sent(guint32 id);
  void invoked(guint32 id, const char *action);
  void closed(guint32 id);

  Callbacks _callbacks;
  GDBusConnection *_bus = nullptr;
  GCancellable *_cancellable = nullptr;
  guint _actionSignal = 0;
  guint _closedSignal = 0;
  State _state = State::None;
  guint32 _id = 0;
  // One Notify call at a time: the next one replaces the notification by the id it returns.
  bool _inFlight = false;
  bool _resend = false;
  bool _dismissed = false;
  QString _name;
  QString _status;
  QString _shown;
  QImage _userpic;
};

} // namespace Contrail
