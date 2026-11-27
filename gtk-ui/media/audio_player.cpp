#include "media/audio_player.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace Contrail {

struct AudioPlayer::Shared {
  std::mutex mutex;
  std::condition_variable wake;
  // Set by the main thread.
  std::string path;  // The parts TDLib has, or the whole file.
  std::int64_t size = 0;
  bool complete = false;
  std::map<std::int64_t, std::int64_t> parts;  // Downloaded, start to end, without overlaps.
  bool stop = false;
  bool paused = false;
  double seekTo = -1.;
  std::uint64_t commands = 0;  // Grows with every pause, play and seek: a read waiting for data gives way to them.
  // Set by the playing thread.
  std::int64_t needed = -1;  // A read waits for the data from there.
  double position = 0.;
  double fileLength = 0.;  // 0 while the file doesn't tell.
  bool playing = false;    // The sound has started.
  bool ended = false;
  bool failed = false;

  // Under the mutex: how much can be read from `offset` on.
  [[nodiscard]] std::int64_t availableAt(std::int64_t offset) const {
    if (path.empty()) return 0;
    if (complete) return std::max<std::int64_t>(size - offset, 0);
    auto it = parts.upper_bound(offset);
    if (it == parts.begin()) return 0;
    --it;
    return it->second > offset ? it->second - offset : 0;
  }
  // TDLib tells only the part from where its last download started: the parts before stay downloaded.
  void addPart(std::int64_t start, std::int64_t end) {
    if (end <= start) return;
    auto it = parts.upper_bound(start);
    if (it != parts.begin()) {
      const auto before = std::prev(it);
      if (before->second >= start) {
        start = before->first;
        end = std::max(end, before->second);
        it = parts.erase(before);
      }
    }
    while (it != parts.end() && it->first <= end) {
      end = std::max(end, it->second);
      it = parts.erase(it);
    }
    parts.emplace(start, end);
  }
};

namespace {

using Shared = AudioPlayer::Shared;

constexpr auto kPriority = 32;
constexpr auto kTickMs = 100u;
constexpr auto kBuffers = 4;
constexpr auto kChunkSeconds = 0.1;
constexpr auto kReadBuffer = 32 * 1024;
constexpr auto kProbeSize = 32 * 1024;
constexpr auto kWaitMs = 100;
constexpr auto kIdleMs = 30;
// A read this far past the end of a download running waits for it: moving the download costs more.
constexpr auto kDownloadAhead = std::int64_t(128 * 1024);

// The file as far as it is downloaded. A read past that waits, and gives way to a command the player hasn't
// handled yet (AVERROR_EXIT) once interruptible: FFmpeg then has to seek before it reads on.
class Input final {
 public:
  Input(Shared &shared, const std::uint64_t &handled, std::function<void()> waiting)
      : _shared(shared), _handled(handled), _waiting(std::move(waiting)) {}
  ~Input() {
    if (_fd >= 0) close(_fd);
  }
  Input(const Input &) = delete;
  Input &operator=(const Input &) = delete;

  int read(std::uint8_t *buffer, int size) {
    auto available = std::int64_t(0);
    auto path = std::string();
    {
      std::unique_lock lock(_shared.mutex);
      while (true) {
        if (_shared.stop || (_interruptible && _shared.commands != _handled)) {
          _shared.needed = -1;
          _interrupted = true;
          return AVERROR_EXIT;
        }
        // The end of the file, downloaded whole or not.
        if (_shared.size > 0 && _position >= _shared.size) return AVERROR_EOF;
        available = _shared.availableAt(_position);
        if (available > 0) break;
        _shared.needed = _position;
        _shared.wake.wait_for(lock, std::chrono::milliseconds(kWaitMs));
        // The sound queued before goes on meanwhile: its position is shown.
        if (_waiting) {
          lock.unlock();
          _waiting();
          lock.lock();
        }
      }
      _shared.needed = -1;
      path = _shared.path;
    }
    // Once downloaded the file is somewhere else.
    if (path != _path) {
      if (_fd >= 0) close(_fd);
      _path = path;
      _fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    }
    if (_fd < 0) return AVERROR(EIO);
    const auto got = pread(_fd, buffer, std::min<std::int64_t>(size, available), _position);
    if (got < 0) return AVERROR(errno);
    if (got == 0) return AVERROR_EOF;
    _position += got;
    return int(got);
  }

