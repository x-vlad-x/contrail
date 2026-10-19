// The photo viewer's geometry (gtk-ui/zoom_geometry.hpp): pinching, dragging, double taps and resizing.
#include "zoom_geometry.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace Contrail::Zoom;

namespace {

bool Near(double a, double b, double epsilon = 1e-6) {
  return std::abs(a - b) < epsilon;
}

bool Near(Point a, Point b, double epsilon = 1e-6) {
  return Near(a.x, b.x, epsilon) && Near(a.y, b.y, epsilon);
}

// The photo covers the view where it is larger than it, and is centered where it is smaller.
void CheckInView(const State &state, Size image, Size view) {
  const auto rect = Displayed(state, image, view);
  const auto axis = [](double start, double length, double viewLength) {
    if (length <= viewLength + 1e-9) {
      assert(Near(start, (viewLength - length) / 2.));
    } else {
      assert(start <= 1e-9);
      assert(start + length >= viewLength - 1e-9);
    }
  };
  axis(rect.x, rect.width, view.width);
  axis(rect.y, rect.height, view.height);
  assert(state.scale >= kMinScale && state.scale <= kMaxScale);
}

}  // namespace

int main() {
  const auto phone = Size{412, 860};       // A phone held upright.
  const auto turned = Size{860, 412};      // The same phone turned.
  const auto wide = Size{4000, 3000};      // A landscape photo.
  const auto tall = Size{1080, 2400};      // A screenshot.

  // The whole photo, centered, with margins only across the photo's short side.
  const auto fit = FitRect(wide, phone);
  assert(Near(fit.x, 0.) && Near(fit.width, 412.) && Near(fit.height, 309.));
  assert(Near(fit.y, (860. - 309.) / 2.));
  const auto fitTall = FitRect(tall, turned);
  assert(Near(fitTall.height, 412.) && Near(fitTall.y, 0.) && fitTall.x > 0.);
  // The texture's size doesn't matter, only the photo's: a thumbnail and the full photo coincide.
  const auto thumbnail = FitRect({90, 67.5}, phone), full = FitRect({1280, 960}, phone);
  assert(Near(thumbnail.width, full.width) && Near(thumbnail.y, full.y));

  // Pinching in the middle keeps the middle of the photo in the middle.
  const auto middle = Point{phone.width / 2., phone.height / 2.};
  const auto centered = Pinch(State(), middle, 2., middle, wide, phone);
  assert(Near(centered.scale, 2.));
  assert(Near(ImagePointAt(centered, wide, phone, middle), {0.5, 0.5}));
  CheckInView(centered, wide, phone);

  // Pinching off the middle: the point between the fingers stays under them.
  const auto between = Point{180, 430};  // Far enough from the edges that nothing is clamped.
  const auto anchor = ImagePointAt(State(), wide, phone, between);
  const auto pinched = Pinch(State(), between, 3., between, wide, phone);
  assert(Near(ImagePointAt(pinched, wide, phone, between), anchor));
  CheckInView(pinched, wide, phone);

  // The fingers move while pinching: the photo follows the point between them.
  const auto moved = Point{between.x + 40, between.y - 25};
  const auto followed = Pinch(State(), between, 3., moved, wide, phone);
  assert(Near(ImagePointAt(followed, wide, phone, moved), anchor));
  CheckInView(followed, wide, phone);

  // Pinching at a corner of the photo: the corner can't leave an empty margin at the view's edge.
  const auto corner = Point{0, fit.y};
  const auto atCorner = Pinch(State(), corner, 2., corner, wide, phone);
  assert(Near(Displayed(atCorner, wide, phone).x, 0.));
  CheckInView(atCorner, wide, phone);
  const auto farCorner = Point{phone.width, fit.y + fit.height};
  const auto atFarCorner = Pinch(State(), farCorner, 4., farCorner, wide, phone);
  const auto far = Displayed(atFarCorner, wide, phone);
  assert(Near(far.x + far.width, phone.width));
  CheckInView(atFarCorner, wide, phone);

  // The scale stays within 1x..6x.
  assert(Near(Pinch(State(), middle, 20., middle, wide, phone).scale, kMaxScale));
  assert(Near(Pinch(centered, middle, 0.1, middle, wide, phone).scale, kMinScale));
  CheckInView(Pinch(centered, middle, 0.1, middle, wide, phone), wide, phone);

  // Dragging a zoomed photo stops at its edges; at 1x it doesn't move.
  const auto dragged = Pan(centered, {10000, -10000}, wide, phone);
  CheckInView(dragged, wide, phone);
  assert(Near(Displayed(dragged, wide, phone).x, 0.));
  const auto notZoomed = Pan(State(), {50, 50}, wide, phone);
  assert(Near(notZoomed.offset, {0, 0}));
  const auto small = Pan(centered, {12, -7}, wide, phone);
  assert(Near(small.offset.x - centered.offset.x, 12.) && Near(small.offset.y, centered.offset.y));

  // A double tap zooms to 2.5x around the tap, the next one shows the whole photo again.
  const auto tap = Point{220, 400};
  const auto tapped = DoubleTap(State(), tap, tall, phone);
  assert(Near(tapped.scale, kDoubleTapScale));
  assert(Near(ImagePointAt(tapped, tall, phone, tap), ImagePointAt(State(), tall, phone, tap)));
  CheckInView(tapped, tall, phone);
  // A wide photo is still lower than the view at 2.5x: it zooms around the tap across, stays centered down.
  const auto tappedWide = DoubleTap(State(), tap, wide, phone);
  assert(Near(ImagePointAt(tappedWide, wide, phone, tap).x, ImagePointAt(State(), wide, phone, tap).x));
  CheckInView(tappedWide, wide, phone);
  const auto back = DoubleTap(tapped, tap, tall, phone);
  assert(Near(back.scale, kMinScale) && Near(back.offset, {0, 0}));
  // On the margin above a wide photo the tap's height is outside it: the photo stays in the view.
  CheckInView(DoubleTap(State(), {200, 20}, wide, phone), wide, phone);

  // Turning the phone keeps what was in the middle of the view in the middle.
  const auto zoomed = Pinch(State(), between, 6., between, tall, phone);  // Larger than the view either way.
  const auto seen = ImagePointAt(zoomed, tall, phone, middle);
  const auto rotated = Resize(zoomed, tall, phone, turned);
  assert(Near(rotated.scale, zoomed.scale));
  assert(Near(ImagePointAt(rotated, tall, turned, {turned.width / 2., turned.height / 2.}), seen, 1e-3));
  CheckInView(rotated, tall, turned);
  // Not zoomed, it stays the whole photo; the first size of the view only places it.
  assert(Near(Resize(State(), tall, phone, turned).offset, {0, 0}));
  CheckInView(Resize(centered, wide, Size(), phone), wide, phone);

  // A dense screen changes nothing: the geometry is in logical pixels, fractions included.
  const auto fractional = Pinch(State(), {200.25, 430.75}, 1.7, {201.5, 429.25}, tall, phone);
  assert(Near(ImagePointAt(fractional, tall, phone, {201.5, 429.25}), ImagePointAt(State(), tall, phone, {200.25, 430.75})));

  std::puts("zoom geometry: ok");
  return 0;
}
