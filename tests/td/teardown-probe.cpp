// Destroys a tgcalls instance with the real sound module, with or without stop() first. No account, no network peer.
// Checks what a client has to do when TDLib closes in the middle of a call.
//
//   teardown-probe VERSION stop|nostop [SECONDS]
#include "media/openal_adm.hpp"

#include "Instance.h"
#include "InstanceImpl.h"
#include "StaticThreads.h"
#include "v2/InstanceV2Impl.h"
#include "v2/InstanceV2ReferenceImpl.h"

#include <api/make_ref_counted.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace {

const auto RegisterV1 = tgcalls::Register<tgcalls::InstanceImpl>();
const auto RegisterV2 = tgcalls::Register<tgcalls::InstanceV2Impl>();
const auto RegisterV2Reference = tgcalls::Register<tgcalls::InstanceV2ReferenceImpl>();

}  // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::puts("teardown-probe VERSION stop|nostop [SECONDS]");
    return 2;
  }
  const auto version = std::string(argv[1]);
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
