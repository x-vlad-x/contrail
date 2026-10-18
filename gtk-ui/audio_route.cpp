#include "audio_route.hpp"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QSettings>
#include <algorithm>

namespace Contrail {
namespace {

bool PhonePort(const QString &port, const char *what) {
  return port.contains(QLatin1String(what), Qt::CaseInsensitive);
}

bool WiredPort(const QString &port) {
  return PhonePort(port, "wired") || PhonePort(port, "headset") || PhonePort(port, "headphone");
}

// PulseAudio's 100%. The earpiece is quiet: a call starts loud enough on it.
constexpr auto kNormalVolume = 65536;
constexpr auto kEarpieceVolume = kNormalVolume * 4 / 5;
const auto kVolumesGroup = QStringLiteral("call-volume");

} // namespace

AudioRoute::AudioRoute(Changed changed, QString volumesFile)
    : _changed(std::move(changed)), _volumesFile(std::move(volumesFile)) {
  if (!_volumesFile.isEmpty()) {
    QSettings settings(_volumesFile, QSettings::IniFormat);
    settings.beginGroup(kVolumesGroup);
    for (const auto &port : settings.childKeys()) {
      const auto volume = settings.value(port).toInt();
      if (volume > 0) _callVolumes.insert(port, volume);
    }
  }
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
  _process.setProcessEnvironment(environment);
  _monitor.setProcessEnvironment(environment);
  const auto complete = [this](int exitCode) {
    auto command = std::move(_running);
    _running = {};
    const auto output = exitCode >= 0 ? _process.readAllStandardOutput() : QByteArray();
    if (command.done) command.done(exitCode, output);
    next();
  };
  QObject::connect(&_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &_process,
      [=](int code, QProcess::ExitStatus status) { complete(status == QProcess::NormalExit ? code : -1); });
  QObject::connect(&_process, &QProcess::errorOccurred, &_process, [=](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) complete(-1);
  });
  // Headphones plugged in, a Bluetooth device connected, another app changing the route.
  QObject::connect(&_monitor, &QProcess::readyReadStandardOutput, &_monitor, [this] {
    const auto events = QString::fromUtf8(_monitor.readAllStandardOutput());
    if (_active && (events.contains(QLatin1String("' on sink #")) || events.contains(QLatin1String("' on server"))
        || events.contains(QLatin1String("' on card #")))) {
      query();
    }
  });
}

AudioRoute::~AudioRoute() {
  // Normally end() restores asynchronously as the call ends. Shutdown must finish that restore
  // before destroying the helper; otherwise the next app inherits our speaker selection.
  _changed = [] {};
  end();
  for (auto i = 0; i != 5 && busy(); ++i) _process.waitForFinished(1000);
  QObject::disconnect(&_process, nullptr, &_process, nullptr);
  QObject::disconnect(&_monitor, nullptr, &_monitor, nullptr);
  if (_process.state() != QProcess::NotRunning) { _process.kill(); _process.waitForFinished(500); }
  if (_monitor.state() != QProcess::NotRunning) _monitor.waitForFinished(500);
}

void AudioRoute::notify() {
  _changed();
}

void AudioRoute::run(QStringList arguments, std::function<void(int, const QByteArray &)> done) {
  _queue.push_back({std::move(arguments), std::move(done)});
  next();
}

void AudioRoute::next() {
  if (_process.state() != QProcess::NotRunning || _running.done || _queue.empty()) return;
  _running = std::move(_queue.front());
  _queue.pop_front();
  _process.start(QStringLiteral("pactl"), _running.arguments);
}

void AudioRoute::begin(bool video) {
  // A restore left over from the previous call keeps its original port: this call continues from it.
  _active = true;
  _video = video;
  _currentId.clear();
  _knownDefaultSink.clear();
  _outputs.clear();
  startMonitor();
  query();
  notify();
}

void AudioRoute::startMonitor() {
  if (_monitor.state() == QProcess::NotRunning) _monitor.start(QStringLiteral("pactl"), {QStringLiteral("subscribe")});
}

void AudioRoute::stopMonitor() {
  if (_monitor.state() != QProcess::NotRunning) _monitor.kill();
}

void AudioRoute::query() {
  if (_queryQueued) return;
  _queryQueued = true;
  run({QStringLiteral("--format=json"), QStringLiteral("info")}, [this](int code, const QByteArray &output) {
    _defaultSink = code == 0
        ? QJsonDocument::fromJson(output).object().value(QStringLiteral("default_sink_name")).toString()
        : QString();
  });
  run({QStringLiteral("--format=json"), QStringLiteral("list"), QStringLiteral("sinks")},
      [this](int code, const QByteArray &output) {
    _queryQueued = false;
    if (code == 0) parseSinks(output);
    else _sinks.clear();
    queried();
  });
}

