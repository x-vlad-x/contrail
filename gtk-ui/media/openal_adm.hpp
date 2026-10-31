// The sound of a call: a WebRTC audio device module on OpenAL, without Qt.
//
// Based on lib_webrtc's webrtc_openal_adm (Desktop App Toolkit, GPLv3 with the OpenSSL exception, see
// https://github.com/desktop-app/legal/blob/master/LEGAL). Loopback capture and the Windows branches are gone;
// the playout queue that grows with what the sound server takes at once is built in.
#pragma once

#include <api/scoped_refptr.h>
#include <api/task_queue/task_queue_factory.h>
#include <modules/audio_device/audio_device_buffer.h>
#include <modules/audio_device/include/audio_device.h>

#include <AL/al.h>
#include <AL/alc.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>

namespace rtc {
class Thread;
}  // namespace rtc

namespace media {

class OpenAlAudioDevice : public webrtc::AudioDeviceModule {
 public:
  // What the module saw, for checking the sound without listening to it.
  struct Stats {
    // Times the source had played all it was given and stopped: every one is a click.
    int64_t underruns = 0;
    // 10 ms buffers handed to OpenAL.
    int64_t buffersPlayed = 0;
    // The longest queue kept ready, in 10 ms buffers.
    int keepReadyCount = 0;
    // The largest number of buffers the sound server took in one pass.
    int largestTake = 0;
    int64_t buffersRecorded = 0;
    int playoutLatencyMs = 0;
  };

  explicit OpenAlAudioDevice(webrtc::TaskQueueFactory *taskQueueFactory);
  ~OpenAlAudioDevice() override;

  // Empty means the system's default. Applied by SetPlayoutDevice and SetRecordingDevice, or the next open.
  void setPlayoutDeviceId(std::string id);
  void setRecordingDeviceId(std::string id);

  // Test only: the queue as it was before it could grow, to show what the growth is worth.
  void setFixedPlayoutQueue(int buffers);

  [[nodiscard]] Stats stats() const;

  int32_t ActiveAudioLayer(AudioLayer *audioLayer) const override;
  int32_t RegisterAudioCallback(webrtc::AudioTransport *audioCallback) override;

  // Main initialization and termination
  int32_t Init() override;
  int32_t Terminate() override;
  bool Initialized() const override;

  // Device enumeration
  int16_t PlayoutDevices() override;
  int16_t RecordingDevices() override;
  int32_t PlayoutDeviceName(uint16_t index, char name[webrtc::kAdmMaxDeviceNameSize],
                            char guid[webrtc::kAdmMaxGuidSize]) override;
  int32_t RecordingDeviceName(uint16_t index, char name[webrtc::kAdmMaxDeviceNameSize],
                              char guid[webrtc::kAdmMaxGuidSize]) override;

  // Device selection
  int32_t SetPlayoutDevice(uint16_t index) override;
  int32_t SetPlayoutDevice(WindowsDeviceType device) override;
  int32_t SetRecordingDevice(uint16_t index) override;
  int32_t SetRecordingDevice(WindowsDeviceType device) override;

  // Audio transport initialization
  int32_t PlayoutIsAvailable(bool *available) override;
  int32_t InitPlayout() override;
  bool PlayoutIsInitialized() const override;
  int32_t RecordingIsAvailable(bool *available) override;
  int32_t InitRecording() override;
  bool RecordingIsInitialized() const override;

  // Audio transport control
  int32_t StartPlayout() override;
  int32_t StopPlayout() override;
  bool Playing() const override;
  int32_t StartRecording() override;
  int32_t StopRecording() override;
  bool Recording() const override;

  // Audio mixer initialization
  int32_t InitSpeaker() override;
  bool SpeakerIsInitialized() const override;
  int32_t InitMicrophone() override;
  bool MicrophoneIsInitialized() const override;

  // Speaker volume controls
  int32_t SpeakerVolumeIsAvailable(bool *available) override;
  int32_t SetSpeakerVolume(uint32_t volume) override;
  int32_t SpeakerVolume(uint32_t *volume) const override;
  int32_t MaxSpeakerVolume(uint32_t *maxVolume) const override;
  int32_t MinSpeakerVolume(uint32_t *minVolume) const override;

