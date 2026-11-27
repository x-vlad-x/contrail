#include "call_tones_decode.hpp"
#include "media/openal_devices.hpp"
#include "call_sounds.h"

#include <AL/al.h>
#include <AL/alc.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <span>
#include <thread>

namespace Contrail::Calls {
namespace {
std::span<const unsigned char> Sound(Tone tone) {
  switch (tone) {
    case Tone::Incoming: return kIncoming;
    case Tone::Outgoing: return kOutgoing;
    case Tone::Connecting: return kConnecting;
    case Tone::Ended: return kEnded;
    case Tone::Busy: return kBusy;
    default: return {};
  }
}

struct Decoder {
  AVFormatContext *format = nullptr;
  AVIOContext *io = nullptr;
  AVCodecContext *codec = nullptr;
  AVPacket *packet = nullptr;
  AVFrame *frame = nullptr;
  SwrContext *resample = nullptr;
  ~Decoder() {
    swr_free(&resample);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
    if (io) { av_freep(&io->buffer); avio_context_free(&io); }
  }
};

struct Source {
  std::span<const unsigned char> bytes;
  std::int64_t offset = 0;
  static int Read(void *data, unsigned char *output, int size) {
    auto &source = *static_cast<Source *>(data);
    const auto count = std::min<std::int64_t>(size, source.bytes.size() - source.offset);
    if (count <= 0) return AVERROR_EOF;
    std::memcpy(output, source.bytes.data() + source.offset, count);
    source.offset += count;
    return int(count);
  }
  static std::int64_t Seek(void *data, std::int64_t offset, int whence) {
    auto &source = *static_cast<Source *>(data);
    if (whence & AVSEEK_SIZE) return source.bytes.size();
    whence &= ~AVSEEK_FORCE;
    const auto target = whence == SEEK_SET ? offset : whence == SEEK_CUR ? source.offset + offset
        : whence == SEEK_END ? std::int64_t(source.bytes.size()) + offset : -1;
    if (target < 0 || target > std::int64_t(source.bytes.size())) return AVERROR(EINVAL);
    return source.offset = target;
  }
};

// OpenAL's thread-local context keeps call tones apart from the voice module's own context.
class Output final {
 public:
  ~Output() {
    if (_context) {
      _setContext(_context);
      if (_source) { alSourceStop(_source); alDeleteSources(1, &_source); }
      if (_buffer) alDeleteBuffers(1, &_buffer);
      _setContext(nullptr);
      alcDestroyContext(_context);
    }
    if (_device) alcCloseDevice(_device);
  }
  bool play(const std::string &device, const std::vector<std::int16_t> &pcm, bool loop) {
    _setContext = reinterpret_cast<SetContext>(alcGetProcAddress(nullptr, "alcSetThreadContext"));
    if (!_setContext || pcm.empty()) return false;
    const auto id = media::OpenAlOutputId(device);
    _device = alcOpenDevice(id.empty() ? nullptr : id.c_str());
    if (!_device && !id.empty()) _device = alcOpenDevice(nullptr);
    if (!_device) return false;
    _context = alcCreateContext(_device, nullptr);
    if (!_context || !_setContext(_context)) return false;
    alGenBuffers(1, &_buffer);
    alBufferData(_buffer, AL_FORMAT_STEREO16, pcm.data(), ALsizei(pcm.size() * sizeof(std::int16_t)), kToneSampleRate);
    alGenSources(1, &_source);
    alSourcei(_source, AL_BUFFER, _buffer);
    alSourcei(_source, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
    alSourcePlay(_source);
    return alGetError() == AL_NO_ERROR;
  }
 private:
  using SetContext = ALCboolean (*)(ALCcontext *);
  SetContext _setContext = nullptr;
  ALCdevice *_device = nullptr;
  ALCcontext *_context = nullptr;
  ALuint _source = 0;
  ALuint _buffer = 0;
};

class Player final : public TonePlayer {
 public:
  ~Player() override {
    { const auto lock = std::lock_guard(_mutex); _quit = true; }
    _wake.notify_one();
    if (_thread.joinable()) _thread.join();
  }
  void play(Tone tone, bool loop) override {
    {
      const auto lock = std::lock_guard(_mutex);
      if (_tone == tone && _loop == loop) return;
      _tone = tone; _loop = loop; ++_generation;
      if (!_thread.joinable() && tone != Tone::None) _thread = std::thread([this] { run(); });
    }
    _wake.notify_one();
  }
  void stop() override { play(Tone::None, false); }
  Tone current() const override { return _playing; }
  void setOutputDevice(const std::string &device) override {
    {
      const auto lock = std::lock_guard(_mutex);
      if (_device == device) return;
      _device = device;
      if (_tone != Tone::None) ++_generation;
    }
    _wake.notify_one();
  }
 private:
  void run() {
    auto lock = std::unique_lock(_mutex);
    auto handled = std::uint64_t(0);
    while (!_quit) {
      _wake.wait(lock, [&] { return _quit || handled != _generation; });
      if (_quit) break;
      const auto generation = _generation;
      const auto tone = _tone;
      const auto loop = _loop;
      const auto device = _device;
      handled = generation;
      if (tone == Tone::None) continue;
      lock.unlock();
      {
        const auto pcm = DecodeCallTone(tone);
        auto output = Output();
        if (output.play(device, pcm, loop)) {
          _playing = tone;
          lock.lock();
          const auto changed = [&] { return _quit || _generation != generation; };
          if (loop) _wake.wait(lock, changed);
          else _wake.wait_for(lock, std::chrono::milliseconds(pcm.size() * 1000 / (kToneSampleRate * 2) + 100), changed);
          if (_generation == generation) _tone = Tone::None;
          lock.unlock();
        }
      }
      _playing = Tone::None;
      lock.lock();
      if (_generation == generation) _tone = Tone::None;
    }
  }
  mutable std::mutex _mutex;
  std::condition_variable _wake;
  std::thread _thread;
  std::string _device;
  Tone _tone = Tone::None;
  std::atomic<Tone> _playing = Tone::None;
  std::uint64_t _generation = 0;
  bool _loop = false;
  bool _quit = false;
};
} // namespace

std::vector<std::int16_t> DecodeCallTone(Tone tone) {
  auto source = Source{Sound(tone)};
  if (source.bytes.empty()) return {};
  auto decoder = Decoder();
  auto *buffer = static_cast<unsigned char *>(av_malloc(4096));
  if (!buffer) return {};
  decoder.io = avio_alloc_context(buffer, 4096, 0, &source, Source::Read, nullptr, Source::Seek);
  if (!decoder.io) { av_free(buffer); return {}; }
  decoder.format = avformat_alloc_context();
  if (!decoder.format) return {};
  decoder.format->pb = decoder.io;
  decoder.format->flags |= AVFMT_FLAG_CUSTOM_IO;
  if (avformat_open_input(&decoder.format, nullptr, nullptr, nullptr) < 0
      || avformat_find_stream_info(decoder.format, nullptr) < 0) return {};
  const AVCodec *codec = nullptr;
  const auto stream = av_find_best_stream(decoder.format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
  if (stream < 0 || !codec) return {};
  decoder.codec = avcodec_alloc_context3(codec);
  if (!decoder.codec || avcodec_parameters_to_context(decoder.codec, decoder.format->streams[stream]->codecpar) < 0) return {};
  decoder.codec->pkt_timebase = decoder.format->streams[stream]->time_base;
  if (avcodec_open2(decoder.codec, codec, nullptr) < 0) return {};
  decoder.packet = av_packet_alloc();
  decoder.frame = av_frame_alloc();
  if (!decoder.packet || !decoder.frame) return {};
  auto pcm = std::vector<std::int16_t>();
  const auto receive = [&] {
    while (avcodec_receive_frame(decoder.codec, decoder.frame) >= 0) {
      if (!decoder.resample) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&decoder.resample, &stereo, AV_SAMPLE_FMT_S16, kToneSampleRate,
            &decoder.frame->ch_layout, AVSampleFormat(decoder.frame->format), decoder.frame->sample_rate, 0, nullptr) < 0
            || swr_init(decoder.resample) < 0) return false;
      }
      const auto maximum = swr_get_out_samples(decoder.resample, decoder.frame->nb_samples);
      if (maximum <= 0) return false;
      const auto before = pcm.size();
      pcm.resize(before + maximum * 2);
      auto *output = reinterpret_cast<unsigned char *>(pcm.data() + before);
      if (decoder.frame->ch_layout.nb_channels < 1 || decoder.frame->ch_layout.nb_channels > 2) return false;
      auto inputs = std::array<const unsigned char *, 2>{decoder.frame->extended_data[0],
          decoder.frame->ch_layout.nb_channels > 1 && av_sample_fmt_is_planar(AVSampleFormat(decoder.frame->format))
              ? decoder.frame->extended_data[1] : nullptr};
      const auto count = swr_convert(decoder.resample, &output, maximum, inputs.data(), decoder.frame->nb_samples);
      if (count < 0) return false;
      pcm.resize(before + count * 2);
    }
    return true;
  };
  while (av_read_frame(decoder.format, decoder.packet) >= 0) {
    if (decoder.packet->stream_index == stream &&
        (avcodec_send_packet(decoder.codec, decoder.packet) < 0 || !receive())) return {};
    av_packet_unref(decoder.packet);
  }
  avcodec_send_packet(decoder.codec, nullptr);
  if (!receive()) return {};
  if (decoder.resample) {
    const auto maximum = swr_get_out_samples(decoder.resample, 0);
    if (maximum > 0) {
      const auto before = pcm.size();
      pcm.resize(before + maximum * 2);
      auto *output = reinterpret_cast<unsigned char *>(pcm.data() + before);
      const auto count = swr_convert(decoder.resample, &output, maximum, nullptr, 0);
      if (count < 0) return {};
      pcm.resize(before + count * 2);
    }
  }
  return pcm;
}

std::unique_ptr<TonePlayer> MakeTonePlayer() { return std::make_unique<Player>(); }
} // namespace Contrail::Calls