void AudioRoute::parseSinks(const QByteArray &json) {
  _sinks.clear();
  QStringList devices;
  for (const auto &value : QJsonDocument::fromJson(json).array()) {
    const auto object = value.toObject();
    const auto properties = object.value(QStringLiteral("properties")).toObject();
    auto sink = Sink{
      .name = object.value(QStringLiteral("name")).toString(),
      .description = object.value(QStringLiteral("description")).toString(),
      .activePort = object.value(QStringLiteral("active_port")).toString(),
      .bus = properties.value(QStringLiteral("device.bus")).toString(),
      .formFactor = properties.value(QStringLiteral("device.form_factor")).toString(),
    };
    if (sink.description.isEmpty()) sink.description = sink.name;
    for (const auto &channel : object.value(QStringLiteral("volume")).toObject()) {
      sink.volume = std::max(sink.volume, channel.toObject().value(QStringLiteral("value")).toInt(-1));
    }
    // OpenAL Soft names devices by their descriptions in this order, numbering repeated ones.
    sink.device = sink.description;
    for (auto count = 1; devices.contains(sink.device);) {
      sink.device = sink.description + QStringLiteral(" #") + QString::number(++count);
    }
    devices.push_back(sink.device);
    auto anyAvailable = false;
    for (const auto &value : object.value(QStringLiteral("ports")).toArray()) {
      const auto port = value.toObject();
      const auto available = port.value(QStringLiteral("availability")).toString() != QStringLiteral("not available");
      sink.ports.push_back({port.value(QStringLiteral("name")).toString(), available});
      anyAvailable = anyAvailable || available;
    }
    // FuriOS's placeholder for Bluetooth calls, PipeWire's dummy output, unplugged HDMI.
    sink.skip = object.value(QStringLiteral("driver")).toString().contains(QLatin1String("null-sink"))
        || sink.name.startsWith(QLatin1String("auto_null")) || sink.name.startsWith(QLatin1String("sink.fake"))
        || (!sink.ports.empty() && !anyAvailable);
    _sinks.push_back(std::move(sink));
  }
}

AudioRoute::Sink *AudioRoute::sink(const QString &name) {
  const auto i = std::find_if(_sinks.begin(), _sinks.end(), [&](const Sink &sink) { return sink.name == name; });
  return i == _sinks.end() ? nullptr : &*i;
}

const AudioRoute::Output *AudioRoute::find(const QString &id) const {
  const auto i = std::find_if(_outputs.begin(), _outputs.end(), [&](const Output &output) { return output.id == id; });
  return i == _outputs.end() ? nullptr : &*i;
}

const AudioRoute::Output *AudioRoute::current() const {
  return find(_currentId);
}

const AudioRoute::Output *AudioRoute::preferred() const {
  if (_outputs.empty()) return nullptr;
  const auto defaultSink = std::any_of(_outputs.begin(), _outputs.end(),
      [&](const Output &output) { return output.sink == _defaultSink; });
  const auto &sink = defaultSink ? _defaultSink : _outputs.front().sink;
  const Output *earpiece = nullptr, *speaker = nullptr, *wired = nullptr, *whole = nullptr;
  for (const auto &output : _outputs) {
    if (output.sink != sink) continue;
    if (output.port.isEmpty()) whole = &output;
    else if (output.kind == Kind::Earpiece) earpiece = &output;
    else if (output.kind == Kind::Speaker) speaker = &output;
    else if (output.kind == Kind::Headphones) wired = &output;
  }
  if (whole) return whole;
  if (wired) return wired;
  const auto builtIn = _video ? speaker : earpiece;
  return builtIn ? builtIn : &_outputs.front();
}

