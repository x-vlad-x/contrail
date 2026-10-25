#pragma once

#include "app.hpp"
#include "memory_pressure.hpp"

#include "base/timer.h"

namespace Contrail {

// Looks at the system's memory every 15 seconds while the window is open, every minute in the background, and
// asks to free what can be freed when it gets short: as soon as the level rises, then again every minute while it
// stays (what was freed may have filled up again).
class MemoryGuard final {
public:
  explicit MemoryGuard(Fn<void(MemoryLevel)> release);

  void setForeground(bool foreground);

  // For the debug actions: the state now, and a level as if the system were that short of memory.
  [[nodiscard]] QString debugState() const;
  void debugSimulate(MemoryLevel level);

private:
  void check();
  void apply(MemoryLevel level);

  const Fn<void(MemoryLevel)> _release;
  base::Timer _timer;
  MemoryLevel _level = MemoryLevel::Normal;
  crl::time _released = 0;
  bool _foreground = false;
};

}  // namespace Contrail
