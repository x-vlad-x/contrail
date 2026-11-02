#include "memory_trim.hpp"

#include <glib.h>
#include <malloc.h>

#include <algorithm>
#include <cstdint>

namespace Contrail {
namespace {

// How long nothing must go on before a due trim runs.
constexpr auto kQuietMs = 2000;

guint Timer = 0;
std::int64_t LastActivity = 0;  // g_get_monotonic_time() of the last PostponeMemoryTrim.

void Schedule(int delayMs);

void Fire() {
  Timer = 0;
  const auto quietFor = (g_get_monotonic_time() - LastActivity) / 1000;
  if (quietFor < kQuietMs) {
    Schedule(int(kQuietMs - quietFor));
    return;
  }
  const auto start = g_get_monotonic_time();
  malloc_trim(0);
  g_debug("Memory: trimmed in %.1f ms", (g_get_monotonic_time() - start) / 1000.);
}

void Schedule(int delayMs) {
  if (Timer) g_source_remove(Timer);
  Timer = g_timeout_add_once(guint(std::max(delayMs, 0)), +[](gpointer) { Fire(); }, nullptr);
}

}  // namespace

void TrimMemorySoon(int delayMs) {
  Schedule(delayMs);
}

void PostponeMemoryTrim() {
  LastActivity = g_get_monotonic_time();
}

}  // namespace Contrail