void AudioRoute::queried() {
  QStringList previous;
  for (const auto &output : _outputs) previous.push_back(output.id);
  _outputs.clear();
  for (const auto &sink : _sinks) {
    if (sink.skip) continue;
    QString earpiece, speaker, wired;
    for (const auto &port : sink.ports) {
      if (!port.available) continue;
      if (PhonePort(port.name, "earpiece")) earpiece = port.name;
      else if (PhonePort(port.name, "speaker")) speaker = port.name;
      else if (WiredPort(port.name) && wired.isEmpty()) wired = port.name;
    }
    if (!earpiece.isEmpty() && !speaker.isEmpty()) {
      const auto add = [&](const QString &port, Kind kind) {
        _outputs.push_back({sink.name + u'/' + port, sink.name, port, QString(), sink.device, kind});
      };
      add(earpiece, Kind::Earpiece);
      add(speaker, Kind::Speaker);
      if (!wired.isEmpty()) add(wired, Kind::Headphones);
      continue;
    }
    const auto kind = (sink.bus == QStringLiteral("bluetooth") || sink.name.startsWith(QLatin1String("bluez")))
        ? Kind::Bluetooth
        : (sink.formFactor == QStringLiteral("headphone") || sink.formFactor == QStringLiteral("headset")
            || WiredPort(sink.activePort))
        ? Kind::Headphones
        : Kind::Other;
    _outputs.push_back({sink.name, sink.name, QString(), sink.description, sink.device, kind});
  }

  if (!_active) {
    // After the call: put the phone's port and volume back, unless the port was changed by someone else
    // since. A call starting meanwhile queries after these commands and starts from the result.
    const auto target = _restoreSink.isEmpty() ? nullptr : sink(_restoreSink);
    if (target) {
      const auto name = target->name;
      const auto ours = target->activePort == _selectedPort;
      if (ours) trackVolume();
      if (ours && _selectedPort != _originalPort) {
        const auto original = _originalPort;
        run({QStringLiteral("set-sink-port"), name, original}, [=, this](int code, const QByteArray &) {
          if (code == 0) if (const auto target = sink(name)) target->activePort = original;
        });
      }
      // The volume goes back also after another app moved the port, unless that changed the volume too.
      if (_originalVolume >= 0 && (ours || target->volume == _appliedVolume)
          && (target->volume != _originalVolume || _volumesPending)) {
        setVolume(name, _originalVolume, false);
      }
    }
    _restoreSink.clear();
    _originalPort.clear();
    _selectedPort.clear();
    _originalVolume = _appliedVolume = -1;
    notify();
    return;
  }
  trackVolume();

  // During the call: follow the system to a newly connected device, as the phone apps do.
  const auto defaultChanged = !_knownDefaultSink.isEmpty() && _defaultSink != _knownDefaultSink;
  _knownDefaultSink = _defaultSink;
  const Output *target = find(_currentId);
  if (!target || defaultChanged) {
    target = preferred();
  } else if (!previous.isEmpty()) {
    for (const auto &output : _outputs) {
      if (!previous.contains(output.id) && (output.kind == Kind::Headphones || output.kind == Kind::Bluetooth)) {
        target = &output;
      }
    }
  }
  if (target && target->id != _currentId) apply(*target);
  else notify();
}

void AudioRoute::select(const QString &id) {
  if (!_active) return;
  if (const auto output = find(id)) apply(*output);
}

void AudioRoute::apply(const Output &output) {
  _currentId = output.id;
  const auto target = output.port.isEmpty() ? nullptr : sink(output.sink);
  if (target) {
    if (_restoreSink.isEmpty()) {
      _restoreSink = target->name;
      _originalPort = _selectedPort = target->activePort;
      _originalVolume = target->volume;
    }
    const auto name = output.sink;
    const auto port = output.port;
    if (target->activePort != port) {
      run({QStringLiteral("set-sink-port"), name, port}, [=, this](int code, const QByteArray &) {
        const auto target = sink(name);
        if (code == 0) {
          if (target) target->activePort = port;
          _selectedPort = port;
        } else if (target && _currentId == name + u'/' + port) {
          // The route stays where the sound actually is.
          const auto actual = find(name + u'/' + target->activePort);
          _currentId = actual ? actual->id : QString();
        }
        notify();
      });
    }
    // Queued right after the port: no query sees the new port with the old volume.
    const auto saved = _callVolumes.find(port);
    const auto volume = saved != _callVolumes.end() ? *saved
        : output.kind == Kind::Earpiece ? kEarpieceVolume
        : _originalVolume;
    if (volume >= 0 && (volume != target->volume || _volumesPending)) setVolume(name, volume);
    else _appliedVolume = target->volume;
  }
  notify();
}

void AudioRoute::setVolume(const QString &name, int volume, bool call) {
  ++_volumesPending;
  run({QStringLiteral("set-sink-volume"), name, QString::number(volume)}, [=, this](int code, const QByteArray &) {
    --_volumesPending;
    if (code != 0) return;
    if (const auto target = sink(name)) target->volume = volume;
    if (call) _appliedVolume = volume;
  });
}

// A volume changed during the call (the volume keys, the shell's slider) belongs to the call's port.
void AudioRoute::trackVolume() {
  const auto output = find(_currentId);
  const auto target = _restoreSink.isEmpty() ? nullptr : sink(_restoreSink);
  if (_volumesPending || !output || !target || output->sink != target->name || output->port != target->activePort
      || target->volume < 0 || target->volume == _appliedVolume) {
    return;
  }
  if (_appliedVolume >= 0) rememberVolume(output->port, target->volume);
  _appliedVolume = target->volume;
}

void AudioRoute::rememberVolume(const QString &port, int volume) {
  _callVolumes.insert(port, volume);
  if (_volumesFile.isEmpty()) return;
  QSettings settings(_volumesFile, QSettings::IniFormat);
  settings.setValue(kVolumesGroup + u'/' + port, volume);
}

void AudioRoute::end() {
  _active = false;
  stopMonitor();
  if (!_restoreSink.isEmpty()) query();
}

} // namespace Contrail
