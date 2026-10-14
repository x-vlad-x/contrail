#include "audio_route.hpp"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcessEnvironment>

namespace Contrail {

AudioRoute::AudioRoute(Changed changed) : _changed(std::move(changed)) {
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
  _process.setProcessEnvironment(environment);
  QObject::connect(&_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &_process,
      [this](int code, QProcess::ExitStatus) { finished(code); });
  QObject::connect(&_process, &QProcess::errorOccurred, &_process, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      _available = false;
      notify();
    }
  });
}

AudioRoute::~AudioRoute() {
  // Normally end() restores asynchronously as the call ends. Shutdown must finish that restore
  // before destroying the helper; otherwise the next app inherits our speaker selection.
  _changed = [](bool, bool, const QString &) {};
  end();
  for (auto i = 0; i != 3 && busy(); ++i) _process.waitForFinished(1000);
  QObject::disconnect(&_process, nullptr, &_process, nullptr);
  if (busy()) { _process.kill(); _process.waitForFinished(500); }
}

void AudioRoute::notify() {
  _changed(_available && !busy(), speaker(), _device);
}

void AudioRoute::begin() {
  if (busy()) { _beginPending = true; return; }
  _active = true;
  if (busy()) return;
  _originalPort.clear();
  _lastSelectedPort.clear();
  _restorePending = false;
  query();
}

void AudioRoute::query() {
  _operation = Operation::Query;
  _process.start(QStringLiteral("pactl"), {QStringLiteral("--format=json"), QStringLiteral("list"), QStringLiteral("sinks")});
  notify();
}

void AudioRoute::setPort(const QString &port, Operation operation) {
  _requestedPort = port;
  _operation = operation;
  _process.start(QStringLiteral("pactl"), {QStringLiteral("set-sink-port"), _sink, port});
  notify();
}

void AudioRoute::toggle() {
  if (!_active || !_available || busy()) return;
  setPort(speaker() ? _earpiecePort : _speakerPort, Operation::Switch);
}

void AudioRoute::end() {
  _beginPending = false;
  _active = false;
  _restorePending = true;
  if (!busy()) restore();
}

void AudioRoute::restore() {
  _restorePending = false;
  if (_originalPort.isEmpty() || _lastSelectedPort.isEmpty()) return;
  // Refresh before restoring: avoid overwriting a route chosen elsewhere during the call.
  query();
}

void AudioRoute::finished(int exitCode) {
  if (_operation == Operation::Query) {
    _available = false;
    if (exitCode == 0) {
      const auto sinks = QJsonDocument::fromJson(_process.readAllStandardOutput()).array();
      for (const auto &value : sinks) {
        const auto sink = value.toObject();
        const auto name = sink.value(QStringLiteral("name")).toString();
        if (!_sink.isEmpty() && !_active && name != _sink) continue;
        QString speaker, earpiece;
        for (const auto &value : sink.value(QStringLiteral("ports")).toArray()) {
          const auto port = value.toObject();
          if (port.value(QStringLiteral("availability")).toString() == QStringLiteral("not available")) continue;
          const auto name = port.value(QStringLiteral("name")).toString();
          if (name.contains(QStringLiteral("speaker"), Qt::CaseInsensitive)) speaker = name;
          if (name.contains(QStringLiteral("earpiece"), Qt::CaseInsensitive)) earpiece = name;
        }
        if (speaker.isEmpty() || earpiece.isEmpty()) continue;
        _sink = name;
        _device = sink.value(QStringLiteral("description")).toString();
        _port = sink.value(QStringLiteral("active_port")).toString();
        _speakerPort = speaker;
        _earpiecePort = earpiece;
        _available = true;
        if (_active && _originalPort.isEmpty()) _originalPort = _port;
        break;
      }
    }
    if (!_active && !_originalPort.isEmpty() && !_lastSelectedPort.isEmpty()) {
      if (_available && _port == _lastSelectedPort && _port != _originalPort) {
        setPort(_originalPort, Operation::Restore);
        return;
      }
      _originalPort.clear();
      _lastSelectedPort.clear();
    }
  } else if (exitCode == 0) {
    _port = _requestedPort;
    if (_operation == Operation::Switch) _lastSelectedPort = _port;
    else {
      _originalPort.clear();
      _lastSelectedPort.clear();
    }
  }
  notify();
  if (_restorePending) restore();
  if (_beginPending && !busy()) {
    _beginPending = false;
    begin();
  }
}

} // namespace Contrail
