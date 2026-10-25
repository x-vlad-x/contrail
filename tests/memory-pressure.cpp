// How short of memory the system is (gtk-ui/memory_pressure.hpp): parsing /proc and the levels.
#include "memory_pressure.hpp"

#include <cassert>
#include <cstdio>

using namespace Contrail;

namespace {

std::string Meminfo(long long total, long long available) {
  return "MemTotal:       " + std::to_string(total) + " kB\nMemFree:          282000 kB\nMemAvailable:   "
         + std::to_string(available) + " kB\nBuffers:           1000 kB\n";
}

std::string Pressure(double some, double full) {
  char buffer[256];
  std::snprintf(buffer, sizeof(buffer),
                "some avg10=%.2f avg60=0.01 avg300=0.00 total=55927348\nfull avg10=%.2f avg60=0.00 avg300=0.00 total=19783997\n",
                some, full);
  return buffer;
}

}  // namespace

int main() {
  // The FLX1s on a quiet day: half the memory available, nobody waiting.
  const auto quiet = ParseMemory(Meminfo(7853056, 3937280), Pressure(0., 0.));
  assert(quiet.known && quiet.totalKb == 7853056 && quiet.availableKb == 3937280);
  assert(LevelOf(quiet) == MemoryLevel::Normal);

  // Little left, or tasks waiting for memory a fifth of the time: low.
  assert(LevelOf(ParseMemory(Meminfo(7853056, 700000), Pressure(0., 0.))) == MemoryLevel::Low);
  const auto waiting = ParseMemory(Meminfo(7853056, 3000000), Pressure(25.5, 2.));
  assert(waiting.someAvg10 == 25.5 && waiting.fullAvg10 == 2.);
  assert(LevelOf(waiting) == MemoryLevel::Low);

  // Almost nothing left, or everything stalled: critical.
  assert(LevelOf(ParseMemory(Meminfo(7853056, 300000), Pressure(0., 0.))) == MemoryLevel::Critical);
  assert(LevelOf(ParseMemory(Meminfo(7853056, 3000000), Pressure(40., 12.))) == MemoryLevel::Critical);

  // Unreadable files (a kernel without pressure, a sandbox without /proc): the memory alone, or nothing.
  assert(LevelOf(ParseMemory(Meminfo(7853056, 700000), "")) == MemoryLevel::Low);
  assert(!ParseMemory("", "").known && LevelOf(ParseMemory("", "")) == MemoryLevel::Normal);

  std::puts("memory pressure: ok");
  return 0;
}