  std::int64_t seek(std::int64_t offset, int whence) {
    whence &= ~AVSEEK_FORCE;
    auto size = std::int64_t(0);
    {
      std::lock_guard lock(_shared.mutex);
      size = _shared.size;
    }
    switch (whence) {
      case AVSEEK_SIZE: return size > 0 ? size : -1;
      case SEEK_SET: _position = offset; break;
      case SEEK_CUR: _position += offset; break;
      case SEEK_END:
        if (size <= 0) return -1;
        _position = size + offset;
        break;
      default: return -1;
    }
    return _position;
  }

  // Only stop() breaks into the opening; a pause or a seek waits for it.
  void setInterruptible(bool value) { _interruptible = value; }
  [[nodiscard]] bool takeInterrupted() { return std::exchange(_interrupted, false); }

 private:
  Shared &_shared;
  const std::uint64_t &_handled;  // The commands the player has seen (Shared::commands then).
  std::function<void()> _waiting;
  std::string _path;
  int _fd = -1;
  std::int64_t _position = 0;
  bool _interruptible = false;
  bool _interrupted = false;
};

// One track, on its own thread, from the opening to the end or stop().
class Engine final {
 public:
  explicit Engine(std::shared_ptr<Shared> shared)
      : _shared(std::move(shared)), _input(*_shared, _handled, [this] {
          reclaim();
          publish();
        }) {}
  ~Engine();
  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;

  void run(double from);

 private:
  enum class Step { More, End, Interrupted, Failed };

  bool open();
  bool openDevice();
  void seekTo(double seconds);
  Step decode();
  void convert();
  bool fill();
  void reclaim();
  void play();
  void devicePaused(bool paused);
  [[nodiscard]] double position() const;
  void publish();

