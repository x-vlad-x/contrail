#pragma once
#include "call_tones.hpp"
#include <cstdint>
#include <vector>
namespace Contrail::Calls {
// Stereo PCM for OpenAL. The original Telegram sounds are embedded as MP3, decoded only when needed.
[[nodiscard]] std::vector<std::int16_t> DecodeCallTone(Tone tone);
inline constexpr int kToneSampleRate = 48000;
} // namespace Contrail::Calls
