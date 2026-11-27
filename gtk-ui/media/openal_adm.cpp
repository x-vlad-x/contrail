// See openal_adm.hpp for where this comes from.
#include "media/openal_adm.hpp"

#include <rtc_base/logging.h>
#include <rtc_base/thread.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <thread>
#include <vector>

namespace media {
namespace {

constexpr auto kRecordingFrequency = 48000;
constexpr auto kPlayoutFrequency = 48000;
constexpr auto kRecordingChannels = 1;
constexpr auto kBufferSizeMs = 10;
constexpr auto kPlayoutPart = (kPlayoutFrequency * kBufferSizeMs + 999) / 1000;
constexpr auto kRecordingPart = (kRecordingFrequency * kBufferSizeMs + 999) / 1000;
constexpr auto kRestartAfterEmptyData = 50;  // Half a second with no data.
constexpr auto kProcessInterval = std::chrono::milliseconds(10);

constexpr auto kBuffersFullCount = 16;
constexpr auto kBuffersKeepReadyCount = 5;

// Some sound servers take much more than 10 ms at once: PulseAudio on the FuriLabs FLX1s takes 40-60 ms every
// ~45 ms, emptying a 50 ms queue each time, and every empty queue is a click. The queue grows to what was
// taken in one pass plus this margin for a late timer.
constexpr auto kBuffersKeepReadyMargin = 4;

constexpr auto kDefaultRecordingLatency = int64_t(20);
constexpr auto kDefaultPlayoutLatency = int64_t(20);
constexpr auto kQueryExactTimeEach = 20;

constexpr auto kALMaxValues = 6;
auto kAL_EVENT_TYPE_DISCONNECTED_SOFT = ALenum();
auto kAL_SAMPLE_OFFSET_CLOCK_SOFT = ALenum();
auto kAL_SAMPLE_OFFSET_CLOCK_EXACT_SOFT = ALenum();

using AL_INT64_TYPE = std::int64_t;

using ALEVENTPROCSOFT = void (*)(ALenum eventType, ALuint object, ALuint param, ALsizei length,
                                 const ALchar *message, void *userParam);
using ALEVENTCALLBACKSOFT = void (*)(ALEVENTPROCSOFT callback, void *userParam);
using ALCSETTHREADCONTEXT = ALCboolean (*)(ALCcontext *context);
using ALGETSOURCEI64VSOFT = void (*)(ALuint source, ALenum param, AL_INT64_TYPE *values);

ALEVENTCALLBACKSOFT alEventCallbackSOFTPtr /* = nullptr*/;
ALCSETTHREADCONTEXT alcSetThreadContextPtr /* = nullptr*/;
ALGETSOURCEI64VSOFT alGetSourcei64vSOFTPtr /* = nullptr*/;

[[nodiscard]] int64_t NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

[[nodiscard]] bool Failed(ALCdevice *device) {
  if (auto code = alcGetError(device); code != ALC_NO_ERROR) {
    RTC_LOG(LS_ERROR) << "OpenAL Error " << code << ": " << (const char *)alcGetString(device, code);
    return true;
  }
  return false;
}

template <typename Callback>
void EnumerateDevices(ALCenum specifier, Callback &&callback) {
  auto devices = alcGetString(nullptr, specifier);
  if (!devices) return;
  while (*devices != 0) {
    callback(devices);
    while (*devices != 0) ++devices;
    ++devices;
  }
}

[[nodiscard]] int DevicesCount(ALCenum specifier) {
  auto result = 0;
  EnumerateDevices(specifier, [&](const char *) { ++result; });
  return result;
}

[[nodiscard]] int DeviceName(ALCenum specifier, int index, std::string *name, std::string *guid) {
  EnumerateDevices(specifier, [&](const char *device) {
    if (index < 0) {
      return;
    } else if (index > 0) {
      --index;
      return;
    }

    auto string = std::string(device);
    if (name) {
      if (guid) *guid = string;
      const auto prefix = std::string("OpenAL Soft on ");
      if (string.rfind(prefix, 0) == 0) string = string.substr(prefix.size());
      *name = std::move(string);
    } else if (guid) {
      *guid = std::move(string);
    }
    index = -1;
  });
  return (index > 0) ? -1 : 0;
}

void SetStringToArray(const std::string &string, char *array, int size) {
  const auto length = std::min(int(string.size()), size - 1);
  if (length > 0) memcpy(array, string.data(), length);
  array[length] = 0;
}

[[nodiscard]] int DeviceName(ALCenum specifier, int index, char name[webrtc::kAdmMaxDeviceNameSize],
                             char guid[webrtc::kAdmMaxGuidSize]) {
  auto sname = std::string();
  auto sguid = std::string();
  const auto result = DeviceName(specifier, index, &sname, &sguid);
  if (result) return result;
  SetStringToArray(sname, name, webrtc::kAdmMaxDeviceNameSize);
  SetStringToArray(sguid, guid, webrtc::kAdmMaxGuidSize);
  return 0;
}

}  // namespace

// A thread with a queue of tasks and a 10 ms tick, only while something plays or records.
struct OpenAlAudioDevice::Data {
  Data() = default;
  ~Data() { assert(!thread.joinable()); }

