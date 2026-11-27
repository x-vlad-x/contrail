#include "media/voice_recorder.hpp"

#include "td/session.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>

namespace Contrail {

struct VoiceRecorder::Shared {
  std::mutex mutex;
  std::condition_variable wake;
  bool stop = false;
  // Set by the recording thread.
  std::int64_t samples = 0;
  int level = 0;
  std::vector<std::uint16_t> peaks;  // Read once the thread is joined.
  bool failed = false;
};

namespace {

namespace api = Td::api;

constexpr auto kRate = 48000;
constexpr auto kBitRate = 32000;
constexpr auto kPeakSamples = kRate / 50;  // A value of the waveform for every 20 ms.
constexpr auto kWaveformValues = 100;
constexpr auto kSilentPeak = 2500;  // Quieter than this is near silence: it doesn't fill the waveform.
constexpr auto kTickMs = 100u;
constexpr auto kPollMs = 20;
constexpr auto kMinDuration = 1.;
constexpr auto kActionPeriod = gint64(5) * G_USEC_PER_SEC;
constexpr auto kKeepFiles = gint64(7) * 24 * 3600 * G_USEC_PER_SEC;

// The scenario's microphone (CONTRAIL_SCENARIO_MICROPHONE=1): a tone that swells, at the pace a microphone gives.
bool ScenarioMicrophone() {
  static const auto result = g_getenv("CONTRAIL_SCENARIO_MICROPHONE") != nullptr;
  return result;
}

class Microphone final {
 public:
  Microphone() = default;
  ~Microphone() {
    if (_device) {
      alcCaptureStop(_device);
      alcCaptureCloseDevice(_device);
    }
  }
  Microphone(const Microphone &) = delete;
  Microphone &operator=(const Microphone &) = delete;

  bool open() {
    if (ScenarioMicrophone()) {
      _started = g_get_monotonic_time();
      return true;
    }
    _device = alcCaptureOpenDevice(nullptr, kRate, AL_FORMAT_MONO16, kRate / 2);
    if (!_device) return false;
    _disconnect = alcIsExtensionPresent(_device, "ALC_EXT_disconnect");
    alcCaptureStart(_device);
    return alcGetError(_device) == ALC_NO_ERROR;
  }

  // What came since the last read; false when the microphone is gone.
  bool read(std::vector<std::int16_t> &out) {
    out.clear();
    if (!_device) {
      const auto due = (g_get_monotonic_time() - _started) * kRate / G_USEC_PER_SEC;
      for (; _made < due; ++_made) {
        const auto time = double(_made) / kRate;
        const auto loudness = 7000. + 6000. * std::sin(2 * G_PI * 0.7 * time);
        out.push_back(std::int16_t(loudness * std::sin(2 * G_PI * 440. * time)));
      }
      return true;
    }
    if (_disconnect) {
      auto connected = ALCint(1);
      alcGetIntegerv(_device, ALC_CONNECTED, 1, &connected);
      if (!connected) return false;
    }
    auto count = ALCint(0);
    alcGetIntegerv(_device, ALC_CAPTURE_SAMPLES, 1, &count);
    if (alcGetError(_device) != ALC_NO_ERROR) return false;
    if (count > 0) {
      out.resize(std::size_t(count));
      alcCaptureSamples(_device, out.data(), count);
    }
    return true;
  }

 private:
  ALCdevice *_device = nullptr;
  bool _disconnect = false;
  gint64 _started = 0;
  std::int64_t _made = 0;
};

// Opus in Ogg, as Telegram's voice messages are.
class Encoder final {
 public:
  Encoder() = default;
  ~Encoder() {
    av_frame_free(&_frame);
    av_packet_free(&_packet);
    avcodec_free_context(&_codec);
    if (_format) {
      if (_opened) avio_closep(&_format->pb);
      avformat_free_context(_format);
    }
  }
  Encoder(const Encoder &) = delete;
  Encoder &operator=(const Encoder &) = delete;

