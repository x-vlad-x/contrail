// VoiceRecorder::Waveform: Telegram's 100 values of 5 bits from the loudest samples of every 20 ms, packed lowest bit
// first, as Telegram Desktop packs them (documentWaveformEncode5bit).
#include "media/voice_recorder.hpp"

#include <glib.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Contrail {
namespace {

std::vector<int> Unpack(const std::string &packed) {
  auto result = std::vector<int>();
  for (auto i = 0; i < 100; ++i) {
    const auto bit = i * 5;
    auto value = unsigned(std::uint8_t(packed[bit / 8])) >> (bit % 8);
    if (bit / 8 + 1 < int(packed.size())) value |= unsigned(std::uint8_t(packed[bit / 8 + 1])) << (8 - bit % 8);
    result.push_back(int(value & 31));
  }
  return result;
}

void TestSize() {
  g_assert_cmpuint(VoiceRecorder::Waveform({}).size(), ==, 63);
  g_assert_cmpuint(VoiceRecorder::Waveform(std::vector<std::uint16_t>(500, 9000)).size(), ==, 63);
}

void TestSilence() {
  // Nothing recorded, or near silence: no bars, not noise blown up to full height.
  for (const auto value : Unpack(VoiceRecorder::Waveform({}))) g_assert_cmpint(value, ==, 0);
  for (const auto value : Unpack(VoiceRecorder::Waveform(std::vector<std::uint16_t>(300, 200)))) {
    g_assert_cmpint(value, <=, 3);
  }
}

void TestLoudest() {
  // The loudest moment is full height; each value is the loudest of its part.
  auto peaks = std::vector<std::uint16_t>(1000, 4000);
  peaks[995] = 20000;
  const auto values = Unpack(VoiceRecorder::Waveform(peaks));
  g_assert_cmpint(values[99], ==, 31);
  g_assert_cmpint(values[0], ==, 4000 * 31 / 20000);
  g_assert_cmpint(values[50], ==, values[0]);
}

void TestRamp() {
  // Every value lands in its own place: a ramp packed and unpacked stays a ramp.
  auto peaks = std::vector<std::uint16_t>();
  for (auto i = 0; i < 100; ++i) peaks.push_back(std::uint16_t(i * 600));
  const auto values = Unpack(VoiceRecorder::Waveform(peaks));
  for (auto i = 0; i < 100; ++i) g_assert_cmpint(values[i], ==, i * 31 / 99);
}

void TestShort() {
  // Fewer moments than values (a short message): each one fills its share.
  const auto values = Unpack(VoiceRecorder::Waveform({10000, 0, 10000, 0}));
  g_assert_cmpint(values[0], ==, 31);
  g_assert_cmpint(values[30], ==, 0);
  g_assert_cmpint(values[60], ==, 31);
  g_assert_cmpint(values[99], ==, 0);
}

}  // namespace
}  // namespace Contrail

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, nullptr);
  g_test_add_func("/waveform/size", Contrail::TestSize);
  g_test_add_func("/waveform/silence", Contrail::TestSilence);
  g_test_add_func("/waveform/loudest", Contrail::TestLoudest);
  g_test_add_func("/waveform/ramp", Contrail::TestRamp);
  g_test_add_func("/waveform/short", Contrail::TestShort);
  return g_test_run();
}