  std::thread thread;
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::function<void()>> tasks;
  bool quit = false;
  bool ticking = false;  // Guarded by mutex; set on the sound thread.

  std::vector<char> recordedSamples;
  int emptyRecordingData = 0;
  bool recording = false;

  std::vector<char> playoutSamples;
  ALuint source = 0;
  int queuedBuffersCount = 0;
  int keepReadyCount = kBuffersKeepReadyCount;
  std::array<ALuint, kBuffersFullCount> buffers = {{0}};
  std::array<bool, kBuffersFullCount> queuedBuffers = {{false}};
  int64_t exactDeviceTimeCounter = 0;
  int64_t lastExactDeviceTime = 0;
  int64_t lastExactDeviceTimeWhen = 0;
  bool playing = false;

  void post(std::function<void()> task) {
    {
      auto lock = std::lock_guard(mutex);
      tasks.push_back(std::move(task));
    }
    wake.notify_one();
  }

  void setTicking(bool value) {
    auto lock = std::lock_guard(mutex);
    ticking = value;
  }

  [[nodiscard]] bool isTicking() {
    auto lock = std::lock_guard(mutex);
    return ticking;
  }

  void run(const std::function<void()> &tick) {
    auto next = std::chrono::steady_clock::now() + kProcessInterval;
    auto lock = std::unique_lock(mutex);
    while (true) {
      const auto ready = [&] { return quit || !tasks.empty(); };
      if (ticking) {
        wake.wait_until(lock, next, ready);
      } else {
        wake.wait(lock, ready);
        next = std::chrono::steady_clock::now();
      }
      if (!tasks.empty()) {
        auto task = std::move(tasks.front());
        tasks.pop_front();
        lock.unlock();
        task();
        lock.lock();
        continue;
      }
      if (quit) return;
      if (ticking && std::chrono::steady_clock::now() >= next) {
        lock.unlock();
        tick();
        lock.lock();
        // A late tick doesn't try to catch up, as a coarse timer wouldn't either.
        next = std::max(next + kProcessInterval, std::chrono::steady_clock::now());
      }
    }
  }
};

void OpenAlAudioDevice::sync(const std::function<void()> &callback) {
  assert(_data != nullptr);
  auto done = std::promise<void>();
  _data->post([&] {
    callback();
    done.set_value();
  });
  done.get_future().wait();
}

OpenAlAudioDevice::OpenAlAudioDevice(webrtc::TaskQueueFactory *taskQueueFactory)
    : _audioDeviceBuffer(taskQueueFactory) {
  _audioDeviceBuffer.SetRecordingSampleRate(kRecordingFrequency);
  _audioDeviceBuffer.SetRecordingChannels(kRecordingChannels);
}

OpenAlAudioDevice::~OpenAlAudioDevice() { Terminate(); }

void OpenAlAudioDevice::setPlayoutDeviceId(std::string id) {
  auto lock = std::lock_guard(_deviceIdsMutex);
  _playoutDeviceId = std::move(id);
}

void OpenAlAudioDevice::setRecordingDeviceId(std::string id) {
  auto lock = std::lock_guard(_deviceIdsMutex);
  _recordingDeviceId = std::move(id);
}

void OpenAlAudioDevice::setFixedPlayoutQueue(int buffers) { _fixedPlayoutQueue = buffers; }

OpenAlAudioDevice::Stats OpenAlAudioDevice::stats() const {
  return {
      .underruns = _underruns.load(),
      .buffersPlayed = _buffersPlayed.load(),
      .keepReadyCount = _keepReadyStat.load(),
      .largestTake = _largestTake.load(),
      .buffersRecorded = _buffersRecorded.load(),
      .playoutLatencyMs = _latencyStat.load(),
  };
}

int32_t OpenAlAudioDevice::ActiveAudioLayer(AudioLayer *audioLayer) const {
  *audioLayer = kPlatformDefaultAudio;
  return 0;
}

int32_t OpenAlAudioDevice::RegisterAudioCallback(webrtc::AudioTransport *audioCallback) {
  return _audioDeviceBuffer.RegisterAudioCallback(audioCallback);
}

int32_t OpenAlAudioDevice::Init() {
  if (_initialized) return 0;
  alcSetThreadContextPtr = (ALCSETTHREADCONTEXT)alcGetProcAddress(nullptr, "alcSetThreadContext");
  if (!alcSetThreadContextPtr) return -1;
  alEventCallbackSOFTPtr = (ALEVENTCALLBACKSOFT)alGetProcAddress("alEventCallbackSOFT");
  alGetSourcei64vSOFTPtr = (ALGETSOURCEI64VSOFT)alGetProcAddress("alGetSourcei64vSOFT");

#define RESOLVE_AL_ENUM(ENUM) k##ENUM = alGetEnumValue(#ENUM)
  RESOLVE_AL_ENUM(AL_EVENT_TYPE_DISCONNECTED_SOFT);
  RESOLVE_AL_ENUM(AL_SAMPLE_OFFSET_CLOCK_SOFT);
  RESOLVE_AL_ENUM(AL_SAMPLE_OFFSET_CLOCK_EXACT_SOFT);
#undef RESOLVE_AL_ENUM

  _initialized = true;
  return 0;
}

int32_t OpenAlAudioDevice::Terminate() {
  StopRecording();
  StopPlayout();
  _initialized = false;
  assert(!_data);
  return 0;
}

bool OpenAlAudioDevice::Initialized() const { return _initialized; }

int32_t OpenAlAudioDevice::InitSpeaker() {
  _speakerInitialized = true;
  return 0;
}

int32_t OpenAlAudioDevice::InitMicrophone() {
  _microphoneInitialized = true;
  return 0;
}

bool OpenAlAudioDevice::SpeakerIsInitialized() const { return _speakerInitialized; }

bool OpenAlAudioDevice::MicrophoneIsInitialized() const { return _microphoneInitialized; }

namespace {

int32_t Unavailable(bool *available) {
  if (available) *available = false;
  return 0;
}

}  // namespace

int32_t OpenAlAudioDevice::SpeakerVolumeIsAvailable(bool *available) { return Unavailable(available); }
int32_t OpenAlAudioDevice::SetSpeakerVolume(uint32_t) { return -1; }
int32_t OpenAlAudioDevice::SpeakerVolume(uint32_t *) const { return -1; }
int32_t OpenAlAudioDevice::MaxSpeakerVolume(uint32_t *) const { return -1; }
int32_t OpenAlAudioDevice::MinSpeakerVolume(uint32_t *) const { return -1; }
int32_t OpenAlAudioDevice::SpeakerMuteIsAvailable(bool *available) { return Unavailable(available); }
int32_t OpenAlAudioDevice::SetSpeakerMute(bool) { return -1; }
int32_t OpenAlAudioDevice::SpeakerMute(bool *enabled) const { return Unavailable(enabled); }
int32_t OpenAlAudioDevice::MicrophoneMuteIsAvailable(bool *available) { return Unavailable(available); }
int32_t OpenAlAudioDevice::SetMicrophoneMute(bool) { return -1; }
int32_t OpenAlAudioDevice::MicrophoneMute(bool *enabled) const { return Unavailable(enabled); }
int32_t OpenAlAudioDevice::StereoRecordingIsAvailable(bool *available) const { return Unavailable(available); }
int32_t OpenAlAudioDevice::SetStereoRecording(bool) { return -1; }
int32_t OpenAlAudioDevice::StereoRecording(bool *enabled) const { return Unavailable(enabled); }
int32_t OpenAlAudioDevice::MicrophoneVolumeIsAvailable(bool *available) { return Unavailable(available); }
int32_t OpenAlAudioDevice::SetMicrophoneVolume(uint32_t) { return -1; }
int32_t OpenAlAudioDevice::MicrophoneVolume(uint32_t *) const { return -1; }
int32_t OpenAlAudioDevice::MaxMicrophoneVolume(uint32_t *) const { return -1; }
int32_t OpenAlAudioDevice::MinMicrophoneVolume(uint32_t *) const { return -1; }

int32_t OpenAlAudioDevice::StereoPlayoutIsAvailable(bool *available) const {
  if (available) *available = true;
  return 0;
}

int32_t OpenAlAudioDevice::SetStereoPlayout(bool enable) {
  if (Playing()) return -1;
  _playoutChannels = enable ? 2 : 1;
  return 0;
}

int32_t OpenAlAudioDevice::StereoPlayout(bool *enabled) const {
  if (enabled) *enabled = (_playoutChannels == 2);
  return 0;
}

int16_t OpenAlAudioDevice::PlayoutDevices() { return DevicesCount(ALC_ALL_DEVICES_SPECIFIER); }

int32_t OpenAlAudioDevice::SetPlayoutDevice(uint16_t) { return restartPlayout(); }

int32_t OpenAlAudioDevice::SetPlayoutDevice(WindowsDeviceType) { return restartPlayout(); }

int32_t OpenAlAudioDevice::PlayoutDeviceName(uint16_t index, char name[webrtc::kAdmMaxDeviceNameSize],
                                             char guid[webrtc::kAdmMaxGuidSize]) {
  return DeviceName(ALC_ALL_DEVICES_SPECIFIER, index, name, guid);
}

int32_t OpenAlAudioDevice::RecordingDeviceName(uint16_t index, char name[webrtc::kAdmMaxDeviceNameSize],
                                               char guid[webrtc::kAdmMaxGuidSize]) {
  return DeviceName(ALC_CAPTURE_DEVICE_SPECIFIER, index, name, guid);
}

int16_t OpenAlAudioDevice::RecordingDevices() { return DevicesCount(ALC_CAPTURE_DEVICE_SPECIFIER); }

int32_t OpenAlAudioDevice::SetRecordingDevice(uint16_t) { return restartRecording(); }

int32_t OpenAlAudioDevice::SetRecordingDevice(WindowsDeviceType) { return restartRecording(); }

int32_t OpenAlAudioDevice::PlayoutIsAvailable(bool *available) {
  if (available) *available = true;
  return 0;
}

int32_t OpenAlAudioDevice::RecordingIsAvailable(bool *available) {
  if (available) *available = true;
  return 0;
}

int32_t OpenAlAudioDevice::InitPlayout() {
  if (!_initialized) {
    return -1;
  } else if (_playoutInitialized) {
    return 0;
  }
  _playoutInitialized = true;
  ensureThreadStarted();
  openPlayoutDevice();
  return 0;
}

void OpenAlAudioDevice::openRecordingDevice() {
  if (_recordingDevice || _recordingFailed) return;
  auto id = std::string();
  {
    auto lock = std::lock_guard(_deviceIdsMutex);
    id = _recordingDeviceId;
  }
  _recordingDevice = alcCaptureOpenDevice(id.empty() ? nullptr : id.c_str(), kRecordingFrequency, AL_FORMAT_MONO16,
                                          kRecordingFrequency / 4);
  if (!_recordingDevice) {
    RTC_LOG(LS_ERROR) << "OpenAL Capture Device open failed, deviceID: '" << id << "'";
    _recordingFailed = true;
    return;
  }
  _audioDeviceBuffer.SetRecordingChannels(kRecordingChannels);
}

void OpenAlAudioDevice::openPlayoutDevice() {
  if (_playoutDevice || _playoutFailed) return;
  auto id = std::string();
  {
    auto lock = std::lock_guard(_deviceIdsMutex);
    id = _playoutDeviceId;
  }
  _playoutDevice = alcOpenDevice(id.empty() ? nullptr : id.c_str());
  if (!_playoutDevice) {
    RTC_LOG(LS_ERROR) << "OpenAL Device open failed, deviceID: '" << id << "'";
    _playoutFailed = true;
    return;
  }
  _playoutContext = alcCreateContext(_playoutDevice, nullptr);
  if (!_playoutContext) {
    RTC_LOG(LS_ERROR) << "OpenAL Context create failed.";
    _playoutFailed = true;
    closePlayoutDevice();
    return;
  }
  sync([&] {
    alcSetThreadContextPtr(_playoutContext);
    if (alEventCallbackSOFTPtr) {
      alEventCallbackSOFTPtr([](ALenum eventType, ALuint, ALuint, ALsizei, const ALchar *,
                                void *that) { static_cast<OpenAlAudioDevice *>(that)->handleEvent(eventType); },
                             this);
    }
  });
}

void OpenAlAudioDevice::handleEvent(ALenum eventType) {
  if (eventType == kAL_EVENT_TYPE_DISCONNECTED_SOFT && _thread) {
    _thread->PostTask([this, weak = std::weak_ptr<Data>(_data)] {
      if (weak.lock()) restartRecording();
    });
  }
}

int32_t OpenAlAudioDevice::InitRecording() {
  if (!_initialized) {
    return -1;
  } else if (_recordingInitialized) {
    return 0;
  }
  _recordingInitialized = true;
  ensureThreadStarted();
  openRecordingDevice();
  _audioDeviceBuffer.SetRecordingSampleRate(kRecordingFrequency);
  _audioDeviceBuffer.SetRecordingChannels(kRecordingChannels);
  return 0;
}

void OpenAlAudioDevice::ensureThreadStarted() {
  if (_data) return;
  _thread = rtc::Thread::Current();
  if (_thread && !_thread->IsOwned()) {
    _thread->UnwrapCurrent();
    _thread = nullptr;
  }

  _data = std::make_shared<Data>();
  _data->thread = std::thread([this, data = _data.get()] { data->run([this] { processData(); }); });
  pthread_setname_np(_data->thread.native_handle(), "contrail-sound");
}

void OpenAlAudioDevice::stopThreadIfIdle() {
  if (!_data || _data->recording || _data->playing) return;
  {
    auto lock = std::lock_guard(_data->mutex);
    _data->quit = true;
  }
  _data->wake.notify_one();
  _data->thread.join();
  _data = nullptr;
}

void OpenAlAudioDevice::processData() {
  assert(_data != nullptr);
  if (_data->playing && !_playoutFailed) processPlayoutData();
  if (_data->recording && !_recordingFailed) processRecordingData();
}

bool OpenAlAudioDevice::processRecordedPart(bool firstInCycle) {
  auto samples = ALint();
  alcGetIntegerv(_recordingDevice, ALC_CAPTURE_SAMPLES, 1, &samples);
  if (Failed(_recordingDevice)) {
    restartRecordingQueued();
    return false;
  }
  if (samples <= 0) {
    if (firstInCycle) {
      ++_data->emptyRecordingData;
      if (_data->emptyRecordingData == kRestartAfterEmptyData) restartRecordingQueued();
    }
    return false;
  } else if (samples < kRecordingPart) {
    // Not enough data for 10ms.
    return false;
  }

  _recordingLatency = kDefaultRecordingLatency;

  _data->emptyRecordingData = 0;
  const auto recordingBufferSize = kRecordingPart * int(sizeof(int16_t)) * kRecordingChannels;
  if (int(_data->recordedSamples.size()) < recordingBufferSize) _data->recordedSamples.resize(recordingBufferSize);
  alcCaptureSamples(_recordingDevice, _data->recordedSamples.data(), kRecordingPart);
  if (Failed(_recordingDevice)) {
    restartRecordingQueued();
    return false;
  }
  _audioDeviceBuffer.SetRecordedBuffer(_data->recordedSamples.data(), kRecordingPart);
  _audioDeviceBuffer.SetVQEData(_playoutLatency, _recordingLatency);
  _audioDeviceBuffer.DeliverRecordedData();
  ++_buffersRecorded;
  return true;
}

void OpenAlAudioDevice::processRecordingData() {
  for (auto first = true; processRecordedPart(first); first = false) {
  }
}

bool OpenAlAudioDevice::clearProcessedBuffer() {
  assert(_data != nullptr);

  auto processed = ALint(0);
  alGetSourcei(_data->source, AL_BUFFERS_PROCESSED, &processed);
  if (processed < 1) return false;
  auto buffer = ALuint(0);
  alSourceUnqueueBuffers(_data->source, 1, &buffer);
  for (auto i = 0; i != int(_data->buffers.size()); ++i) {
    if (_data->buffers[i] == buffer) {
      _data->queuedBuffers[i] = false;
      --_data->queuedBuffersCount;
      return true;
    }
  }
  RTC_LOG(LS_ERROR) << "OpenAL: a processed buffer is not ours.";
  return false;
}

void OpenAlAudioDevice::unqueueAllBuffers() {
  alSourcei(_data->source, AL_BUFFER, AL_NONE);
  std::fill(_data->queuedBuffers.begin(), _data->queuedBuffers.end(), false);
  _data->queuedBuffersCount = 0;
}

void OpenAlAudioDevice::clearProcessedBuffers() {
  while (clearProcessedBuffer()) {
  }
}

int64_t OpenAlAudioDevice::countExactQueuedMsForLatency(int64_t now, bool playing) {
  auto values = std::array<AL_INT64_TYPE, kALMaxValues>{};
  auto &sampleOffset = values[0];
  auto &clockTime = values[1];
  auto &exactDeviceTime = values[2];
  const auto countExact =
      alGetSourcei64vSOFTPtr && kAL_SAMPLE_OFFSET_CLOCK_SOFT && kAL_SAMPLE_OFFSET_CLOCK_EXACT_SOFT;
  if (countExact) {
    if (!_data->lastExactDeviceTimeWhen || !(++_data->exactDeviceTimeCounter % kQueryExactTimeEach)) {
      alGetSourcei64vSOFTPtr(_data->source, kAL_SAMPLE_OFFSET_CLOCK_EXACT_SOFT, values.data());
      _data->lastExactDeviceTime = exactDeviceTime;
      _data->lastExactDeviceTimeWhen = now;
    } else {
      alGetSourcei64vSOFTPtr(_data->source, kAL_SAMPLE_OFFSET_CLOCK_SOFT, values.data());

      // The exactDeviceTime is in nanoseconds.
      exactDeviceTime = _data->lastExactDeviceTime + (now - _data->lastExactDeviceTimeWhen) * 1'000'000;
    }
  } else {
    auto offset = ALint(0);
    alGetSourcei(_data->source, AL_SAMPLE_OFFSET, &offset);
    sampleOffset = (AL_INT64_TYPE(offset) << 32);
  }

  const auto queuedSamples = (AL_INT64_TYPE(_data->queuedBuffersCount * kPlayoutPart) << 32);
  const auto processedInOpenAL = playing ? sampleOffset : queuedSamples;
  const auto secondsQueuedInDevice = std::max(clockTime - exactDeviceTime, AL_INT64_TYPE(0)) / 1'000'000'000.;
  const auto secondsQueuedInOpenAL =
      (double((queuedSamples - processedInOpenAL) >> (32 - 10)) / double(kPlayoutFrequency * (1 << 10)));

  const auto queuedTotal = int64_t(std::round((secondsQueuedInDevice + secondsQueuedInOpenAL) * 1'000));

  return countExact ? queuedTotal : std::max(queuedTotal, kDefaultPlayoutLatency);
}

void OpenAlAudioDevice::processPlayoutData() {
  assert(_data != nullptr);

  const auto playing = [&] {
    auto state = ALint(AL_INITIAL);
    alGetSourcei(_data->source, AL_SOURCE_STATE, &state);
    return (state == AL_PLAYING);
  };
  const auto wasPlaying = playing();

  const auto queuedBefore = _data->queuedBuffersCount;
  const auto taken = [&](int count) {
    _largestTake = std::max(_largestTake.load(), count);
    if (_fixedPlayoutQueue) return;
    _data->keepReadyCount = std::clamp(count + kBuffersKeepReadyMargin, _data->keepReadyCount, kBuffersFullCount);
    _keepReadyStat = _data->keepReadyCount;
  };
  if (wasPlaying) {
    clearProcessedBuffers();
    taken(queuedBefore - _data->queuedBuffersCount);
  } else {
    if (queuedBefore) {
      // The source played everything we gave it and stopped.
      ++_underruns;
      taken(_data->keepReadyCount);
    }
    unqueueAllBuffers();
  }

  const auto wereQueued = _data->queuedBuffers;
  while (_data->queuedBuffersCount < _data->keepReadyCount) {
    const auto available = _audioDeviceBuffer.RequestPlayoutData(kPlayoutPart);
    if (available == kPlayoutPart) {
      _audioDeviceBuffer.GetPlayoutData(_data->playoutSamples.data());
    } else {
      break;
    }
    const auto now = NowMs();
    _playoutLatency = countExactQueuedMsForLatency(now, wasPlaying);
    _latencyStat = int(_playoutLatency);

    const auto i = std::find(_data->queuedBuffers.begin(), _data->queuedBuffers.end(), false);
    assert(i != _data->queuedBuffers.end());
    const auto index = int(i - _data->queuedBuffers.begin());
    alBufferData(_data->buffers[index], (_playoutChannels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16,
                 _data->playoutSamples.data(), ALsizei(_data->playoutSamples.size()), kPlayoutFrequency);

    _data->queuedBuffers[index] = true;
    ++_data->queuedBuffersCount;
    ++_buffersPlayed;
    if (wasPlaying) alSourceQueueBuffers(_data->source, 1, _data->buffers.data() + index);
  }
  if (!_data->queuedBuffersCount) return;
  if (!playing()) {
    if (wasPlaying) {
      // While we were queueing buffers the source stopped. Now we can't unqueue only old buffers, so we
      // unqueue all of them and then re-queue the ones we queued right now.
      ++_underruns;
      unqueueAllBuffers();
      for (auto i = 0; i != int(_data->buffers.size()); ++i) {
        if (!wereQueued[i] && _data->queuedBuffers[i]) {
          alSourceQueueBuffers(_data->source, 1, _data->buffers.data() + i);
        }
      }
    } else {
      // We were not playing and had no buffers, so queue them all at once.
      alSourceQueueBuffers(_data->source, _data->queuedBuffersCount, _data->buffers.data());
    }
    alSourcePlay(_data->source);
  }

  if (Failed(_playoutDevice)) _playoutFailed = true;
}

int32_t OpenAlAudioDevice::StartRecording() {
  if (!_recordingInitialized) {
    return -1;
  } else if (_data && _data->recording) {
    return 0;
  }
  if (_recordingFailed) {
    _recordingFailed = false;
    openRecordingDevice();
  }
  _audioDeviceBuffer.StartRecording();
  startCaptureOnThread();
  return 0;
}

void OpenAlAudioDevice::startCaptureOnThread() {
  assert(_data != nullptr);

  sync([&] {
    _data->recording = true;
    if (_recordingFailed) return;
    alcCaptureStart(_recordingDevice);
    if (Failed(_recordingDevice)) {
      _recordingFailed = true;
      return;
    }
    _data->setTicking(true);
  });
  if (_recordingFailed) closeRecordingDevice();
}

void OpenAlAudioDevice::stopCaptureOnThread() {
  assert(_data != nullptr);

  if (!_data->recording) return;
  sync([&] {
    _data->recording = false;
    if (_recordingFailed) return;
    if (!_data->playing) _data->setTicking(false);
    if (_recordingDevice) alcCaptureStop(_recordingDevice);
  });
}

void OpenAlAudioDevice::startPlayingOnThread() {
  assert(_data != nullptr);

  sync([&] {
    _data->playing = true;
    if (_playoutFailed) return;
    ALuint source = 0;
    alGenSources(1, &source);
    if (!source) return;
    alSourcef(source, AL_PITCH, 1.f);
    alSource3f(source, AL_POSITION, 0, 0, 0);
    alSource3f(source, AL_VELOCITY, 0, 0, 0);
    alSourcei(source, AL_LOOPING, 0);
    alSourcei(source, AL_SOURCE_RELATIVE, 1);
    alSourcei(source, AL_ROLLOFF_FACTOR, 0);
    if (alIsExtensionPresent("AL_SOFT_direct_channels_remix")) {
      alSourcei(source, alGetEnumValue("AL_DIRECT_CHANNELS_SOFT"), alGetEnumValue("AL_REMIX_UNMATCHED_SOFT"));
    }
    _data->source = source;
    alGenBuffers(ALsizei(_data->buffers.size()), _data->buffers.data());

    _data->keepReadyCount =
        _fixedPlayoutQueue ? std::min(_fixedPlayoutQueue, kBuffersFullCount) : kBuffersKeepReadyCount;
    _keepReadyStat = _data->keepReadyCount;
    _data->exactDeviceTimeCounter = 0;
    _data->lastExactDeviceTime = 0;
    _data->lastExactDeviceTimeWhen = 0;

    _data->playoutSamples.assign(kPlayoutPart * sizeof(int16_t) * _playoutChannels, 0);

    _data->setTicking(true);
  });
}

void OpenAlAudioDevice::stopPlayingOnThread() {
  assert(_data != nullptr);

  sync([&] {
    const auto clear = [&] {
      if (alEventCallbackSOFTPtr) alEventCallbackSOFTPtr(nullptr, nullptr);
      alcSetThreadContextPtr(nullptr);
    };
    if (!_data->playing) {
      clear();
      return;
    }
    _data->playing = false;
    if (_playoutFailed) {
      clear();
      return;
    }
    if (!_data->recording) _data->setTicking(false);
    if (_data->source) {
      alSourceStop(_data->source);
      unqueueAllBuffers();
      alDeleteBuffers(ALsizei(_data->buffers.size()), _data->buffers.data());
      alDeleteSources(1, &_data->source);
      _data->source = 0;
      _data->buffers.fill(0);
    }
    clear();
  });
}

int32_t OpenAlAudioDevice::StopRecording() {
  if (_data) {
    stopCaptureOnThread();
    _audioDeviceBuffer.StopRecording();
    stopThreadIfIdle();
  }
  closeRecordingDevice();
  _recordingInitialized = false;
  return 0;
}

void OpenAlAudioDevice::restartRecordingQueued() {
  assert(_data != nullptr);

  // Auto-restarting works only when started from an rtc::Thread.
  if (!_thread) return;
  _thread->PostTask([this, weak = std::weak_ptr<Data>(_data)] {
    const auto data = weak.lock();
    if (!data) return;
    restartRecording();
    data->post([data] { data->emptyRecordingData = 0; });
  });
}

int OpenAlAudioDevice::restartRecording() {
  if (!_data || !_data->recording) return 0;
  stopCaptureOnThread();
  closeRecordingDevice();

  _recordingFailed = false;
  openRecordingDevice();
  startCaptureOnThread();
  return 0;
}

int OpenAlAudioDevice::restartPlayout() {
  if (!_data || !_data->playing) return 0;
  stopPlayingOnThread();
  closePlayoutDevice();

  _playoutFailed = false;
  openPlayoutDevice();
  startPlayingOnThread();
  return 0;
}

void OpenAlAudioDevice::closeRecordingDevice() {
  if (_recordingDevice) {
    alcCaptureCloseDevice(_recordingDevice);
    _recordingDevice = nullptr;
  }
}

void OpenAlAudioDevice::closePlayoutDevice() {
  if (_playoutContext) {
    alcDestroyContext(_playoutContext);
    _playoutContext = nullptr;
  }
  if (_playoutDevice) {
    alcCloseDevice(_playoutDevice);
    _playoutDevice = nullptr;
  }
}

bool OpenAlAudioDevice::RecordingIsInitialized() const { return _recordingInitialized; }

bool OpenAlAudioDevice::Recording() const { return _data && _data->recording; }

bool OpenAlAudioDevice::PlayoutIsInitialized() const { return _playoutInitialized; }

int32_t OpenAlAudioDevice::StartPlayout() {
  if (!_playoutInitialized) {
    return -1;
  } else if (Playing()) {
    return 0;
  }
  if (_playoutFailed) {
    _playoutFailed = false;
    openPlayoutDevice();
  }
  _audioDeviceBuffer.SetPlayoutSampleRate(kPlayoutFrequency);
  _audioDeviceBuffer.SetPlayoutChannels(_playoutChannels);
  _audioDeviceBuffer.StartPlayout();
  startPlayingOnThread();
  return 0;
}

int32_t OpenAlAudioDevice::StopPlayout() {
  if (_data) {
    stopPlayingOnThread();
    _audioDeviceBuffer.StopPlayout();
    stopThreadIfIdle();
  }
  closePlayoutDevice();
  _playoutInitialized = false;
  return 0;
}

int32_t OpenAlAudioDevice::PlayoutDelay(uint16_t *delayMS) const {
  if (delayMS) *delayMS = 0;
  return 0;
}

bool OpenAlAudioDevice::BuiltInAECIsAvailable() const { return false; }

bool OpenAlAudioDevice::BuiltInAGCIsAvailable() const { return false; }

bool OpenAlAudioDevice::BuiltInNSIsAvailable() const { return false; }

int32_t OpenAlAudioDevice::EnableBuiltInAEC(bool enable) { return enable ? -1 : 0; }

int32_t OpenAlAudioDevice::EnableBuiltInAGC(bool enable) { return enable ? -1 : 0; }

int32_t OpenAlAudioDevice::EnableBuiltInNS(bool enable) { return enable ? -1 : 0; }

bool OpenAlAudioDevice::Playing() const { return _data && _data->playing; }

}  // namespace media