  bool open(const std::string &path) {
    const auto *codec = avcodec_find_encoder_by_name("libopus");
    if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    if (!codec) return false;
    if (avformat_alloc_output_context2(&_format, nullptr, "ogg", path.c_str()) < 0) return false;
    _stream = avformat_new_stream(_format, nullptr);
    _codec = avcodec_alloc_context3(codec);
    if (!_stream || !_codec) return false;
    _codec->sample_rate = kRate;
    av_channel_layout_default(&_codec->ch_layout, 1);
    _codec->bit_rate = kBitRate;
    _codec->time_base = {1, kRate};
    _codec->sample_fmt = sampleFormat(codec);
    _float = _codec->sample_fmt == AV_SAMPLE_FMT_FLT || _codec->sample_fmt == AV_SAMPLE_FMT_FLTP;
    // FFmpeg's own Opus encoder, when it was built without libopus.
    if (std::strcmp(codec->name, "libopus") != 0) _codec->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    if (_format->oformat->flags & AVFMT_GLOBALHEADER) _codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    auto *options = static_cast<AVDictionary *>(nullptr);
    av_dict_set(&options, "application", "voip", 0);
    const auto opened = avcodec_open2(_codec, codec, &options);
    av_dict_free(&options);
    if (opened < 0) return false;
    if (avcodec_parameters_from_context(_stream->codecpar, _codec) < 0) return false;
    _stream->time_base = _codec->time_base;
    if (avio_open(&_format->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) return false;
    _opened = true;
    if (avformat_write_header(_format, nullptr) < 0) return false;
    _frameSize = _codec->frame_size > 0 ? _codec->frame_size : kPeakSamples;
    _frame = av_frame_alloc();
    _packet = av_packet_alloc();
    if (!_frame || !_packet) return false;
    _frame->nb_samples = _frameSize;
    _frame->format = _codec->sample_fmt;
    _frame->sample_rate = kRate;
    if (av_channel_layout_copy(&_frame->ch_layout, &_codec->ch_layout) < 0) return false;
    return av_frame_get_buffer(_frame, 0) >= 0;
  }

  bool write(const std::vector<std::int16_t> &samples) {
    _pending.insert(_pending.end(), samples.begin(), samples.end());
    auto used = std::size_t(0);
    for (; _pending.size() - used >= std::size_t(_frameSize); used += _frameSize) {
      if (!encode(_pending.data() + used)) return false;
    }
    _pending.erase(_pending.begin(), _pending.begin() + std::ptrdiff_t(used));
    return true;
  }

  // The rest, with silence to a whole frame, and the end of the file.
  bool finish() {
    if (!_pending.empty()) {
      _pending.resize(std::size_t(_frameSize), 0);
      if (!encode(_pending.data())) return false;
      _pending.clear();
    }
    return send(nullptr) && av_write_trailer(_format) >= 0;
  }

 private:
  static AVSampleFormat sampleFormat(const AVCodec *codec) {
    const void *configs = nullptr;
    auto count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configs, &count) < 0
        || !configs) {
      return AV_SAMPLE_FMT_S16;
    }
    const auto *formats = static_cast<const AVSampleFormat *>(configs);
    const auto has = [&](AVSampleFormat format) { return std::find(formats, formats + count, format) != formats + count; };
    if (has(AV_SAMPLE_FMT_S16)) return AV_SAMPLE_FMT_S16;
    if (has(AV_SAMPLE_FMT_FLT)) return AV_SAMPLE_FMT_FLT;
    return has(AV_SAMPLE_FMT_FLTP) ? AV_SAMPLE_FMT_FLTP : formats[0];
  }

  bool encode(const std::int16_t *samples) {
    if (av_frame_make_writable(_frame) < 0) return false;
    if (_float) {
      auto *out = reinterpret_cast<float *>(_frame->data[0]);
      for (auto i = 0; i < _frameSize; ++i) out[i] = float(samples[i]) / 32768.f;
    } else {
      std::memcpy(_frame->data[0], samples, std::size_t(_frameSize) * sizeof(std::int16_t));
    }
    _frame->pts = _pts;
    _pts += _frameSize;
    return send(_frame);
  }

  bool send(AVFrame *frame) {
    if (avcodec_send_frame(_codec, frame) < 0) return false;
    while (true) {
      const auto result = avcodec_receive_packet(_codec, _packet);
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
      if (result < 0) return false;
      av_packet_rescale_ts(_packet, _codec->time_base, _stream->time_base);
      _packet->stream_index = _stream->index;
      if (av_interleaved_write_frame(_format, _packet) < 0) return false;
    }
  }