  std::shared_ptr<Shared> _shared;
  std::uint64_t _handled = 0;
  Input _input;
  AVFormatContext *_format = nullptr;
  AVCodecContext *_codec = nullptr;
  SwrContext *_swr = nullptr;
  AVPacket *_packet = nullptr;
  AVFrame *_frame = nullptr;
  AVRational _timeBase = {1, 1};
  int _stream = -1;
  int _rate = 0;
  int _channels = 0;
  double _length = 0.;
  ALCdevice *_device = nullptr;
  ALCcontext *_context = nullptr;
  ALCboolean (*_setThreadContext)(ALCcontext *) = nullptr;
  void (*_pauseDevice)(ALCdevice *) = nullptr;
  void (*_resumeDevice)(ALCdevice *) = nullptr;
  ALuint _source = 0;
  ALuint _buffers[kBuffers] = {};
  std::deque<ALuint> _free;
  std::deque<int> _queued;  // The frames in each queued buffer, in their order.
  std::vector<std::int16_t> _pcm;
  std::vector<std::int16_t> _converted;
  double _base = 0.;          // Where the queue started.
  std::int64_t _played = 0;   // Frames played since then.
  double _skipUntil = -1.;    // After a seek: what decodes before this is dropped.
  double _resumeAt = -1.;     // A read gave way to a command: the decoder goes on from there.
  bool _interrupted = false;
  bool _ended = false;        // The decoder gave its last frame.
  bool _draining = false;
  bool _started = false;
};

Engine::~Engine() {
  if (_source) {
    alSourceStop(_source);
    alSourcei(_source, AL_BUFFER, 0);
    alDeleteSources(1, &_source);
    alDeleteBuffers(kBuffers, _buffers);
  }
  if (_context) {
    if (_setThreadContext) {
      _setThreadContext(nullptr);
    } else {
      alcMakeContextCurrent(nullptr);
    }
    alcDestroyContext(_context);
  }
  if (_device) alcCloseDevice(_device);
  av_frame_free(&_frame);
  av_packet_free(&_packet);
  swr_free(&_swr);
  avcodec_free_context(&_codec);
  if (_format) {
    auto *io = _format->pb;
    avformat_close_input(&_format);
    if (io) {
      av_freep(&io->buffer);
      avio_context_free(&io);
    }
  }
}

bool Engine::open() {
  auto *buffer = static_cast<unsigned char *>(av_malloc(kReadBuffer));
  if (!buffer) return false;
  auto *io = avio_alloc_context(
      buffer, kReadBuffer, 0, &_input,
      +[](void *opaque, std::uint8_t *data, int size) { return static_cast<Input *>(opaque)->read(data, size); },
      nullptr, +[](void *opaque, std::int64_t offset, int whence) {
        return static_cast<Input *>(opaque)->seek(offset, whence);
      });
  if (!io) {
    av_free(buffer);
    return false;
  }
  auto complete = false;
  {
    std::lock_guard lock(_shared->mutex);
    complete = _shared->complete;
  }
  // Ogg and MP3 look for the length at the end of a file that can be seeked: one still downloading would wait for
  // its end first. The message tells the length then.
  io->seekable = complete ? AVIO_SEEKABLE_NORMAL : 0;
  _format = avformat_alloc_context();
  if (!_format) {
    av_freep(&io->buffer);
    avio_context_free(&io);
    return false;
  }
  _format->pb = io;
  _format->flags |= AVFMT_FLAG_CUSTOM_IO;
  // One kind of audio in a file: a few packets tell it. FFmpeg would read seconds of it first, waiting for them
  // to download.
  _format->probesize = kProbeSize;
  _format->max_analyze_duration = AV_TIME_BASE / 2;
  if (avformat_open_input(&_format, nullptr, nullptr, nullptr) < 0) {
    // It frees the format context on failure, not the AVIO.
    av_freep(&io->buffer);
    avio_context_free(&io);
    return false;
  }
  if (avformat_find_stream_info(_format, nullptr) < 0) return false;
  io->seekable = AVIO_SEEKABLE_NORMAL;
  const AVCodec *decoder = nullptr;
  _stream = av_find_best_stream(_format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
  if (_stream < 0 || !decoder) return false;
  const auto *stream = _format->streams[_stream];
  _timeBase = stream->time_base;
  _codec = avcodec_alloc_context3(decoder);
  if (!_codec || avcodec_parameters_to_context(_codec, stream->codecpar) < 0) return false;
  if (avcodec_open2(_codec, decoder, nullptr) < 0) return false;
  _rate = _codec->sample_rate;
  if (_rate <= 0) return false;
  auto input = AVChannelLayout();
  if (_codec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
    av_channel_layout_default(&input, _codec->ch_layout.nb_channels);
  } else if (av_channel_layout_copy(&input, &_codec->ch_layout) < 0) {
    return false;
  }
  _channels = input.nb_channels > 1 ? 2 : 1;
  auto output = AVChannelLayout();
  av_channel_layout_default(&output, _channels);
  const auto made = swr_alloc_set_opts2(&_swr, &output, AV_SAMPLE_FMT_S16, _rate, &input, _codec->sample_fmt, _rate,
                                        0, nullptr);
  av_channel_layout_uninit(&input);
  av_channel_layout_uninit(&output);
  if (made < 0 || swr_init(_swr) < 0) return false;
  if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
    _length = double(stream->duration) * av_q2d(_timeBase);
  } else if (_format->duration != AV_NOPTS_VALUE && _format->duration > 0) {
    _length = double(_format->duration) / AV_TIME_BASE;
  }
  _packet = av_packet_alloc();
  _frame = av_frame_alloc();
  return _packet && _frame;
}

bool Engine::openDevice() {
  _device = alcOpenDevice(nullptr);
  if (!_device) return false;
  _context = alcCreateContext(_device, nullptr);
  if (!_context) return false;
  // Only for this thread: calls have their own device and context.
  if (alcIsExtensionPresent(nullptr, "ALC_EXT_thread_local_context")) {
    _setThreadContext = reinterpret_cast<ALCboolean (*)(ALCcontext *)>(alcGetProcAddress(nullptr, "alcSetThreadContext"));
  }
  if (_setThreadContext) {
    _setThreadContext(_context);
  } else {
    alcMakeContextCurrent(_context);
  }
  // A paused device stops mixing silence: no wakeups while a track is paused.
  if (alcIsExtensionPresent(_device, "ALC_SOFT_pause_device")) {
    _pauseDevice = reinterpret_cast<void (*)(ALCdevice *)>(alcGetProcAddress(_device, "alcDevicePauseSOFT"));
    _resumeDevice = reinterpret_cast<void (*)(ALCdevice *)>(alcGetProcAddress(_device, "alcDeviceResumeSOFT"));
  }
  alGetError();
  alGenSources(1, &_source);
  if (alGetError() != AL_NO_ERROR) {
    _source = 0;
    return false;
  }
  alGenBuffers(kBuffers, _buffers);
  if (alGetError() != AL_NO_ERROR) {
    alDeleteSources(1, &_source);
    _source = 0;
    return false;
  }
  alSourcei(_source, AL_SOURCE_RELATIVE, AL_TRUE);
  _free.assign(std::begin(_buffers), std::end(_buffers));
  return true;
}

void Engine::run(double from) {
  if (!open() || !openDevice()) {
    std::lock_guard lock(_shared->mutex);
    _shared->failed = true;
    return;
  }
  {
    std::lock_guard lock(_shared->mutex);
    _handled = _shared->commands;
  }
  _input.setInterruptible(true);
  if (from > 0.) seekTo(from);
  auto paused = false;
  while (true) {
    auto seek = -1.;
    auto pause = false;
    {
      std::lock_guard lock(_shared->mutex);
      if (_shared->stop) break;
      _handled = _shared->commands;
      pause = _shared->paused;
      seek = std::exchange(_shared->seekTo, -1.);
    }
    if (seek >= 0.) {
      _interrupted = false;
      seekTo(seek);
    } else if (std::exchange(_interrupted, false)) {
      seekTo(_resumeAt);
    }
    if (pause != paused) {
      paused = pause;
      if (paused) {
        alSourcePause(_source);
        devicePaused(true);
      } else {
        devicePaused(false);
      }
    }
    if (paused) {
      publish();
      std::unique_lock lock(_shared->mutex);
      _shared->wake.wait(lock, [&] { return _shared->stop || _shared->commands != _handled; });
      continue;
    }
    reclaim();
    while (int(_queued.size()) < kBuffers && !_interrupted) {
      if (!fill()) break;
    }
    if (_interrupted) continue;
    play();
    publish();
    if (_ended && _queued.empty() && _pcm.empty()) {
      std::lock_guard lock(_shared->mutex);
      _shared->ended = true;
      break;
    }
    std::unique_lock lock(_shared->mutex);
    _shared->wake.wait_for(lock, std::chrono::milliseconds(kIdleMs),
                           [&] { return _shared->stop || _shared->commands != _handled; });
  }
}

void Engine::seekTo(double seconds) {
  alSourceStop(_source);
  reclaim();
  _played = 0;
  _base = seconds;
  _pcm.clear();
  _ended = false;
  _draining = false;
  _skipUntil = seconds;
  _format->pb->eof_reached = 0;
  _format->pb->error = 0;
  const auto target = std::int64_t(seconds / av_q2d(_timeBase));
  if (avformat_seek_file(_format, _stream, INT64_MIN, target, target, 0) < 0) {
    av_seek_frame(_format, _stream, target, AVSEEK_FLAG_BACKWARD);
  }
  avcodec_flush_buffers(_codec);
  swr_init(_swr);  // Drops what it held from before.
  if (_input.takeInterrupted()) {
    _interrupted = true;
    _resumeAt = seconds;
  }
}

// One step of the decoder: a frame converted, or a packet read and given to it.
Engine::Step Engine::decode() {
  auto result = avcodec_receive_frame(_codec, _frame);
  if (result == 0) {
    convert();
    av_frame_unref(_frame);
    return Step::More;
  }
  if (result == AVERROR_EOF) {
    _ended = true;
    return Step::End;
  }
  if (result != AVERROR(EAGAIN)) return Step::Failed;
  result = av_read_frame(_format, _packet);
  if (result < 0) {
    if (_input.takeInterrupted()) {
      _interrupted = true;
      _resumeAt = position();
      return Step::Interrupted;
    }
    if (!_draining) {
      _draining = true;
      avcodec_send_packet(_codec, nullptr);
      return Step::More;
    }
    _ended = true;
    return Step::End;
  }
  if (_packet->stream_index == _stream) {
    // A broken packet is skipped: the next one may decode.
    avcodec_send_packet(_codec, _packet);
  }
  av_packet_unref(_packet);
  return Step::More;
}

void Engine::convert() {
  auto drop = 0;  // Frames of this one before where a seek went.
  if (_skipUntil >= 0.) {
    const auto pts = _frame->best_effort_timestamp;
    if (pts != AV_NOPTS_VALUE) {
      const auto start = double(pts) * av_q2d(_timeBase);
      const auto end = start + double(_frame->nb_samples) / _rate;
      if (end <= _skipUntil) return;
      drop = std::max(0, int((_skipUntil - start) * _rate));
    }
    _skipUntil = -1.;
  }
  const auto wanted = swr_get_out_samples(_swr, _frame->nb_samples);
  if (wanted <= 0) return;
  _converted.resize(std::size_t(wanted) * _channels);
  auto *out = reinterpret_cast<std::uint8_t *>(_converted.data());
  const auto got = swr_convert(_swr, &out, wanted, const_cast<const std::uint8_t **>(_frame->extended_data),
                               _frame->nb_samples);
  if (got <= drop) return;
  _pcm.insert(_pcm.end(), _converted.begin() + std::ptrdiff_t(drop) * _channels,
              _converted.begin() + std::ptrdiff_t(got) * _channels);
}

// A buffer of sound queued, false when nothing more can be now: the end, an error, a command.
bool Engine::fill() {
  const auto chunk = std::size_t(_rate * kChunkSeconds) * _channels;
  while (_pcm.size() < chunk && !_ended) {
    const auto step = decode();
    if (step == Step::Interrupted) return false;
    if (step == Step::Failed) {
      _ended = true;
      break;
    }
  }
  if (_pcm.empty() || _free.empty()) return false;
  const auto buffer = _free.front();
  _free.pop_front();
  alBufferData(buffer, _channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16, _pcm.data(),
               ALsizei(_pcm.size() * sizeof(std::int16_t)), _rate);
  alSourceQueueBuffers(_source, 1, &buffer);
  _queued.push_back(int(_pcm.size() / _channels));
  _pcm.clear();
  return !_ended;
}

void Engine::reclaim() {
  auto processed = ALint(0);
  alGetSourcei(_source, AL_BUFFERS_PROCESSED, &processed);
  while (processed-- > 0 && !_queued.empty()) {
    auto buffer = ALuint(0);
    alSourceUnqueueBuffers(_source, 1, &buffer);
    _played += _queued.front();
    _queued.pop_front();
    _free.push_back(buffer);
  }
}

// After a seek, or when the sound ran out while the file was downloading.
void Engine::play() {
  if (_queued.empty()) return;
  auto state = ALint(0);
  alGetSourcei(_source, AL_SOURCE_STATE, &state);
  if (state != AL_PLAYING) alSourcePlay(_source);
  _started = true;
}

void Engine::devicePaused(bool paused) {
  if (paused && _pauseDevice) {
    _pauseDevice(_device);
  } else if (!paused && _resumeDevice) {
    _resumeDevice(_device);
  }
}

double Engine::position() const {
  if (_rate <= 0 || !_source) return _base;  // Still opening.
  auto offset = ALint(0);
  if (!_queued.empty()) alGetSourcei(_source, AL_SAMPLE_OFFSET, &offset);
  return _base + double(_played + offset) / _rate;
}

void Engine::publish() {
  const auto now = position();
  std::lock_guard lock(_shared->mutex);
  _shared->position = now;
  _shared->fileLength = _length;
  _shared->playing = _started;
}

}  // namespace

