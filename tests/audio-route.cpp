#include "audio_route.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QThread>
#include <cassert>
#include <cstdio>

static void write(const QString &path, const QByteArray &data) {
  QFile file(path);
  assert(file.open(QIODevice::WriteOnly));
  assert(file.write(data) == data.size());
}

static QByteArray read(const QString &path) {
  QFile file(path);
  assert(file.open(QIODevice::ReadOnly));
  return file.readAll();
}

template <typename Predicate>
static void waitFor(Predicate predicate) {
  QElapsedTimer timer;
  timer.start();
  while (!predicate() && timer.elapsed() < 3000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(1);
  }
  assert(predicate());
}

static QString hardwarePort() {
  QProcess process;
  process.start(QStringLiteral("pactl"), {QStringLiteral("--format=json"), QStringLiteral("list"), QStringLiteral("sinks")});
  assert(process.waitForFinished(3000) && process.exitCode() == 0);
  for (const auto &value : QJsonDocument::fromJson(process.readAllStandardOutput()).array()) {
    const auto sink = value.toObject();
    bool speaker = false, earpiece = false;
    for (const auto &entry : sink.value(QStringLiteral("ports")).toArray()) {
      const auto name = entry.toObject().value(QStringLiteral("name")).toString();
      speaker = speaker || name.contains(QStringLiteral("speaker"));
      earpiece = earpiece || name.contains(QStringLiteral("earpiece"));
    }
    if (speaker && earpiece) return sink.value(QStringLiteral("active_port")).toString();
  }
  return {};
}

using Kind = Contrail::AudioRoute::Kind;

static const Contrail::AudioRoute::Output *output(const Contrail::AudioRoute &route, Kind kind) {
  for (const auto &output : route.outputs()) if (output.kind == kind) return &output;
  return nullptr;
}

