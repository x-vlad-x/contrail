#pragma once

#include "app.hpp"
#include "audio_route.hpp"
#include "call_lockscreen.hpp"
#include "call_notification.hpp"
#include "call_shell.hpp"
#include "proximity.hpp"
#include "video_frame.hpp"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include <rpl/lifetime.h>

class UserData;
namespace Calls { class Call; }
namespace Webrtc { class VideoTrack; }

namespace Contrail {

// A call belongs to the application, so navigating away from a chat never ends it.
class CallView final : public base::has_weak_ptr {
public:
  explicit CallView(not_null<MainWindow *> window);
  ~CallView();
  void start(not_null<UserData *> user, bool video = false);
  void present();
  // The main window was built again (it is released while closed).
  void setParentWindow(GtkWindow *parent);
  // CONTRAIL_DEBUG only; does not call anyone.
  void preview(bool incoming, bool video = false, const QString &name = u"Contrail"_q);
  void debugLocalCall(const QString &action);

private:
  void setCall(Calls::Call *call);
  void refresh();
  void showFrame(not_null<Webrtc::VideoTrack *> track, GtkWidget *picture);
  void stopVideo();
  void switchCamera();
  void choosePhoneCamera();
  void refreshDevices();
  void updateAudioRoute();
  void chooseAudioOutput();
  void updateProximity();
  void setProximityShield(bool near);
  void hangup();
  void updateNotification();
  void updateLockScreen();
  void toggleSpeaker();

  const not_null<MainWindow *> _parent;
  GtkWidget *_window = nullptr;
  GtkWidget *_avatarBox = nullptr;
  GtkWidget *_name = nullptr;
  GtkWidget *_status = nullptr;
  GtkWidget *_fingerprint = nullptr;
  GtkWidget *_remoteMuted = nullptr;
  GtkWidget *_video = nullptr;
  GtkWidget *_localVideo = nullptr;
  GtkWidget *_accept = nullptr;
  GtkWidget *_hangup = nullptr;
  GtkWidget *_mute = nullptr;
  GtkWidget *_camera = nullptr;
  GtkWidget *_switchCamera = nullptr;
  GtkWidget *_speaker = nullptr;
  GtkWidget *_localFrame = nullptr;
  GtkWidget *_shield = nullptr;
  base::weak_ptr<Calls::Call> _call;
  bool _ending = false;
  bool _remoteHasFrame = false;
  QSize _localSourceSize;
  QSize _remoteSourceSize;
  QString _playbackOutput;
  AudioRoute _audio;
  Proximity _proximity;
  CallNotification _notification;
  Ringer _ringer;
  LockScreenCall _lockScreen;
  ScreenLock _screenLock;
  bool _audioStarted = false;
  IdleInhibitor _idle;
  bool _shieldFullscreen = false;
  VideoFrameConverter _incomingConverter;
  VideoFrameConverter _outgoingConverter;
  base::Timer _timer;
  SignalConnections _signals;
  rpl::lifetime _callLifetime;
  rpl::lifetime _videoLifetime;
  rpl::lifetime _lifetime;
};

} // namespace Contrail