AudioPlayer::AudioPlayer(Td::Store &store) : _store(store) {
  _store.onFile([this](const Td::File &file) {
    if (_shared && file.id == _state.track.file) feed(file);
  }, _lifetime);
}

AudioPlayer::~AudioPlayer() {
  stopThread();
}

void AudioPlayer::playPause(const Track &track) {
  if (!current(track)) {
    start(track, 0.);
    return;
  }
  setPaused(_state.status != Status::Paused);
  notify();
}

void AudioPlayer::seek(const Track &track, double fraction) {
  fraction = std::clamp(fraction, 0., 1.);
  if (!current(track)) {
    start(track, fraction);
    return;
  }
  const auto to = fraction * _state.length;
  // A drag paused the track: it plays on. A track paused before stays paused at its new place.
  const auto resume = std::exchange(_resumeAfterSeeking, false);
  {
    std::lock_guard lock(_shared->mutex);
    _shared->seekTo = to;
    if (resume) _shared->paused = false;
    ++_shared->commands;
  }
  _shared->wake.notify_all();
  _state.position = to;
  if (resume) _state.status = Status::Playing;
  notify();
}

void AudioPlayer::startSeeking(const Track &track) {
  if (!current(track) || _state.status == Status::Paused) return;
  _resumeAfterSeeking = true;
  setPaused(true);
}

