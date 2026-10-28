// The progress of downloads and of the player's tracks (gtk-ui/media_progress.hpp).
#include "media_progress.hpp"

#include <cassert>
#include <cstdio>

using namespace Contrail;

int main() {
  assert(DownloadFraction(0, 1000) == 0.);
  assert(DownloadFraction(250, 1000) == 0.25);
  assert(DownloadFraction(1000, 1000) == 1.);
  assert(DownloadFraction(1500, 1000) == 1.);
  assert(DownloadFraction(100, 0) == 0.);  // The size isn't known.

  assert(SeekFraction(50, 200) == 0.25);
  assert(SeekFraction(-10, 200) == 0.);  // A drag past the ends stays at them.
  assert(SeekFraction(300, 200) == 1.);
  assert(SeekFraction(10, 0) == 0.);  // Not allocated yet.

  assert(BufferedFraction(0, 60000) == 1.);  // Loaded whole.
  assert(BufferedFraction(15000, 60000) == 0.25);
  assert(BufferedFraction(70000, 60000) == 1.);
  assert(BufferedFraction(100, 0) == 1.);

  assert(!WaitingForData(10000, 0, 60000));  // Loaded whole.
  assert(!WaitingForData(10000, 15000, 60000));
  assert(WaitingForData(15000, 15000, 60000));
  assert(!WaitingForData(60000, 60000, 60000));  // Received to the end: nothing to wait for.
  std::puts("media-progress: ok");
}
