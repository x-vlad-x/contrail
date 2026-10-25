#pragma once

// How short of memory the system is, from /proc/meminfo and /proc/pressure/memory: no GTK or Telegram here
// (tests/memory-pressure.cpp).
//
// The phones have no low-memory-monitor (GLib's GMemoryMonitor and the portal's would never warn), and a kernel
// that lets only root wait for pressure: Contrail reads both files from time to time instead.

#include <cstdint>
#include <cstdio>
#include <string>

namespace Contrail {

enum class MemoryLevel {
  Normal,
  Low,       // Free what doesn't show: rows far from the screen, other chats' messages.
  Critical,  // And the whole interface, if it isn't in front.
};

struct MemoryState {
  int64_t totalKb = 0;
  int64_t availableKb = 0;
  double someAvg10 = 0.;  // The share of the last 10 s some tasks waited for memory, percent.
  double fullAvg10 = 0.;  // ...and all of them did.
  bool known = false;
};

struct MemoryThresholds {
  static constexpr auto kLowAvailable = 0.10;       // Of all the memory.
  static constexpr auto kCriticalAvailable = 0.05;
  static constexpr auto kLowSome = 20.;             // Percent of the time waiting.
  static constexpr auto kCriticalFull = 10.;
};

inline MemoryState ParseMemory(const std::string &meminfo, const std::string &pressure) {
  auto result = MemoryState();
  const auto field = [&](const char *name) -> int64_t {
    const auto at = meminfo.find(name);
    if (at == std::string::npos) return -1;
    long long value = -1;
    return (std::sscanf(meminfo.c_str() + at + std::string(name).size(), " %lld", &value) == 1) ? value : -1;
  };
  result.totalKb = field("MemTotal:");
  result.availableKb = field("MemAvailable:");
  result.known = result.totalKb > 0 && result.availableKb >= 0;
  const auto avg10 = [&](const char *line) {
    const auto at = pressure.find(line);
    if (at == std::string::npos) return 0.;
    const auto value = pressure.find("avg10=", at);
    double result = 0.;
    return (value != std::string::npos && std::sscanf(pressure.c_str() + value + 6, "%lf", &result) == 1) ? result : 0.;
  };
  result.someAvg10 = avg10("some ");
  result.fullAvg10 = avg10("full ");
  return result;
}

inline MemoryLevel LevelOf(const MemoryState &state) {
  using T = MemoryThresholds;
  if (!state.known) return MemoryLevel::Normal;
  const auto share = double(state.availableKb) / double(state.totalKb);
  if (share < T::kCriticalAvailable || state.fullAvg10 >= T::kCriticalFull) return MemoryLevel::Critical;
  if (share < T::kLowAvailable || state.someAvg10 >= T::kLowSome) return MemoryLevel::Low;
  return MemoryLevel::Normal;
}

}  // namespace Contrail