void AudioPlayer::cancelSeeking(const Track &track) {
  if (!current(track) || !std::exchange(_resumeAfterSeeking, false)) return;
  setPaused(false);
  notify();
}

void AudioPlayer::stop() {
  if (_state.status == Status::Stopped && !_shared) return;
  stopThread();
  _state = State();
  notify();
}

void AudioPlayer::onState(std::function<void(const State &)> handler, const Td::Lifetime &lifetime) {
  _subscribers.push_back({std::move(handler), lifetime.token()});
}

void AudioPlayer::setNext(Next next, const Td::Lifetime &lifetime) {
  _next = std::move(next);
  _nextToken = lifetime.token();
}

std::string AudioPlayer::debugState() const {
  const auto status = _state.status == Status::Playing ? "playing"
                      : _state.status == Status::Paused ? "paused"
                      : _state.status == Status::Loading ? "loading"
                                                         : "stopped";
  // With a point whatever the locale: the tests read it.
  const auto number = [](double value) {
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    return std::string(g_ascii_formatd(buffer, sizeof(buffer), "%.2f", value));
  };
  return std::string("status=") + status + " message=" + std::to_string(_state.track.message >> 20)
         + " position=" + number(_state.position) + " length=" + number(_state.length)
         + " buffered=" + number(_state.buffered) + " waiting=" + std::to_string(int(_state.waiting))
         + " thread=" + std::to_string(int(_thread.joinable()));
}

