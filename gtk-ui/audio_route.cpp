#include "audio_route.hpp"

#include <json-glib/json-glib.h>
#include <chrono>
#include <thread>
#include <filesystem>
#include <algorithm>

namespace Contrail {
namespace {

bool PhonePort(const std::string &port, const char *what) {
  auto *lower = g_ascii_strdown(port.c_str(), -1);
  const auto result = std::string(lower).find(what) != std::string::npos;
  g_free(lower);
  return result;
}

bool WiredPort(const std::string &port) {
  return PhonePort(port, "wired") || PhonePort(port, "headset") || PhonePort(port, "headphone");
}

// PulseAudio's 100%. The earpiece is quiet: a call starts loud enough on it.
constexpr auto kNormalVolume = 65536;
constexpr auto kEarpieceVolume = kNormalVolume * 4 / 5;
constexpr auto kVolumesGroup = "call-volume";

std::string String(JsonObject *object, const char *key) {
  auto *node = object ? json_object_get_member(object, key) : nullptr;
  return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
      ? json_node_get_string(node) : "";
}
JsonObject *Object(JsonObject *object, const char *key) {
  return object && json_object_has_member(object, key) ? json_object_get_object_member(object, key) : nullptr;
}
JsonArray *Array(JsonObject *object, const char *key) {
  return object && json_object_has_member(object, key) ? json_object_get_array_member(object, key) : nullptr;
}

} // namespace

AudioRoute::AudioRoute(Changed changed, std::string volumesFile)
    : _cancel(g_cancellable_new()), _changed(std::move(changed)), _volumesFile(std::move(volumesFile)) {
  auto *settings = g_key_file_new();
  if (!_volumesFile.empty() && g_key_file_load_from_file(settings, _volumesFile.c_str(), G_KEY_FILE_NONE, nullptr)) {
    auto **keys = g_key_file_get_keys(settings, kVolumesGroup, nullptr, nullptr);
    for (auto **key = keys; key && *key; ++key) {
      const auto volume = g_key_file_get_integer(settings, kVolumesGroup, *key, nullptr);
      if (volume >= 0) _callVolumes[*key] = volume;
    }
    g_strfreev(keys);
  }
  g_key_file_unref(settings);
}

AudioRoute::~AudioRoute() {
  _changed = nullptr;
  end();
  // Finish restoring the media port and volume while callbacks can still reach this object.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (busy() && std::chrono::steady_clock::now() < deadline) {
    g_main_context_iteration(nullptr, FALSE);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  _alive.reset();
  g_cancellable_cancel(_cancel);
  if (_process) { g_subprocess_force_exit(_process); g_clear_object(&_process); }
  stopMonitor();
  g_object_unref(_cancel);
}

void AudioRoute::notify() { if (_changed) _changed(); }

void AudioRoute::run(std::vector<std::string> arguments, std::function<void(int, const std::string &)> done) {
  _queue.push_back({std::move(arguments), std::move(done)});
  next();
}

void AudioRoute::next() {
  if (_process || _queue.empty()) return;
  _running = std::move(_queue.front());
  _queue.pop_front();
  auto args = std::vector<const char *>{"pactl"};
  for (const auto &arg : _running.arguments) args.push_back(arg.c_str());
  args.push_back(nullptr);
  auto *launcher = g_subprocess_launcher_new(GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE));
  g_subprocess_launcher_setenv(launcher, "LC_ALL", "C", TRUE);
  _process = g_subprocess_launcher_spawnv(launcher, args.data(), nullptr);
  g_object_unref(launcher);
  if (!_process) {
    auto command = std::move(_running);
    _running = {};
    if (command.done) command.done(-1, {});
    next();
    return;
  }
  using Pending = std::pair<AudioRoute *, std::weak_ptr<char>>;
  g_subprocess_communicate_utf8_async(_process, nullptr, _cancel,
      +[](GObject *source, GAsyncResult *result, gpointer data) {
        auto pending = std::unique_ptr<Pending>(static_cast<Pending *>(data));
        gchar *output = nullptr;
        const auto ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &output, nullptr, nullptr);
        if (!pending->second.expired()) {
          auto *self = pending->first;
          const auto code = ok && g_subprocess_get_if_exited(G_SUBPROCESS(source))
              ? g_subprocess_get_exit_status(G_SUBPROCESS(source)) : -1;
          auto command = std::move(self->_running);
          self->_running = {};
          g_clear_object(&self->_process);
          if (command.done) command.done(code, output ? output : "");
          self->next();
        }
        g_free(output);
      }, new Pending(this, _alive));
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
  if (_monitor) return;
  auto *launcher = g_subprocess_launcher_new(GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE));
  g_subprocess_launcher_setenv(launcher, "LC_ALL", "C", TRUE);
  const char *arguments[] = {"pactl", "subscribe", nullptr};
  _monitor = g_subprocess_launcher_spawnv(launcher, arguments, nullptr);
  g_object_unref(launcher);
  if (!_monitor) return;
  _events = g_data_input_stream_new(g_subprocess_get_stdout_pipe(_monitor));
  readEvent();
}

