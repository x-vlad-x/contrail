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

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  if (argc > 1) {
    const auto original = hardwarePort();
    assert(!original.isEmpty());
    auto available = false;
    Contrail::AudioRoute route([&](bool ready, bool, const QString &) { available = ready; });
    route.begin();
    waitFor([&] { return available && !route.busy(); });
    route.toggle();
    waitFor([&] { return available && !route.busy(); });
    assert(hardwarePort() != original);
    route.end();
    waitFor([&] { return !route.busy(); });
    assert(hardwarePort() == original);
    std::puts("Phone audio ports: switched speaker/earpiece and restored the original route");
    return 0;
  }
  QTemporaryDir directory;
  assert(directory.isValid());
  const auto state = directory.path() + QStringLiteral("/port");
  const auto pactl = directory.path() + QStringLiteral("/pactl");
  write(state, "output-earpiece");
  write(pactl, R"py(#!/usr/bin/python3
import json, os, pathlib, sys, time
state = pathlib.Path(os.environ['CONTRAIL_TEST_AUDIO_STATE'])
if sys.argv[1:] == ['--format=json', 'list', 'sinks']:
    print(json.dumps([{'name': 'phone-output', 'description': 'Phone audio',
        'active_port': state.read_text(), 'ports': [
            {'name': 'output-speaker', 'availability': 'available'},
            {'name': 'output-earpiece', 'availability': 'available'},
            {'name': 'output-wired_headset', 'availability': 'available'}]}]))
elif len(sys.argv) == 4 and sys.argv[1:3] == ['set-sink-port', 'phone-output']:
    time.sleep(0.1)
    if state.with_name('fail').exists(): sys.exit(1)
    state.write_text(sys.argv[3])
else: sys.exit(1)
)py");
  assert(QFile::setPermissions(pactl, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
  qputenv("PATH", directory.path().toUtf8() + ':' + qgetenv("PATH"));
  qputenv("CONTRAIL_TEST_AUDIO_STATE", state.toUtf8());

  auto available = false;
  auto speaker = false;
  Contrail::AudioRoute route([&](bool ready, bool active, const QString &) { available = ready; speaker = active; });
  const auto ready = [&] { return available && !route.busy(); };
  route.begin();
  waitFor(ready);
  assert(!speaker);
  route.toggle();
  waitFor([&] { return ready() && speaker; });
  assert(read(state) == "output-speaker");
  route.toggle();
  waitFor([&] { return ready() && !speaker; });
  assert(read(state) == "output-earpiece");

  // Hanging up while the route change is still in flight must restore the initial port.
  route.toggle();
  route.end();
  waitFor([&] { return !route.busy() && read(state) == "output-earpiece"; });

  // Ending a call must respect a later route chosen by another app or system settings.
  route.begin();
  waitFor(ready);
  route.toggle();
  waitFor([&] { return ready() && speaker; });
  write(state, "output-wired_headset");
  route.end();
  waitFor([&] { return !route.busy(); });
  assert(read(state) == "output-wired_headset");

  write(state, "output-earpiece");
  route.begin();
  waitFor(ready);
  write(directory.path() + QStringLiteral("/fail"), "1");
  route.toggle();
  waitFor([&] { return !route.busy(); });
  assert(!speaker);
  assert(read(state) == "output-earpiece");
  assert(QFile::remove(directory.path() + QStringLiteral("/fail")));
  route.end();
  std::puts("Audio regressions: speaker/earpiece, restore, hangup during switching, external route and failed switching passed");
}
