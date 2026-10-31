// Destroys a tgcalls instance with the real sound module, with or without stop() first. No account, no network peer.
// Checks what a client has to do when TDLib closes in the middle of a call.
//
//   teardown-probe VERSION stop|nostop [SECONDS]
//   teardown-probe VERSION pair [SECONDS] [MS_WAITED_BEFORE_THE_PROCESS_EXITS]
#include "media/openal_adm.hpp"

#include "Instance.h"
#include "InstanceImpl.h"
#include "StaticThreads.h"
#include "v2/InstanceV2Impl.h"
#include "v2/InstanceV2ReferenceImpl.h"

#include <api/make_ref_counted.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

const auto RegisterV1 = tgcalls::Register<tgcalls::InstanceImpl>();
const auto RegisterV2 = tgcalls::Register<tgcalls::InstanceV2Impl>();
const auto RegisterV2Reference = tgcalls::Register<tgcalls::InstanceV2ReferenceImpl>();

}  // namespace

// Two instances that reach each other on this device, exchanging signaling in memory, so the teardown runs on a
// CONNECTED call, which `stop|nostop` above never had.
int Pair(const std::string &version, int seconds, int settleMs) {
  auto key = std::make_shared<std::array<uint8_t, tgcalls::EncryptionKey::kSize>>();
  key->fill(7);
  std::array<std::unique_ptr<tgcalls::Instance>, 2> instances;
  std::mutex mutex;
  std::array<std::atomic<bool>, 2> connected{};
  std::array<std::vector<std::vector<uint8_t>>, 2> early;
  const auto deliver = [&](int to, const std::vector<uint8_t> &data) {
    auto lock = std::lock_guard(mutex);
    if (instances[to]) instances[to]->receiveSignalingData(data);
    else early[to].push_back(data);
  };
  const auto make = [&](int index) {
    auto descriptor = tgcalls::Descriptor{
        .version = version,
        .config = tgcalls::Config{.initializationTimeout = 30., .receiveTimeout = 10., .enableP2P = true,
                                  .enableNS = true, .enableAGC = true, .enableVolumeControl = true,
                                  .maxApiLayer = 92},
        .encryptionKey = tgcalls::EncryptionKey(key, index == 0),
        .mediaDevicesConfig = tgcalls::MediaDevicesConfig{.inputVolume = 1.f, .outputVolume = 1.f},
        .stateUpdated = [&connected, index](tgcalls::State state) {
          if (state == tgcalls::State::Established) connected[index] = true;
        },
        .signalBarsUpdated = [](int) {},
        .remoteMediaStateUpdated = [](tgcalls::AudioState, tgcalls::VideoState) {},
        .signalingDataEmitted = [&deliver, index](const std::vector<uint8_t> &data) { deliver(1 - index, data); },
        .createAudioDeviceModule = [](webrtc::TaskQueueFactory *factory) {
          return webrtc::scoped_refptr<webrtc::AudioDeviceModule>(
              rtc::make_ref_counted<media::OpenAlAudioDevice>(factory));
        },
    };
    auto instance = tgcalls::Meta::Create(version, std::move(descriptor));
    auto lock = std::lock_guard(mutex);
    instances[index] = std::move(instance);
    for (const auto &data : early[index]) instances[index]->receiveSignalingData(data);
    early[index].clear();
  };
  make(0);
  make(1);
  for (auto waited = 0; waited < 200 && !(connected[0] && connected[1]); ++waited) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::printf("connected %d %d\n", int(connected[0]), int(connected[1]));
  std::fflush(stdout);
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  for (auto &instance : instances) {
    instance->stop([](tgcalls::FinalState) {});
    auto lock = std::lock_guard(mutex);
    instance.reset();
  }
  if (settleMs) std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));
  std::puts("CLEAN");
  std::fflush(stdout);
  return (connected[0] && connected[1]) ? 0 : 3;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    std::puts("teardown-probe VERSION stop|nostop [SECONDS]");
    return 2;
  }
  const auto version = std::string(argv[1]);
  if (std::string(argv[2]) == "pair") {
    return Pair(version, (argc > 3) ? std::atoi(argv[3]) : 3, (argc > 4) ? std::atoi(argv[4]) : 0);
  }
  const auto stop = std::string(argv[2]) == "stop";
  const auto seconds = (argc > 3) ? std::atoi(argv[3]) : 3;

  auto key = std::make_shared<std::array<uint8_t, tgcalls::EncryptionKey::kSize>>();
  key->fill(7);

  auto descriptor = tgcalls::Descriptor{
      .version = version,
      .config =
          tgcalls::Config{
              .initializationTimeout = 30.,
              .receiveTimeout = 10.,
              .enableP2P = false,
              .enableNS = true,
              .enableAGC = true,
              .enableVolumeControl = true,
              .maxApiLayer = 92,
          },
      .encryptionKey = tgcalls::EncryptionKey(key, true),
      .mediaDevicesConfig = tgcalls::MediaDevicesConfig{.inputVolume = 1.f, .outputVolume = 1.f},
      .stateUpdated = [](tgcalls::State) {},
      .signalBarsUpdated = [](int) {},
      .remoteMediaStateUpdated = [](tgcalls::AudioState, tgcalls::VideoState) {},
      .signalingDataEmitted = [](const std::vector<uint8_t> &) {},
      .createAudioDeviceModule = [](webrtc::TaskQueueFactory *factory) {
        return webrtc::scoped_refptr<webrtc::AudioDeviceModule>(
            rtc::make_ref_counted<media::OpenAlAudioDevice>(factory));
      },
  };

  auto instance = tgcalls::Meta::Create(version, std::move(descriptor));
  if (!instance) {
    std::printf("tgcalls does not know version %s.\n", version.c_str());
    return 2;
  }
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  if (stop) instance->stop([](tgcalls::FinalState) {});
  instance.reset();
  std::puts("CLEAN");
  return 0;
}