void AudioRoute::readEvent() {
  if (!_events) return;
  using Pending = std::pair<AudioRoute *, std::weak_ptr<char>>;
  g_data_input_stream_read_line_async(_events, G_PRIORITY_DEFAULT, _cancel,
      +[](GObject *source, GAsyncResult *result, gpointer data) {
        auto pending = std::unique_ptr<Pending>(static_cast<Pending *>(data));
        auto *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), result, nullptr, nullptr);
        if (!pending->second.expired()) {
          auto *self = pending->first;
          if (line && self->_events == G_DATA_INPUT_STREAM(source)) {
            const auto text = std::string(line);
            if (self->_active && (text.find("sink #") != std::string::npos
                || text.find("server") != std::string::npos || text.find("card #") != std::string::npos)) self->query();
            self->readEvent();
          }
        }
        g_free(line);
      }, new Pending(this, _alive));
}

void AudioRoute::stopMonitor() {
  if (_monitor) g_subprocess_force_exit(_monitor);
  g_clear_object(&_events);
  g_clear_object(&_monitor);
}

void AudioRoute::query() {
  if (_queryQueued) return;
  _queryQueued = true;
  run({"--format=json", "info"}, [this](int code, const std::string &output) {
    auto *parser = json_parser_new();
    _defaultSink.clear();
    if (code == 0 && json_parser_load_from_data(parser, output.data(), output.size(), nullptr)) {
      auto *root = json_parser_get_root(parser);
      if (JSON_NODE_HOLDS_OBJECT(root)) _defaultSink = String(json_node_get_object(root), "default_sink_name");
    }
    g_object_unref(parser);
  });
  run({"--format=json", "list", "sinks"}, [this](int code, const std::string &output) {
    _queryQueued = false;
    if (code == 0) parseSinks(output);
    else _sinks.clear();
    queried();
  });
}

void AudioRoute::parseSinks(const std::string &json) {
  _sinks.clear();
  auto *parser = json_parser_new();
  if (!json_parser_load_from_data(parser, json.data(), json.size(), nullptr)
      || !JSON_NODE_HOLDS_ARRAY(json_parser_get_root(parser))) { g_object_unref(parser); return; }
  auto *array = json_node_get_array(json_parser_get_root(parser));
  auto devices = std::vector<std::string>();
  for (guint i = 0; i < json_array_get_length(array); ++i) {
    auto *object = json_array_get_object_element(array, i);
    auto *properties = Object(object, "properties");
    auto sink = Sink{.name = String(object, "name"), .description = String(object, "description"),
        .activePort = String(object, "active_port"), .bus = String(properties, "device.bus"),
        .formFactor = String(properties, "device.form_factor")};
    if (sink.description.empty()) sink.description = sink.name;
    if (auto *volume = Object(object, "volume")) {
      auto *members = json_object_get_members(volume);
      for (auto *p = members; p; p = p->next) {
        auto *channel = Object(volume, static_cast<const char *>(p->data));
        if (channel && json_object_has_member(channel, "value")) {
          sink.volume = std::max(sink.volume, int(json_object_get_int_member(channel, "value")));
        }
      }
      g_list_free(members);
    }
    sink.device = sink.description;
    for (auto count = 1; std::find(devices.begin(), devices.end(), sink.device) != devices.end();) {
      sink.device = sink.description + " #" + std::to_string(++count);
    }
    devices.push_back(sink.device);
    auto anyAvailable = false;
    if (auto *ports = Array(object, "ports")) for (guint j = 0; j < json_array_get_length(ports); ++j) {
      auto *port = json_array_get_object_element(ports, j);
      const auto available = String(port, "availability") != "not available";
      sink.ports.push_back({String(port, "name"), available});
      anyAvailable = anyAvailable || available;
    }
    sink.skip = String(object, "driver").find("null-sink") != std::string::npos
        || sink.name.starts_with("auto_null") || sink.name.starts_with("sink.fake")
        || (!sink.ports.empty() && !anyAvailable);
    _sinks.push_back(std::move(sink));
  }
  g_object_unref(parser);
}

AudioRoute::Sink *AudioRoute::sink(const std::string &name) {
  const auto i = std::find_if(_sinks.begin(), _sinks.end(), [&](const Sink &sink) { return sink.name == name; });
  return i == _sinks.end() ? nullptr : &*i;
}

