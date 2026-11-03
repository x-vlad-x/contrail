#pragma once

#include <gio/gio.h>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <memory>

namespace Contrail {

// Where a call is heard, chosen as in the phone apps: the earpiece, the loudspeaker, headphones or
// another device (Bluetooth). FuriOS exposes the earpiece, the loudspeaker and the wired jack as ports
// of one PulseAudio sink, not as separate devices; every other sink is one output.
// Those ports share the sink's volume, so a call keeps a volume of its own for each of them (in
// volumesFile) and the sink gets its volume back after the call, as phones keep call and media volumes apart.
class AudioRoute final {
public:
  enum class Kind { Earpiece, Speaker, Headphones, Bluetooth, Other };
  struct Output {
    std::string id;
    std::string sink;
    std::string port;   // A port of the phone's sink; empty for a whole sink.
    std::string name;   // The sink's description; empty for the phone's own ports.
    std::string device; // The sink's name for the core's audio (OpenAL): its unique description.
    Kind kind = Kind::Other;
  };
  using Changed = std::function<void()>;

  explicit AudioRoute(Changed changed, std::string volumesFile = {});
  ~AudioRoute();

  // A call starts where the sound goes now: headphones or another device when one is the default
  // output, otherwise the earpiece (the loudspeaker for a video call).
  void begin(bool video);
  void select(const std::string &id);
  void end();

  [[nodiscard]] const std::vector<Output> &outputs() const { return _outputs; }
  [[nodiscard]] const Output *current() const;
  [[nodiscard]] bool busy() const { return _process || !_queue.empty(); }

private:
  struct Port {
    std::string name;
    bool available = true;
  };
  struct Sink {
    std::string name;
    std::string description;
    std::string device;
    std::string activePort;
    std::string bus;
    std::string formFactor;
    int volume = -1;
    std::vector<Port> ports;
    bool skip = false;
  };
  struct Command {
    std::vector<std::string> arguments;
    std::function<void(int exitCode, const std::string &output)> done;
  };

  void run(std::vector<std::string> arguments, std::function<void(int, const std::string &)> done);
  void next();
  void query();
  void parseSinks(const std::string &json);
  void queried();
  void apply(const Output &output);
  void setVolume(const std::string &sink, int volume, bool call = true);
  void rememberVolume(const std::string &port, int volume);
  void trackVolume();
  void startMonitor();
  void stopMonitor();
  [[nodiscard]] const Output *find(const std::string &id) const;
  [[nodiscard]] const Output *preferred() const;
  [[nodiscard]] Sink *sink(const std::string &name);
  void notify();

  GSubprocess *_process = nullptr;
  GSubprocess *_monitor = nullptr;
  GDataInputStream *_events = nullptr;
  GCancellable *_cancel = nullptr;
  std::shared_ptr<char> _alive = std::make_shared<char>();
  void readEvent();
  std::deque<Command> _queue;
  Command _running;
  Changed _changed;
  std::vector<Sink> _sinks;
  std::vector<Output> _outputs;
  std::string _defaultSink;
  std::string _knownDefaultSink;
  std::string _currentId;
  // The port and the volume of the phone's sink before the call, to put back afterwards unless
  // someone else changed the port in the meantime.
  std::string _restoreSink;
  std::string _originalPort;
  std::string _selectedPort;
  int _originalVolume = -1;
  int _appliedVolume = -1;
  int _volumesPending = 0;
  std::string _volumesFile;
  std::map<std::string, int> _callVolumes;
  bool _active = false;
  bool _video = false;
  bool _queryQueued = false;
};

} // namespace Contrail
