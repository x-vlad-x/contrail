#pragma once

#include <algorithm>
#include <cstdint>

// The progress of a download and of a player's track as the messages show it: plain arithmetic, tested
// without a display (tests/media-progress.cpp).
namespace Contrail {

// The part of a file downloaded, 0 while nothing has come yet (or its size is unknown).
inline double DownloadFraction(int64_t offset, int64_t size) {
  return (offset > 0 && size > 0) ? std::clamp(double(offset) / size, 0., 1.) : 0.;
}

// Where a tap or a drag on a player's bar seeks to: its share of the bar's width.
inline double SeekFraction(double x, double width) {
  return (width > 0) ? std::clamp(x / width, 0., 1.) : 0.;
}

// The part of a track the player has the data for (Media::Player::TrackState::receivedTill, in the units of its
// position and length): all of it when the file is loaded whole, which the core says with 0.
inline double BufferedFraction(int64_t receivedTill, int64_t length) {
  return (receivedTill > 0 && length > 0) ? std::clamp(double(receivedTill) / length, 0., 1.) : 1.;
}

// The player has reached the end of the data received so far and waits for more.
inline bool WaitingForData(int64_t position, int64_t receivedTill, int64_t length) {
  return receivedTill > 0 && receivedTill < length && position >= receivedTill;
}

}  // namespace Contrail
