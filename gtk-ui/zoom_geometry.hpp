#pragma once

#include <algorithm>
#include <cmath>

// The geometry of the photo viewer: where the photo is drawn for a scale and an offset, and how
// pinching, dragging, double tapping and resizing change them. Plain arithmetic, tested without a
// display (tests/zoom-geometry.cpp).
namespace Contrail::Zoom {

constexpr auto kMinScale = 1.;
constexpr auto kMaxScale = 6.;
constexpr auto kDoubleTapScale = 2.5;

struct Point {
  double x = 0.;
  double y = 0.;
};

struct Size {
  double width = 0.;
  double height = 0.;
  [[nodiscard]] bool empty() const { return width <= 0. || height <= 0.; }
};

struct Rect {
  double x = 0.;
  double y = 0.;
  double width = 0.;
  double height = 0.;
};

// The scale over the fitted photo, and where the photo's center is from the view's center.
struct State {
  double scale = kMinScale;
  Point offset;
};

// The whole photo in the view, as large as it fits, centered: the margins are left or right of a tall
// photo, above and below a wide one. `image` is the photo's own size (its proportions only matter).
[[nodiscard]] inline Rect FitRect(Size image, Size view) {
  if (image.empty() || view.empty()) return {};
  const auto factor = std::min(view.width / image.width, view.height / image.height);
  const auto width = image.width * factor, height = image.height * factor;
  return {(view.width - width) / 2., (view.height - height) / 2., width, height};
}

// Where the photo is drawn.
[[nodiscard]] inline Rect Displayed(const State &state, Size image, Size view) {
  const auto fit = FitRect(image, view);
  const auto width = fit.width * state.scale, height = fit.height * state.scale;
  const auto centerX = view.width / 2. + state.offset.x, centerY = view.height / 2. + state.offset.y;
  return {centerX - width / 2., centerY - height / 2., width, height};
}

// Keeps the photo in the view: wider than the view it can't be moved off its edges (no empty margin
// opens up at an edge), narrower it stays in the middle. The same vertically.
[[nodiscard]] inline State Clamp(State state, Size image, Size view) {
  state.scale = std::clamp(state.scale, kMinScale, kMaxScale);
  const auto rect = Displayed(state, image, view);
  const auto clampAxis = [](double offset, double length, double viewLength) {
    if (length <= viewLength) return 0.;
    const auto limit = (length - viewLength) / 2.;
    return std::clamp(offset, -limit, limit);
  };
  state.offset.x = clampAxis(state.offset.x, rect.width, view.width);
  state.offset.y = clampAxis(state.offset.y, rect.height, view.height);
  return state;
}

// The point of the photo under a point of the view, as a fraction of the photo's width and height.
[[nodiscard]] inline Point ImagePointAt(const State &state, Size image, Size view, Point at) {
  const auto rect = Displayed(state, image, view);
  if (rect.width <= 0. || rect.height <= 0.) return {0.5, 0.5};
  return {(at.x - rect.x) / rect.width, (at.y - rect.y) / rect.height};
}

// The state at `scale` with the photo's point `imagePoint` under the view's point `at`, kept in the view.
[[nodiscard]] inline State Place(double scale, Point imagePoint, Point at, Size image, Size view) {
  scale = std::clamp(scale, kMinScale, kMaxScale);
  const auto fit = FitRect(image, view);
  const auto width = fit.width * scale, height = fit.height * scale;
  auto state = State{scale};
  state.offset.x = at.x - imagePoint.x * width + width / 2. - view.width / 2.;
  state.offset.y = at.y - imagePoint.y * height + height / 2. - view.height / 2.;
  return Clamp(state, image, view);
}

// Pinching: the point of the photo that was between the fingers when they touched follows the point
// between them, which moves as the fingers move.
[[nodiscard]] inline State Pinch(const State &start, Point startCenter, double scaleChange, Point center, Size image,
                                 Size view) {
  const auto anchor = ImagePointAt(start, image, view, startCenter);
  return Place(start.scale * scaleChange, anchor, center, image, view);
}

// Dragging a zoomed photo.
[[nodiscard]] inline State Pan(const State &start, Point delta, Size image, Size view) {
  auto state = start;
  state.offset.x += delta.x;
  state.offset.y += delta.y;
  return Clamp(state, image, view);
}

// A double tap zooms in around the tapped point; on a zoomed photo it shows the whole photo again.
[[nodiscard]] inline State DoubleTap(const State &state, Point at, Size image, Size view) {
  if (state.scale > kMinScale + 0.01) {
    return State();
  }
  return Place(kDoubleTapScale, ImagePointAt(state, image, view, at), at, image, view);
}

// The view changed its size (the window, the phone turned): what was in the middle stays in the middle.
[[nodiscard]] inline State Resize(const State &state, Size image, Size oldView, Size newView) {
  if (oldView.empty()) {
    return Clamp(state, image, newView);
  }
  const auto middle = ImagePointAt(state, image, oldView, {oldView.width / 2., oldView.height / 2.});
  return Place(state.scale, middle, {newView.width / 2., newView.height / 2.}, image, newView);
}

}  // namespace Contrail::Zoom