const AudioRoute::Output *AudioRoute::find(const std::string &id) const {
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
    if (output.port.empty()) whole = &output;
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
  std::vector<std::string> previous;
  for (const auto &output : _outputs) previous.push_back(output.id);
  _outputs.clear();
  for (const auto &sink : _sinks) {
    if (sink.skip) continue;
    std::string earpiece, speaker, wired;
    for (const auto &port : sink.ports) {
      if (!port.available) continue;
      if (PhonePort(port.name, "earpiece")) earpiece = port.name;
      else if (PhonePort(port.name, "speaker")) speaker = port.name;
      else if (WiredPort(port.name) && wired.empty()) wired = port.name;
    }
    if (!earpiece.empty() && !speaker.empty()) {
      const auto add = [&](const std::string &port, Kind kind) {
        _outputs.push_back({sink.name + '/' + port, sink.name, port, std::string(), sink.device, kind});
      };
      add(earpiece, Kind::Earpiece);
      add(speaker, Kind::Speaker);
      if (!wired.empty()) add(wired, Kind::Headphones);
      continue;
    }
    const auto kind = (sink.bus == "bluetooth" || sink.name.starts_with("bluez"))
        ? Kind::Bluetooth
        : (sink.formFactor == "headphone" || sink.formFactor == "headset"
            || WiredPort(sink.activePort))
        ? Kind::Headphones
        : Kind::Other;
    _outputs.push_back({sink.name, sink.name, std::string(), sink.description, sink.device, kind});
  }

  if (!_active) {
    // After the call: put the phone's port and volume back, unless the port was changed by someone else
    // since. A call starting meanwhile queries after these commands and starts from the result.
    const auto target = _restoreSink.empty() ? nullptr : sink(_restoreSink);
    if (target) {
      const auto name = target->name;
      const auto ours = target->activePort == _selectedPort;
      if (ours) trackVolume();
      if (ours && _selectedPort != _originalPort) {
        const auto original = _originalPort;
        run({"set-sink-port", name, original}, [=, this](int code, const std::string &) {
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
  const auto defaultChanged = !_knownDefaultSink.empty() && _defaultSink != _knownDefaultSink;
  _knownDefaultSink = _defaultSink;
  const Output *target = find(_currentId);
  if (!target || defaultChanged) {
    target = preferred();
  } else if (!previous.empty()) {
    for (const auto &output : _outputs) {
      if (std::find(previous.begin(), previous.end(), output.id) == previous.end() && (output.kind == Kind::Headphones || output.kind == Kind::Bluetooth)) {
        target = &output;
      }
    }
  }
  if (target && target->id != _currentId) apply(*target);
  else notify();
}

void AudioRoute::select(const std::string &id) {
  if (!_active) return;
  if (const auto output = find(id)) apply(*output);
}

void AudioRoute::apply(const Output &output) {
  _currentId = output.id;
  const auto target = output.port.empty() ? nullptr : sink(output.sink);
  if (target) {
    if (_restoreSink.empty()) {
      _restoreSink = target->name;
      _originalPort = _selectedPort = target->activePort;
      _originalVolume = target->volume;
    }
    const auto name = output.sink;
    const auto port = output.port;
    if (target->activePort != port) {
      run({"set-sink-port", name, port}, [=, this](int code, const std::string &) {
        const auto target = sink(name);
        if (code == 0) {
          if (target) target->activePort = port;
          _selectedPort = port;
        } else if (target && _currentId == name + '/' + port) {
          // The route stays where the sound actually is.
          const auto actual = find(name + '/' + target->activePort);
          _currentId = actual ? actual->id : std::string();
        }
        notify();
      });
    }
    // Queued right after the port: no query sees the new port with the old volume.
    const auto saved = _callVolumes.find(port);
    const auto volume = saved != _callVolumes.end() ? saved->second
        : output.kind == Kind::Earpiece ? kEarpieceVolume
        : _originalVolume;
    if (volume >= 0 && (volume != target->volume || _volumesPending)) setVolume(name, volume);
    else _appliedVolume = target->volume;
  }
  notify();
}

void AudioRoute::setVolume(const std::string &name, int volume, bool call) {
  ++_volumesPending;
  run({"set-sink-volume", name, std::to_string(volume)}, [=, this](int code, const std::string &) {
    --_volumesPending;
    if (code != 0) return;
    if (const auto target = sink(name)) target->volume = volume;
    if (call) _appliedVolume = volume;
  });
}

// A volume changed during the call (the volume keys, the shell's slider) belongs to the call's port.
void AudioRoute::trackVolume() {
  const auto output = find(_currentId);
  const auto target = _restoreSink.empty() ? nullptr : sink(_restoreSink);
  if (_volumesPending || !output || !target || output->sink != target->name || output->port != target->activePort
      || target->volume < 0 || target->volume == _appliedVolume) {
    return;
  }
  if (_appliedVolume >= 0) rememberVolume(output->port, target->volume);
  _appliedVolume = target->volume;
}

void AudioRoute::rememberVolume(const std::string &port, int volume) {
  _callVolumes[port] = volume;
  if (_volumesFile.empty()) return;
  auto *settings = g_key_file_new();
  for (const auto &[name, saved] : _callVolumes) g_key_file_set_integer(settings, kVolumesGroup, name.c_str(), saved);
  const auto directory = std::filesystem::path(_volumesFile).parent_path().string();
  g_mkdir_with_parents(directory.c_str(), 0700);
  g_key_file_save_to_file(settings, _volumesFile.c_str(), nullptr);
  g_key_file_unref(settings);
}

void AudioRoute::end() {
  _active = false;
  stopMonitor();
  if (!_restoreSink.empty()) query();
}

} // namespace Contrail
