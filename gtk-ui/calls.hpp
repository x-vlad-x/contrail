#pragma once

#include "app.hpp"
#include "audio_route.hpp"
#include "call_backend.hpp"
#include "call_lockscreen.hpp"
#include "call_notification.hpp"
#include "call_shell.hpp"
#include "proximity.hpp"
#include "td/store.hpp"

namespace Contrail {

class CallView final {
 public:
  CallView(MainWindow &window, Td::Store &store);
  ~CallView();
  void start(Td::UserId user, bool video = false);
  void present();
  void debug(const std::string &action, const std::string &argument);
  [[nodiscard]] bool active() const { return _backend.active(); }
 private:
  void refresh();
  void showFrame(Calls::Frame frame, bool local);
  void stopVideo();
  void switchCamera();
  void loadCameras();
  void updateAudioRoute();
  void chooseAudioOutput();
  void toggleSpeaker();
  void updateProximity();
  void setProximityShield(bool near);
  void updateLockScreen();
  void rating();
  [[nodiscard]] std::string name() const;
  [[nodiscard]] std::string status() const;

  MainWindow &_parent;
  Td::Store &_store;
  CallBackend _backend;
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
  AudioRoute _audio;
  Proximity _proximity;
  CallNotification _notification;
  Ringer _ringer;
  LockScreenCall _lockScreen;
  ScreenLock _screenLock;
  IdleInhibitor _idle;
  SignalConnections _signals;
  Td::Lifetime _lifetime;
  guint _timer = 0;
  int _shownCall = 0;
  int _ratedCall = 0;
  bool _audioStarted = false;
  bool _shieldFullscreen = false;
  bool _debugLocked = false;
  int _remoteWidth = 0;
  int _localWidth = 0;
  std::string _output;
  std::string _shownError;
  std::vector<Calls::Camera> _cameras;
  bool _devicesLoaded = false;
};

} // namespace Contrail