static bool on(const Contrail::AudioRoute &route, Kind kind) {
  return !route.busy() && route.current() && route.current()->kind == kind;
}

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  if (argc > 1) {
    const auto original = hardwarePort();
    assert(!original.isEmpty());
    Contrail::AudioRoute route([] {});
    route.begin(false);
    waitFor([&] { return !route.busy() && route.current(); });
    const auto other = output(route, route.current()->kind == Kind::Speaker ? Kind::Earpiece : Kind::Speaker);
    assert(other);
    route.select(other->id);
    waitFor([&] { return !route.busy() && hardwarePort() == other->port; });
    route.end();
    waitFor([&] { return !route.busy(); });
    assert(hardwarePort() == original);
    std::puts("Phone audio ports: switched speaker/earpiece and restored the original route");
    return 0;
  }
  QTemporaryDir directory;
  assert(directory.isValid());
  const auto path = [&](const char *name) { return directory.path() + u'/' + QLatin1String(name); };
  const auto state = path("port");
  const auto pactl = path("pactl");
  write(state, "output-speaker");
  const auto volume = path("volume");
  write(volume, "13107");
  write(pactl, R"py(#!/usr/bin/python3
import json, os, pathlib, sys, time
state = pathlib.Path(os.environ['CONTRAIL_TEST_AUDIO_STATE'])
other = lambda name: state.with_name(name)
if sys.argv[1:] == ['--format=json', 'info']:
    default = other('default').read_text() if other('default').exists() else 'phone-output'
    print(json.dumps({'default_sink_name': default}))
elif sys.argv[1:] == ['--format=json', 'list', 'sinks']:
    wired = 'available' if other('wired').exists() else 'not available'
    volume = int(other('volume').read_text())
    sinks = [{'name': 'phone-output', 'description': 'Phone audio', 'driver': 'module-droid-card.c',
        'active_port': state.read_text(), 'properties': {},
        'volume': {'front-left': {'value': volume}, 'front-right': {'value': volume}}, 'ports': [
            {'name': 'output-parking', 'availability': 'available'},
            {'name': 'output-speaker', 'availability': 'available'},
            {'name': 'output-earpiece', 'availability': 'available'},
            {'name': 'output-wired_headset', 'availability': wired}]},
        {'name': 'sink.fake.sco', 'description': 'Null', 'driver': 'module-null-sink.c',
         'active_port': None, 'properties': {}, 'ports': []}]
    if other('bluetooth').exists():
        sinks.append({'name': 'bluez_sink.1C_6E.a2dp_sink', 'description': 'Phone audio',
            'driver': 'module-bluez5-device.c', 'active_port': 'headphone-output',
            'properties': {'device.bus': 'bluetooth'},
            'ports': [{'name': 'headphone-output', 'availability': 'available'}]})
    print(json.dumps(sinks))
elif len(sys.argv) == 4 and sys.argv[1:3] == ['set-sink-port', 'phone-output']:
    time.sleep(0.1)
    if other('fail').exists(): sys.exit(1)
    state.write_text(sys.argv[3])
elif len(sys.argv) == 4 and sys.argv[1:3] == ['set-sink-volume', 'phone-output']:
    other('volume').write_text(sys.argv[3])
elif sys.argv[1:] == ['subscribe']:
    # Someone else changed the sink: the test creates the "event" file.
    while True:
        if other('event').exists():
            other('event').unlink()
            print("Event 'change' on sink #1", flush=True)
        time.sleep(0.02)
else: sys.exit(1)
)py");
  assert(QFile::setPermissions(pactl, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
  qputenv("PATH", directory.path().toUtf8() + ':' + qgetenv("PATH"));
  qputenv("CONTRAIL_TEST_AUDIO_STATE", state.toUtf8());

  const auto volumes = path("volumes.ini");
  auto route = Contrail::AudioRoute([] {}, volumes);
  // An audio call starts on the earpiece; the parking port and the placeholder sink are no outputs.
  route.begin(false);
  waitFor([&] { return on(route, Kind::Earpiece); });
  assert(read(state) == "output-earpiece");
  assert(route.outputs().size() == 2);
  assert(route.current()->device == QStringLiteral("Phone audio"));
  route.select(output(route, Kind::Speaker)->id);
  waitFor([&] { return on(route, Kind::Speaker); });
  assert(read(state) == "output-speaker");
  route.select(output(route, Kind::Earpiece)->id);
  waitFor([&] { return on(route, Kind::Earpiece); });
  route.end();
  waitFor([&] { return !route.busy(); });
  assert(read(state) == "output-speaker");

  // Hanging up while the route change is still in flight must restore the initial port.
  route.begin(false);
  waitFor([&] { return !route.busy() && route.current(); });
  route.select(output(route, Kind::Speaker)->id);
  route.end();
  waitFor([&] { return !route.busy() && read(state) == "output-speaker"; });
  route.begin(false);
  route.end();
  waitFor([&] { return !route.busy(); });
  assert(read(state) == "output-speaker");

  // Ending a call must respect a later route chosen by another app or system settings.
  route.begin(false);
  waitFor([&] { return on(route, Kind::Earpiece); });
  write(state, "output-wired_headset");
  route.end();
  waitFor([&] { return !route.busy(); });
  assert(read(state) == "output-wired_headset");

  // A failed switch leaves the route where the sound is.
  write(state, "output-earpiece");
  route.begin(false);
  waitFor([&] { return on(route, Kind::Earpiece); });
  write(path("fail"), "1");
  route.select(output(route, Kind::Speaker)->id);
  waitFor([&] { return on(route, Kind::Earpiece); });
  assert(read(state) == "output-earpiece");
  assert(QFile::remove(path("fail")));
  route.end();
  waitFor([&] { return !route.busy(); });

  // A video call starts on the loudspeaker, plugged headphones win over both.
  write(state, "output-speaker");
  route.begin(true);
  waitFor([&] { return on(route, Kind::Speaker); });
  route.end();
  waitFor([&] { return !route.busy(); });
  write(path("wired"), "1");
  route.begin(false);
  waitFor([&] { return on(route, Kind::Headphones); });
  assert(read(state) == "output-wired_headset");
  route.end();
  waitFor([&] { return !route.busy(); });
  assert(read(state) == "output-speaker");
  assert(QFile::remove(path("wired")));

  // Bluetooth headphones as the default output: the call goes there and the phone's port is untouched.
  write(path("bluetooth"), "1");
  write(path("default"), "bluez_sink.1C_6E.a2dp_sink");
  route.begin(false);
  waitFor([&] { return on(route, Kind::Bluetooth); });
  assert(route.outputs().size() == 3);
  assert(route.current()->device == QStringLiteral("Phone audio #2"));
  assert(read(state) == "output-speaker");
  route.select(output(route, Kind::Earpiece)->id);
  waitFor([&] { return on(route, Kind::Earpiece); });
  route.select(output(route, Kind::Bluetooth)->id);
  waitFor([&] { return on(route, Kind::Bluetooth); });
  route.end();
  waitFor([&] { return !route.busy(); });
  assert(read(state) == "output-speaker");
  // Call volumes are kept for each phone port and the sink's own volume comes back after the call.
  assert(QFile::remove(path("bluetooth")) && QFile::remove(path("default")));
  write(volume, "13107");
  route.begin(false);
  waitFor([&] { return on(route, Kind::Earpiece) && read(volume) == "52428"; });
  write(volume, "30000");
  write(path("event"), "");
  waitFor([&] { return !QFile::exists(path("event")); });
  // The event reaches the route a little later and makes it query the sink.
  for (QElapsedTimer timer; !timer.isValid() || timer.elapsed() < 300;) {
    if (!timer.isValid()) timer.start();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  waitFor([&] { return !route.busy(); });
  route.select(output(route, Kind::Speaker)->id);
  waitFor([&] { return on(route, Kind::Speaker) && read(volume) == "13107"; });
  write(volume, "20000");
  route.end();
  waitFor([&] { return !route.busy() && read(state) == "output-speaker"; });
  assert(read(volume) == "13107");
  {
    auto again = Contrail::AudioRoute([] {}, volumes);
    again.begin(false);
    waitFor([&] { return on(again, Kind::Earpiece) && read(volume) == "30000"; });
    again.select(output(again, Kind::Speaker)->id);
    waitFor([&] { return on(again, Kind::Speaker) && read(volume) == "20000"; });
    again.end();
    waitFor([&] { return !again.busy(); });
    assert(read(volume) == "13107");
  }
  std::puts("Audio regressions: earpiece/speaker, restore, hangup during switching, external route, failed switching, "
            "video, headphones, Bluetooth and call volumes passed");
}
