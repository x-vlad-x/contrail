#pragma once

#include <QtCore/QProcess>
#include <QtCore/QStringList>
#include <deque>
#include <functional>
#include <vector>

namespace Contrail {

// Where a call is heard, chosen as in the phone apps: the earpiece, the loudspeaker, headphones or
// another device (Bluetooth). FuriOS exposes the earpiece, the loudspeaker and the wired jack as ports
// of one PulseAudio sink, not as separate devices; every other sink is one output.
class AudioRoute final {
public:
  enum class Kind { Earpiece, Speaker, Headphones, Bluetooth, Other };
  struct Output {
    QString id;
    QString sink;
    QString port;   // A port of the phone's sink; empty for a whole sink.
    QString name;   // The sink's description; empty for the phone's own ports.
    QString device; // The sink's name for the core's audio (OpenAL): its unique description.
    Kind kind = Kind::Other;
  };
  using Changed = std::function<void()>;

  explicit AudioRoute(Changed changed);
  ~AudioRoute();

  // A call starts where the sound goes now: headphones or another device when one is the default
  // output, otherwise the earpiece (the loudspeaker for a video call).
  void begin(bool video);
  void select(const QString &id);
  void end();

  [[nodiscard]] const std::vector<Output> &outputs() const { return _outputs; }
  [[nodiscard]] const Output *current() const;
  [[nodiscard]] bool busy() const { return _process.state() != QProcess::NotRunning || !_queue.empty(); }

private:
  struct Port {
    QString name;
    bool available = true;
  };
  struct Sink {
    QString name;
    QString description;
    QString device;
    QString activePort;
    QString bus;
    QString formFactor;
    std::vector<Port> ports;
    bool skip = false;
  };
  struct Command {
    QStringList arguments;
    std::function<void(int exitCode, const QByteArray &output)> done;
  };

  void run(QStringList arguments, std::function<void(int, const QByteArray &)> done);
  void next();
  void query();
  void parseSinks(const QByteArray &json);
  void queried();
  void apply(const Output &output);
  void startMonitor();
  void stopMonitor();
  [[nodiscard]] const Output *find(const QString &id) const;
  [[nodiscard]] const Output *preferred() const;
  [[nodiscard]] Sink *sink(const QString &name);
  void notify();

  QProcess _process;
  QProcess _monitor;
  std::deque<Command> _queue;
  Command _running;
  Changed _changed;
  std::vector<Sink> _sinks;
  std::vector<Output> _outputs;
  QString _defaultSink;
  QString _knownDefaultSink;
  QString _currentId;
  // The port of the phone's sink before the call, to put back afterwards unless someone else
  // changed it in the meantime.
  QString _restoreSink;
  QString _originalPort;
  QString _selectedPort;
  bool _active = false;
  bool _video = false;
  bool _queryQueued = false;
};

} // namespace Contrail
