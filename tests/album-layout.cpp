// Albums shown narrower than they were laid out (gtk-ui/album_layout.hpp): the cells keep touching
// where they touched, keep their gaps, and fill the album.
#include "album_layout.hpp"

#include <cassert>
#include <cstdio>

using namespace Contrail;

int main() {
  // Telegram's layout of three photos at 280 px: one tall on the left, two on the right.
  const auto cells = std::vector<Rect>{{0, 0, 173, 280}, {175, 0, 105, 139}, {175, 141, 105, 139}};
  const auto size = AlbumSize(cells);
  assert(size == Size{280, 280});
  assert(AlbumHeightForWidth(size, 280) == 280);
  assert(AlbumHeightForWidth(size, 600) == 280);  // Never larger than laid out.
  for (const auto width : {280, 279, 250, 201, 160, 133, 100}) {
    const auto height = AlbumHeightForWidth(size, width);
    const auto left = ScaleAlbumCell(cells[0], size, width, height);
    const auto top = ScaleAlbumCell(cells[1], size, width, height);
    const auto bottom = ScaleAlbumCell(cells[2], size, width, height);
    // The album's edges are the cells' edges.
    assert(left.x == 0 && left.y == 0 && left.height == height);
    assert(top.x + top.width == width && bottom.y + bottom.height == height);
    // No overlap, and the gaps are the same on both sides of the right column.
    assert(left.x + left.width <= top.x);
    assert(top.x == bottom.x && top.width == bottom.width);
    assert(top.y + top.height <= bottom.y);
    const auto gapX = top.x - (left.x + left.width);
    const auto gapY = bottom.y - (top.y + top.height);
    assert(gapX >= 0 && gapX <= 2 && gapY >= 0 && gapY <= 2);
  }
  // Ten cells in rows: every row spans the whole width, at any width.
  auto grid = std::vector<Rect>();
  for (auto row = 0; row != 3; ++row) {
    const auto count = (row == 1) ? 4 : 3;
    const auto cell = (280 - (count - 1) * 2) / count;
    for (auto i = 0; i != count; ++i) {
      const auto x = i * (cell + 2);
      grid.push_back(Rect{x, row * 92, (i + 1 == count) ? 280 - x : cell, 90});
    }
  }
  const auto gridSize = AlbumSize(grid);
  for (const auto width : {280, 237, 199}) {
    const auto height = AlbumHeightForWidth(gridSize, width);
    auto right = 0;
    for (const auto &cell : grid) {
      const auto scaled = ScaleAlbumCell(cell, gridSize, width, height);
      assert(scaled.width > 0 && scaled.height > 0);
      right = std::max(right, scaled.x + scaled.width);
    }
    assert(right == width);
  }
  assert(ScaleAlbumCell(Rect{0, 0, 10, 10}, Size(), 100, 100) == Rect());
  // The arithmetic of the official apps: an even pair, and three with a tall first one.
  assert((LayoutAlbum({{800, 800}, {800, 800}}, 280, 100, 2) == std::vector<Rect>{{0, 0, 139, 139}, {141, 0, 139, 139}}));
  const auto three = LayoutAlbum({{600, 1200}, {800, 800}, {800, 800}}, 280, 100, 2);
  assert(three.size() == 3 && three[0].x == 0 && three[0].height == 280 && three[1].x == three[2].x);
  assert(three[1].right() <= 280 && three[2].bottom() == 280);
  // Ten pictures fill every line, without overlapping.
  const auto many = LayoutAlbum(std::vector<Size>(10, Size{800, 600}), 280, 100, 2);
  assert(many.size() == 10);
  for (auto i = std::size_t(0); i != many.size(); ++i) {
    assert(many[i].width > 0 && many[i].height > 0 && many[i].right() <= 280);
    for (auto j = i + 1; j != many.size(); ++j) {
      const auto &a = many[i], &b = many[j];
      assert(a.right() <= b.x || b.right() <= a.x || a.bottom() <= b.y || b.bottom() <= a.y);
    }
  }
  assert(LayoutAlbum({}, 280, 100, 2).empty());
  std::puts("album layout: ok");
  return 0;
}
