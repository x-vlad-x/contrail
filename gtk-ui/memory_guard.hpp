#pragma once

#include "memory_pressure.hpp"

#include <glib.h>

#include <cstdint>
#include <functional>
#include <string>

namespace Contrail {

// Looks at the system's memory every 15 seconds while the window is open, every minute in the background, and
// asks to free what can be freed when it gets short: as soon as the level rises, then again every minute while it
// stays (what was freed may have filled up again).
class MemoryGuard final {
 public:
  explicit MemoryGuard(std::function<void(MemoryLevel)> release);
  ~MemoryGuard();
  MemoryGuard(const MemoryGuard &) = delete;
  MemoryGuard &operator=(const MemoryGuard &) = delete;

  void setForeground(bool foreground);

  // For the debug actions: the state now, and a level as if the system were that short of memory.
  [[nodiscard]] std::string debugState() const;
  void debugSimulate(MemoryLevel level);

 private:
  void schedule();
  void check();
  void apply(MemoryLevel level);

  const std::function<void(MemoryLevel)> _release;
  guint _timer = 0;
  MemoryLevel _level = MemoryLevel::Normal;
  std::int64_t _released = 0;  // g_get_monotonic_time() of the last release, 0 before the first.
  bool _foreground = false;
};

}  // namespace Contrail
