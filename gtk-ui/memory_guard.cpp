#include "memory_guard.hpp"

#include "logs.h"

#include <fstream>
#include <sstream>

namespace Contrail {
namespace {

constexpr auto kForegroundInterval = 15 * crl::time(1000);
constexpr auto kBackgroundInterval = 60 * crl::time(1000);
constexpr auto kRepeatInterval = 60 * crl::time(1000);  // While the level stays where it is.

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

MemoryGuard::MemoryGuard(Fn<void(MemoryLevel)> release) : _release(std::move(release)), _timer([=] { check(); }) {
  _timer.callEach(kBackgroundInterval);
}

void MemoryGuard::setForeground(bool foreground) {
  if (_foreground == foreground) return;
  _foreground = foreground;
  _timer.callEach(foreground ? kForegroundInterval : kBackgroundInterval);
}

void MemoryGuard::check() {
  apply(LevelOf(ReadState()));
}

void MemoryGuard::apply(MemoryLevel level) {
  const auto rose = (level > _level);
  _level = level;
  if (level == MemoryLevel::Normal) return;
  const auto now = crl::now();
  if (!rose && now - _released < kRepeatInterval) return;
  _released = now;
  const auto state = ReadState();
  LOG(("Contrail: memory %1, %2 MB available of %3, pressure %4/%5; freeing what can be freed.")
          .arg(LevelName(level))
          .arg(state.availableKb / 1024)
          .arg(state.totalKb / 1024)
          .arg(state.someAvg10)
          .arg(state.fullAvg10));
  _release(level);
}

QString MemoryGuard::debugState() const {
  const auto state = ReadState();
  return u"level=%1 now=%2 available=%3/%4 MB some=%5 full=%6"_q.arg(LevelName(_level))
      .arg(LevelName(LevelOf(state)))
      .arg(state.availableKb / 1024)
      .arg(state.totalKb / 1024)
      .arg(state.someAvg10)
      .arg(state.fullAvg10);
}

void MemoryGuard::debugSimulate(MemoryLevel level) {
  _level = MemoryLevel::Normal;
  apply(level);
}

}  // namespace Contrail