  AVFormatContext *_format = nullptr;
  AVStream *_stream = nullptr;
  AVCodecContext *_codec = nullptr;
  AVFrame *_frame = nullptr;
  AVPacket *_packet = nullptr;
  std::vector<std::int16_t> _pending;
  std::int64_t _pts = 0;
  int _frameSize = 0;
  bool _float = false;
  bool _opened = false;
};

void Record(const std::shared_ptr<VoiceRecorder::Shared> &shared, const std::string &path) {
  auto microphone = Microphone();
  auto encoder = Encoder();
  const auto failed = [&] {
    std::lock_guard lock(shared->mutex);
    shared->failed = true;
  };
  if (!microphone.open() || !encoder.open(path)) {
    failed();
    return;
  }
  auto chunk = std::vector<std::int16_t>();
  auto peaks = std::vector<std::uint16_t>();
  auto peak = 0;
  auto inPeak = 0;
  auto level = 0;
  auto samples = std::int64_t(0);
  auto stop = false;
  while (!stop) {
    {
      std::unique_lock lock(shared->mutex);
      shared->wake.wait_for(lock, std::chrono::milliseconds(kPollMs), [&] { return shared->stop; });
      stop = shared->stop;  // What came meanwhile is still read.
    }
    if (!microphone.read(chunk) || !encoder.write(chunk)) {
      failed();
      return;
    }
    for (const auto sample : chunk) {
      peak = std::max(peak, std::abs(int(sample)));
      if (++inPeak == kPeakSamples) {
        peaks.push_back(std::uint16_t(std::min(peak, 32767)));
        level = peak;
        peak = inPeak = 0;
      }
    }
    samples += std::int64_t(chunk.size());
    std::lock_guard lock(shared->mutex);
    shared->samples = samples;
    shared->level = level;
  }
  if (!encoder.finish()) {
    failed();
    return;
  }
  std::lock_guard lock(shared->mutex);
  shared->peaks = std::move(peaks);
}

// Recordings stay until TDLib has uploaded them; those older than a week go.
std::string RecordingPath() {
  auto *directory = g_build_filename(Td::DataDirectory().c_str(), "voice", nullptr);
  g_mkdir_with_parents(directory, 0700);
  if (auto *dir = g_dir_open(directory, 0, nullptr)) {
    const auto now = g_get_real_time();
    while (const auto *name = g_dir_read_name(dir)) {
      auto *path = g_build_filename(directory, name, nullptr);
      GStatBuf info;
      if (g_stat(path, &info) == 0 && now - gint64(info.st_mtime) * G_USEC_PER_SEC > kKeepFiles) g_remove(path);
      g_free(path);
    }
    g_dir_close(dir);
  }
  auto *name = g_strdup_printf("voice-%lld.ogg", (long long)g_get_real_time());
  auto *path = g_build_filename(directory, name, nullptr);
  auto result = std::string(path);
  g_free(path);
  g_free(name);
  g_free(directory);
  return result;
}

}  // namespace

VoiceRecorder::VoiceRecorder(Td::Store &store) : _store(store) {}

VoiceRecorder::~VoiceRecorder() {
  if (!_shared) return;
  stopThread();
  g_remove(_path.c_str());
}

bool VoiceRecorder::start(Td::ChatId chat, std::int32_t topic, std::int64_t replyTo) {
  if (_shared) return false;
  if (!ScenarioMicrophone()) {
    const auto *name = alcGetString(nullptr, ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER);
    if (!name || !*name) return false;
  }
  _chat = chat;
  _topic = topic;
  _replyTo = replyTo;
  _path = RecordingPath();
  _state = State();
  _shared = std::make_shared<Shared>();
  _thread = std::thread([shared = _shared, path = _path] { Record(shared, path); });
  _timer = g_timeout_add(kTickMs, +[](gpointer data) -> gboolean {
    static_cast<VoiceRecorder *>(data)->tick();
    return G_SOURCE_CONTINUE;
  }, this);
  _lastAction = 0;
  chatAction(true);
  return true;
}

void VoiceRecorder::finish(bool send) {
  if (!_shared) return;
  stopThread();
  auto samples = std::int64_t(0);
  auto failed = false;
  auto peaks = std::vector<std::uint16_t>();
  {
    std::lock_guard lock(_shared->mutex);
    samples = _shared->samples;
    failed = _shared->failed;
    peaks = std::move(_shared->peaks);
  }
  _shared = nullptr;
  chatAction(false);
  const auto duration = double(samples) / kRate;
  if (send && !failed && duration >= kMinDuration) {
    const auto seconds = int(std::lround(duration));
    auto waveform = Waveform(peaks);
    auto topic = _topic ? api::make_object<api::messageTopicForum>(_topic) : nullptr;
    auto reply = _replyTo ? api::make_object<api::inputMessageReplyToMessage>(_replyTo, nullptr, 0, std::string())
                          : nullptr;
    auto voice = api::make_object<api::inputVoiceNote>(api::make_object<api::inputFileLocal>(_path), seconds, waveform);
    _store.client().send(api::make_object<api::sendMessage>(
        _chat, std::move(topic), std::move(reply), nullptr, nullptr,
        api::make_object<api::inputMessageVoiceNote>(std::move(voice), nullptr, nullptr)));
    _lastSent = "sent=" + std::to_string(seconds) + "s waveform=" + std::to_string(waveform.size());
  } else {
    g_remove(_path.c_str());
  }
  _state = State();
  const auto count = _stateHandlers.size();
  for (auto i = std::size_t(0); i < count; ++i) {
    if (!_stateHandlers[i].token.expired()) _stateHandlers[i].handler(_state);
  }
}

