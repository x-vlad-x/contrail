// Plays a quiet tone through media::OpenAlAudioDevice on the real sound server, with no call and no account, and
// reports what the sound server did with the queue. An underrun is a click.
//   tone-probe SECONDS [FIXED_QUEUE] [AMPLITUDE]
// FIXED_QUEUE (buffers of 10 ms) turns the growing queue off: 5 is the module as it was before the FLX1s fix.
#include "media/openal_adm.hpp"

#include <api/make_ref_counted.h>
#include <api/task_queue/default_task_queue_factory.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

class Tone final : public webrtc::AudioTransport {
 public:
  explicit Tone(double amplitude) : _amplitude(amplitude) {}

  int32_t RecordedDataIsAvailable(const void *, size_t samples, size_t, size_t, uint32_t, uint32_t, int32_t, uint32_t,
                                  bool, uint32_t &) override {
    _recorded += samples;
    return 0;
  }

  int32_t NeedMorePlayData(size_t samples, size_t, size_t channels, uint32_t rate, void *data, size_t &out,
                           int64_t *elapsed, int64_t *ntp) override {
    auto *to = static_cast<int16_t *>(data);
    for (size_t i = 0; i != samples; ++i) {
      const auto value = int16_t(_amplitude * 32767 * std::sin(_phase));
      _phase += 2 * M_PI * 440 / rate;
      if (_phase > 2 * M_PI) _phase -= 2 * M_PI;
      for (size_t c = 0; c != channels; ++c) *to++ = value;
    }
    out = samples * channels;  // WebRTC counts the samples of all the channels here.
    *elapsed = 0;
    *ntp = 0;
    return 0;
  }

  void PullRenderData(int, int, size_t, size_t, void *, int64_t *, int64_t *) override {}

  [[nodiscard]] size_t recorded() const { return _recorded; }

 private:
  double _amplitude = 0;
  double _phase = 0;
  std::atomic<size_t> _recorded = 0;
};

}  // namespace

int main(int argc, char **argv) {
  const auto seconds = (argc > 1) ? std::atoi(argv[1]) : 5;
  const auto fixed = (argc > 2) ? std::atoi(argv[2]) : 0;
  const auto amplitude = (argc > 3) ? std::atof(argv[3]) : 0.03;

  auto factory = webrtc::CreateDefaultTaskQueueFactory();
  auto device = rtc::make_ref_counted<media::OpenAlAudioDevice>(factory.get());
  auto tone = Tone(amplitude);
  device->setFixedPlayoutQueue(fixed);
  device->RegisterAudioCallback(&tone);
  if (device->Init() != 0) {
    std::puts("init failed");
    return 1;
  }
  std::printf("playout devices %d, recording devices %d\n", int(device->PlayoutDevices()),
              int(device->RecordingDevices()));
  device->InitPlayout();
  device->InitRecording();
  device->StartPlayout();
  device->StartRecording();
  for (auto second = 1; second <= seconds; ++second) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto stats = device->stats();
    std::printf("%3ds played=%lld underruns=%lld keep=%d largest_take=%d latency=%dms recorded=%lld\n", second,
                (long long)stats.buffersPlayed, (long long)stats.underruns, stats.keepReadyCount, stats.largestTake,
                stats.playoutLatencyMs, (long long)stats.buffersRecorded);
    std::fflush(stdout);
  }
  device->StopRecording();
  device->StopPlayout();
  device->Terminate();
  const auto stats = device->stats();
  std::printf("RESULT underruns=%lld of %lld buffers, %.2f/s, microphone samples %zu\n",
              (long long)stats.underruns, (long long)stats.buffersPlayed, double(stats.underruns) / seconds,
              tone.recorded());
  return 0;
}