void AudioPlayer::start(const Track &track, double from) {
  // The length the file told, when it is this track again.
  const auto length = (_state.track.same(track) && _state.length > 0.) ? _state.length : double(track.duration);
  stopThread();
  _state = State();
  _state.track = track;
  _state.status = Status::Loading;
  _state.length = length;
  _state.position = from * length;
  _resumeAfterSeeking = false;
  _asked = -1;
  _shared = std::make_shared<Shared>();
  // Listened: the dot under a voice message goes for its sender too, as the official apps do when it plays.
  if (track.message > 0) _store.client().send(Td::api::make_object<Td::api::openMessageContent>(track.chat, track.message));
  const auto known = _store.file(track.file);
  if (known) feed(*known);
  if (!known || !known->complete) {
    _asked = 0;
    _store.download(track.file, kPriority);
  }
  _thread = std::thread([shared = _shared, at = _state.position] { Engine(shared).run(at); });
  _timer = g_timeout_add(kTickMs, +[](gpointer data) -> gboolean {
    static_cast<AudioPlayer *>(data)->tick();
    return G_SOURCE_CONTINUE;
  }, this);
  notify();
}

void AudioPlayer::stopThread() {
  if (_timer) {
    g_source_remove(_timer);
    _timer = 0;
  }
  if (_shared) {
    {
      std::lock_guard lock(_shared->mutex);
      _shared->stop = true;
    }
    _shared->wake.notify_all();
  }
  if (_thread.joinable()) _thread.join();
  _shared = nullptr;
}

