#include "memory_guard.hpp"

#include <fstream>
#include <sstream>

namespace Contrail {
namespace {

constexpr auto kForegroundIntervalS = 15;
constexpr auto kBackgroundIntervalS = 60;
constexpr auto kRepeatIntervalUs = std::int64_t(60) * G_USEC_PER_SEC;  // While the level stays where it is.

std::string ReadFile(const char *path) {
  auto file = std::ifstream(path);
  auto result = std::ostringstream();
  result << file.rdbuf();
  return result.str();
}

MemoryState ReadState() {
  return ParseMemory(ReadFile("/proc/meminfo"), ReadFile("/proc/pressure/memory"));
}

const char *LevelName(MemoryLevel level) {
  switch (level) {
    case MemoryLevel::Low: return "low";
    case MemoryLevel::Critical: return "critical";
    default: return "normal";
  }
}

}  // namespace

MemoryGuard::MemoryGuard(std::function<void(MemoryLevel)> release) : _release(std::move(release)) {
  schedule();
}

MemoryGuard::~MemoryGuard() {
  if (_timer) g_source_remove(_timer);
}

void MemoryGuard::setForeground(bool foreground) {
  if (_foreground == foreground) return;
  _foreground = foreground;
  schedule();
}

// In whole seconds: GLib wakes the phone once for this and its other second timers.
void MemoryGuard::schedule() {
  if (_timer) g_source_remove(_timer);
  _timer = g_timeout_add_seconds(_foreground ? kForegroundIntervalS : kBackgroundIntervalS, +[](gpointer data) {
    static_cast<MemoryGuard *>(data)->check();
    return gboolean(G_SOURCE_CONTINUE);
  }, this);
}

void MemoryGuard::check() {
  apply(LevelOf(ReadState()));
}

void MemoryGuard::apply(MemoryLevel level) {
  const auto rose = (level > _level);
  _level = level;
  if (level == MemoryLevel::Normal) return;
  const auto now = g_get_monotonic_time();
  if (!rose && _released && now - _released < kRepeatIntervalUs) return;
  _released = now;
  const auto state = ReadState();
  g_message("memory %s, %lld MB available of %lld, pressure %.2f/%.2f; freeing what can be freed.", LevelName(level),
            static_cast<long long>(state.availableKb / 1024), static_cast<long long>(state.totalKb / 1024),
            state.someAvg10, state.fullAvg10);
  _release(level);
}

std::string MemoryGuard::debugState() const {
  const auto state = ReadState();
  char buffer[160];
  std::snprintf(buffer, sizeof(buffer), "level=%s now=%s available=%lld/%lld MB some=%.2f full=%.2f foreground=%d",
                LevelName(_level), LevelName(LevelOf(state)), static_cast<long long>(state.availableKb / 1024),
                static_cast<long long>(state.totalKb / 1024), state.someAvg10, state.fullAvg10, int(_foreground));
  return buffer;
}

void MemoryGuard::debugSimulate(MemoryLevel level) {
  _level = MemoryLevel::Normal;
  _released = 0;
  apply(level);
}

}  // namespace Contrail
