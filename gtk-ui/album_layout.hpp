#pragma once

#include <QtCore/QRect>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Contrail {

// The size of an album laid out by Ui::LayoutMediaGroup: the union of its cells.
[[nodiscard]] inline QSize AlbumSize(const std::vector<QRect> &cells) {
  auto united = QRect();
  for (const auto &cell : cells) {
    united = united.united(cell);
  }
  return QSize(united.x() + united.width(), united.y() + united.height());
}

// A cell of an album laid out for `full` and shown at `width` x `height` (narrower on a small screen):
// each edge is scaled on its own, so cells that touched still touch and the gaps stay even.
[[nodiscard]] inline QRect ScaleAlbumCell(const QRect &cell, const QSize &full, int width, int height) {
  if (full.width() <= 0 || full.height() <= 0) return QRect();
  const auto x = [&](int value) { return int(std::lround(double(value) * width / full.width())); };
  const auto y = [&](int value) { return int(std::lround(double(value) * height / full.height())); };
  const auto left = x(cell.x()), right = x(cell.x() + cell.width());
  const auto top = y(cell.y()), bottom = y(cell.y() + cell.height());
  return QRect(left, top, std::max(right - left, 1), std::max(bottom - top, 1));
}

// The height of an album shown `width` wide: it keeps its proportions and never grows past its size.
[[nodiscard]] inline int AlbumHeightForWidth(const QSize &full, int width) {
  if (full.width() <= 0) return 0;
  const auto shown = std::min(width, full.width());
  return std::max(1, int(std::lround(double(full.height()) * shown / full.width())));
}

}  // namespace Contrail