void VoiceRecorder::onState(std::function<void(const State &)> handler, const Td::Lifetime &lifetime) {
  std::erase_if(_stateHandlers, [](const auto &item) { return item.token.expired(); });
  _stateHandlers.push_back({std::move(handler), lifetime.token()});
}

void VoiceRecorder::onFailed(std::function<void()> handler, const Td::Lifetime &lifetime) {
  std::erase_if(_failedHandlers, [](const auto &item) { return item.token.expired(); });
  _failedHandlers.push_back({std::move(handler), lifetime.token()});
}

std::string VoiceRecorder::Waveform(const std::vector<std::uint16_t> &peaks) {
  auto values = std::vector<int>(kWaveformValues, 0);
  const auto count = peaks.size();
  if (count) {
    // Each value is the loudest of its part of the recording.
    for (auto i = std::size_t(0); i < kWaveformValues; ++i) {
      const auto from = i * count / kWaveformValues;
      const auto till = std::max(from + 1, (i + 1) * count / kWaveformValues);
      values[i] = *std::max_element(peaks.begin() + std::ptrdiff_t(from), peaks.begin() + std::ptrdiff_t(till));
    }
  }
  const auto loudest = std::max(*std::max_element(values.begin(), values.end()), kSilentPeak);
  // Five bits each, the lowest first.
  auto result = std::string((kWaveformValues * 5 + 7) / 8, '\0');
  for (auto i = 0; i < kWaveformValues; ++i) {
    const auto value = std::min(31, values[i] * 31 / loudest);
    const auto bit = i * 5;
    const auto bits = unsigned(value) << (bit % 8);
    result[bit / 8] = char(std::uint8_t(result[bit / 8]) | (bits & 0xff));
    if (bits > 0xff) result[bit / 8 + 1] = char(std::uint8_t(result[bit / 8 + 1]) | (bits >> 8));
  }
  return result;
}

std::string VoiceRecorder::debugState() const {
  char buffer[G_ASCII_DTOSTR_BUF_SIZE];
  return std::string("active=") + std::to_string(int(active())) + " chat=" + std::to_string(_chat)
         + " duration=" + g_ascii_formatd(buffer, sizeof(buffer), "%.1f", _state.duration)
         + " level=" + std::to_string(_state.level) + " thread=" + std::to_string(int(_thread.joinable()))
         + (_lastSent.empty() ? "" : " " + _lastSent);
}

void VoiceRecorder::tick() {
  auto samples = std::int64_t(0);
  auto level = 0;
  auto failed = false;
  {
    std::lock_guard lock(_shared->mutex);
    samples = _shared->samples;
    level = _shared->level;
    failed = _shared->failed;
  }
  if (failed) {
    finish(false);
    const auto count = _failedHandlers.size();
    for (auto i = std::size_t(0); i < count; ++i) {
      if (!_failedHandlers[i].token.expired()) _failedHandlers[i].handler();
    }
    return;
  }
  _state = State{double(samples) / kRate, level};
  chatAction(true);
  const auto count = _stateHandlers.size();
  for (auto i = std::size_t(0); i < count; ++i) {
    if (!_stateHandlers[i].token.expired()) _stateHandlers[i].handler(_state);
  }
}

void VoiceRecorder::stopThread() {
  if (_timer) {
    g_source_remove(_timer);
    _timer = 0;
  }
  {
    std::lock_guard lock(_shared->mutex);
    _shared->stop = true;
  }
  _shared->wake.notify_all();
  if (_thread.joinable()) _thread.join();
}

// Telegram shows "recording a voice message" for a few seconds from one request.
void VoiceRecorder::chatAction(bool recording) {
  const auto now = g_get_monotonic_time();
  if (recording && now - _lastAction < kActionPeriod) return;
  _lastAction = recording ? now : 0;
  auto topic = _topic ? api::make_object<api::messageTopicForum>(_topic) : nullptr;
  auto action = recording ? api::object_ptr<api::ChatAction>(api::make_object<api::chatActionRecordingVoiceNote>())
                          : api::object_ptr<api::ChatAction>(api::make_object<api::chatActionCancel>());
  _store.client().send(api::make_object<api::sendChatAction>(_chat, std::move(topic), std::string(), std::move(action)));
}

}  // namespace Contrail
