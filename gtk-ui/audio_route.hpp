#pragma once

#include <QtCore/QProcess>
#include <functional>

namespace Contrail {

// FuriOS exposes speaker and earpiece as ports of one PulseAudio sink, not two WebRTC devices.
class AudioRoute final {
public:
  using Changed = std::function<void(bool available, bool speaker, const QString &device)>;
  explicit AudioRoute(Changed changed);
  ~AudioRoute();
  void begin();
  void toggle();
  void end();
  bool available() const { return _available; }
  bool speaker() const { return _port == _speakerPort; }
  bool selected() const { return !_lastSelectedPort.isEmpty(); }
  bool busy() const { return _process.state() != QProcess::NotRunning; }

private:
  enum class Operation { Query, Switch, Restore };
  void query();
  void setPort(const QString &port, Operation operation);
  void finished(int exitCode);
  void restore();
  void notify();

  QProcess _process;
  Changed _changed;
  Operation _operation = Operation::Query;
  QString _sink;
  QString _device;
  QString _port;
  QString _speakerPort;
  QString _earpiecePort;
  QString _originalPort;
  QString _requestedPort;
  QString _lastSelectedPort;
  bool _available = false;
  bool _active = false;
  bool _restorePending = false;
  bool _beginPending = false;
};

} // namespace Contrail