void AudioPlayer::feed(const Td::File &file) {
  auto buffered = 1.;
  {
    std::lock_guard lock(_shared->mutex);
    _shared->size = file.size;
    _shared->complete = file.complete;
    _shared->path = file.complete ? file.path : file.partialPath;
    if (!file.complete) {
      _shared->addPart(file.prefixOffset, file.prefixOffset + file.prefix);
      buffered = file.size > 0 ? double(_shared->availableAt(0)) / double(file.size) : 0.;
    }
  }
  _shared->wake.notify_all();
  _state.buffered = std::clamp(buffered, 0., 1.);
}

void AudioPlayer::tick() {
  auto needed = std::int64_t(-1);
  auto position = 0.;
  auto length = 0.;
  auto playing = false;
  auto ended = false;
  auto failed = false;
  {
    std::lock_guard lock(_shared->mutex);
    needed = _shared->needed;
    position = _shared->position;
    length = _shared->fileLength;
    playing = _shared->playing;
    ended = _shared->ended;
    failed = _shared->failed;
  }
  if (failed) {
    g_warning("Player: can't play file %d", _state.track.file);
    stop();
    return;
  }
  if (ended) {
    finished();
    return;
  }
  if (needed >= 0) {
    // Past what the download running brings soon: it is moved there.
    const auto file = _store.file(_state.track.file);
    const auto running = file && file->downloading && needed >= file->prefixOffset
                         && needed <= file->prefixOffset + file->prefix + kDownloadAhead;
    if (!running && (needed != _asked || (file && !file->downloading))) {
      _asked = needed;
      _store.downloadFrom(_state.track.file, kPriority, needed);
    }
  }
  if (length > 0.) _state.length = length;
  if (_state.status != Status::Paused) {
    _state.position = position;
    _state.status = playing ? Status::Playing : Status::Loading;
  }
  _state.waiting = needed >= 0 && playing;
  notify();
}

void AudioPlayer::finished() {
  const auto track = _state.track;
  stopThread();
  _state = State();
  auto next = std::optional<Track>();
  if (_next && !_nextToken.expired()) next = _next(track);
  if (next) {
    start(*next, 0.);
  } else {
    notify();
  }
}

void AudioPlayer::setPaused(bool paused) {
  if (!_shared) return;
  {
    std::lock_guard lock(_shared->mutex);
    _shared->paused = paused;
    ++_shared->commands;
  }
  _shared->wake.notify_all();
  if (paused) {
    _state.status = Status::Paused;
  } else {
    auto playing = false;
    {
      std::lock_guard lock(_shared->mutex);
      playing = _shared->playing;
    }
    _state.status = playing ? Status::Playing : Status::Loading;
  }
}

void AudioPlayer::notify() {
  // A handler can subscribe while this runs (a new row): those see the next change.
  const auto count = _subscribers.size();
  for (auto i = std::size_t(0); i < count; ++i) {
    if (_subscribers[i].token.expired()) continue;
    const auto handler = _subscribers[i].handler;
    handler(_state);
  }
  std::erase_if(_subscribers, [](const Subscriber &item) { return item.token.expired(); });
}

}  // namespace Contrail
