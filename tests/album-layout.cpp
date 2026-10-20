// Albums shown narrower than they were laid out (gtk-ui/album_layout.hpp): the cells keep touching
// where they touched, keep their gaps, and fill the album.
#include "album_layout.hpp"

#include <cassert>
#include <cstdio>

using namespace Contrail;

int main() {
  // Ui::LayoutMediaGroup's layout of three photos at 280 px: one tall on the left, two on the right.
  const auto cells = std::vector<QRect>{QRect(0, 0, 173, 280), QRect(175, 0, 105, 139), QRect(175, 141, 105, 139)};
  const auto size = AlbumSize(cells);
  assert(size == QSize(280, 280));
  assert(AlbumHeightForWidth(size, 280) == 280);
  assert(AlbumHeightForWidth(size, 600) == 280);  // Never larger than laid out.
  for (const auto width : {280, 279, 250, 201, 160, 133, 100}) {
    const auto height = AlbumHeightForWidth(size, width);
    const auto left = ScaleAlbumCell(cells[0], size, width, height);
    const auto top = ScaleAlbumCell(cells[1], size, width, height);
    const auto bottom = ScaleAlbumCell(cells[2], size, width, height);
    // The album's edges are the cells' edges.
    assert(left.x() == 0 && left.y() == 0 && left.height() == height);
    assert(top.x() + top.width() == width && bottom.y() + bottom.height() == height);
    // No overlap, and the gaps are the same on both sides of the right column.
    assert(left.x() + left.width() <= top.x());
    assert(top.x() == bottom.x() && top.width() == bottom.width());
    assert(top.y() + top.height() <= bottom.y());
    const auto gapX = top.x() - (left.x() + left.width());
    const auto gapY = bottom.y() - (top.y() + top.height());
    assert(gapX >= 0 && gapX <= 2 && gapY >= 0 && gapY <= 2);
  }
  // Ten cells in rows: every row spans the whole width, at any width.
  auto grid = std::vector<QRect>();
  for (auto row = 0; row != 3; ++row) {
    const auto count = (row == 1) ? 4 : 3;
    const auto cell = (280 - (count - 1) * 2) / count;
    for (auto i = 0; i != count; ++i) {
      const auto x = i * (cell + 2);
      grid.push_back(QRect(x, row * 92, (i + 1 == count) ? 280 - x : cell, 90));
    }
  }
  const auto gridSize = AlbumSize(grid);
  for (const auto width : {280, 237, 199}) {
    const auto height = AlbumHeightForWidth(gridSize, width);
    auto right = 0;
    for (const auto &cell : grid) {
      const auto scaled = ScaleAlbumCell(cell, gridSize, width, height);
      assert(scaled.width() > 0 && scaled.height() > 0);
      right = std::max(right, scaled.x() + scaled.width());
    }
    assert(right == width);
  }
  assert(ScaleAlbumCell(QRect(0, 0, 10, 10), QSize(), 100, 100).isNull());
  std::puts("album layout: ok");
  return 0;
}
