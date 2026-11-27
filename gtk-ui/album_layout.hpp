#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace Contrail {

struct Size {
  int width = 0;
  int height = 0;
  friend bool operator==(const Size &, const Size &) = default;
};

struct Rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  [[nodiscard]] int right() const { return x + width; }
  [[nodiscard]] int bottom() const { return y + height; }
  friend bool operator==(const Rect &, const Rect &) = default;
};

// Telegram's album layout: where each picture of `sizes` goes in an album `maxWidth` wide. The same arithmetic as
// the official apps, so an album looks the same everywhere. `sizes` must be positive.
[[nodiscard]] std::vector<Rect> LayoutAlbum(const std::vector<Size> &sizes, int maxWidth, int minWidth, int spacing);

// The size of an album: the union of its cells.
[[nodiscard]] inline Size AlbumSize(const std::vector<Rect> &cells) {
  auto result = Size();
  for (const auto &cell : cells) {
    result.width = std::max(result.width, cell.right());
    result.height = std::max(result.height, cell.bottom());
  }
  return result;
}

// A cell of an album laid out for `full` and shown at `width` x `height` (narrower on a small screen):
// each edge is scaled on its own, so cells that touched still touch and the gaps stay even.
[[nodiscard]] inline Rect ScaleAlbumCell(const Rect &cell, const Size &full, int width, int height) {
  if (full.width <= 0 || full.height <= 0) return Rect();
  const auto x = [&](int value) { return int(std::lround(double(value) * width / full.width)); };
  const auto y = [&](int value) { return int(std::lround(double(value) * height / full.height)); };
  const auto left = x(cell.x), right = x(cell.right());
  const auto top = y(cell.y), bottom = y(cell.bottom());
  return Rect{left, top, std::max(right - left, 1), std::max(bottom - top, 1)};
}

// The height of an album shown `width` wide: it keeps its proportions and never grows past its size.
[[nodiscard]] inline int AlbumHeightForWidth(const Size &full, int width) {
  if (full.width <= 0) return 0;
  const auto shown = std::min(width, full.width);
  return std::max(1, int(std::lround(double(full.height) * shown / full.width)));
}

}  // namespace Contrail
