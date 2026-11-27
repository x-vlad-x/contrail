#pragma once

#include <AL/alc.h>
#include <cstring>
#include <string>

namespace media {
// PulseAudio gives descriptions; older OpenAL Soft versions prefix their actual device ids.
inline std::string OpenAlOutputId(const std::string &description) {
  if (description.empty()) return {};
  const auto *devices = alcGetString(nullptr, ALC_ALL_DEVICES_SPECIFIER);
  if (!devices) return {};
  constexpr auto prefix = "OpenAL Soft on ";
  for (auto *next = devices; *next; next += std::strlen(next) + 1) {
    const auto id = std::string(next);
    const auto name = id.starts_with(prefix) ? id.substr(std::strlen(prefix)) : id;
    if (id == description || name == description) return id;
  }
  return {};  // A device that went away follows the system's default.
}
} // namespace media