  // Microphone volume controls
  int32_t MicrophoneVolumeIsAvailable(bool *available) override;
  int32_t SetMicrophoneVolume(uint32_t volume) override;
  int32_t MicrophoneVolume(uint32_t *volume) const override;
  int32_t MaxMicrophoneVolume(uint32_t *maxVolume) const override;
  int32_t MinMicrophoneVolume(uint32_t *minVolume) const override;

  // Microphone mute control
  int32_t MicrophoneMuteIsAvailable(bool *available) override;
  int32_t SetMicrophoneMute(bool enable) override;
  int32_t MicrophoneMute(bool *enabled) const override;

  // Speaker mute control
  int32_t SpeakerMuteIsAvailable(bool *available) override;
  int32_t SetSpeakerMute(bool enable) override;
  int32_t SpeakerMute(bool *enabled) const override;

  // Stereo support
  int32_t StereoPlayoutIsAvailable(bool *available) const override;
  int32_t SetStereoPlayout(bool enable) override;
  int32_t StereoPlayout(bool *enabled) const override;
  int32_t StereoRecordingIsAvailable(bool *available) const override;
  int32_t SetStereoRecording(bool enable) override;
  int32_t StereoRecording(bool *enabled) const override;

  // Delay information and control
  int32_t PlayoutDelay(uint16_t *delayMS) const override;

  // Only supported on Android.
  bool BuiltInAECIsAvailable() const override;
  bool BuiltInAGCIsAvailable() const override;
  bool BuiltInNSIsAvailable() const override;

  // Enables the built-in audio effects. Only supported on Android.
  int32_t EnableBuiltInAEC(bool enable) override;
  int32_t EnableBuiltInAGC(bool enable) override;
  int32_t EnableBuiltInNS(bool enable) override;

 private:
  struct Data;

  // Runs on the sound thread and waits for it.
  void sync(const std::function<void()> &callback);

  void openRecordingDevice();
  void openPlayoutDevice();
  void closeRecordingDevice();

  // NB! stopPlayingOnThread should be called before this,
  // to clear the thread local context and event callback.
  void closePlayoutDevice();

  int restartPlayout();
  int restartRecording();
  void restartRecordingQueued();

  void ensureThreadStarted();
  void stopThreadIfIdle();
  void startCaptureOnThread();
  void stopCaptureOnThread();
  void startPlayingOnThread();

  // NB! closePlayoutDevice should be called after this, so that next time
  // we start playing, we set the thread local context and event callback.
  void stopPlayingOnThread();

  void processData();
  void processRecordingData();
  void processPlayoutData();
  bool processRecordedPart(bool firstInCycle);

  void clearProcessedBuffers();
  bool clearProcessedBuffer();
  void unqueueAllBuffers();

  void handleEvent(ALenum eventType);

  [[nodiscard]] int64_t countExactQueuedMsForLatency(int64_t now, bool playing);

  rtc::Thread *_thread = nullptr;
  webrtc::AudioDeviceBuffer _audioDeviceBuffer;
  std::shared_ptr<Data> _data;

  mutable std::mutex _deviceIdsMutex;
  std::string _playoutDeviceId;
  std::string _recordingDeviceId;
  int _fixedPlayoutQueue = 0;

  ALCdevice *_playoutDevice = nullptr;
  ALCcontext *_playoutContext = nullptr;
  int64_t _playoutLatency = 0;
  int _playoutChannels = 2;
  bool _playoutInitialized = false;
  bool _playoutFailed = false;

  ALCdevice *_recordingDevice = nullptr;
  int64_t _recordingLatency = 0;
  bool _recordingInitialized = false;
  bool _recordingFailed = false;

  bool _speakerInitialized = false;
  bool _microphoneInitialized = false;
  bool _initialized = false;

  std::atomic<int64_t> _underruns = 0;
  std::atomic<int64_t> _buffersPlayed = 0;
  std::atomic<int64_t> _buffersRecorded = 0;
  std::atomic<int> _keepReadyStat = 0;
  std::atomic<int> _largestTake = 0;
  std::atomic<int> _latencyStat = 0;
};

}  // namespace media
