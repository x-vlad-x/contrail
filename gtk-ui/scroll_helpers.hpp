#pragma once

#include <gtk/gtk.h>

#include <cmath>

namespace Contrail {

inline bool WidgetViewportY(GtkWidget *widget, GtkWidget *scroll, double &y) {
  const graphene_point_t zero = GRAPHENE_POINT_INIT_ZERO;
  graphene_point_t point;
  if (!gtk_widget_compute_point(widget, scroll, &zero, &point)) return false;
  y = point.y;
  return true;
}

inline bool ScrollWidgetToY(GtkWidget *widget, GtkWidget *scroll, GtkAdjustment *adjustment, double y) {
  double currentY = 0;
  if (!WidgetViewportY(widget, scroll, currentY)) return false;
  // compute_point() includes the viewport's scrolling transform: it is a visible coordinate,
  // not an absolute history offset. Add the current scroll value before positioning the row.
  gtk_adjustment_set_value(adjustment, gtk_adjustment_get_value(adjustment) + currentY - y);
  return true;
}

// Telling a tap from a scroll, as the phone apps do. Links, photos and buttons in messages react
// on release; GTK still delivers that release after a short flick that stayed under the scrolled
// window's drag threshold, or after a touch that only stopped a fling. The messages ask
// TapAllowed() before acting. The window feeds every press, motion and release here (capture
// phase, before the widgets under the finger see them).
struct TapState {
  double x = 0;
  double y = 0;
  bool pressed = false;
  bool moved = false;
  bool stoppedScroll = false;
};
inline TapState tapState;
inline gint64 lastListMotion = 0;
constexpr auto kTapSlop = 10.;  // Logical pixels, Android's touch slop is 8dp.

// A scrolled list reports every position change: a press while it moves stops a fling, not a tap.
inline void NoteListMotion() {
  lastListMotion = g_get_monotonic_time();
}
inline bool ListMoving() {
  return lastListMotion && g_get_monotonic_time() - lastListMotion < 150 * 1000;
}

inline void TrackTap(GdkEvent *event) {
  const auto listMoving = ListMoving();
  const auto type = gdk_event_get_event_type(event);
  double x = 0, y = 0;
  const auto hasPosition = gdk_event_get_position(event, &x, &y);
  if (type == GDK_TOUCH_BEGIN || type == GDK_BUTTON_PRESS) {
    tapState = {x, y, true, false, listMoving};
  } else if ((type == GDK_TOUCH_UPDATE || type == GDK_MOTION_NOTIFY) && tapState.pressed && hasPosition) {
    if (std::hypot(x - tapState.x, y - tapState.y) > kTapSlop) tapState.moved = true;
  } else if (type == GDK_TOUCH_END || type == GDK_TOUCH_CANCEL || type == GDK_BUTTON_RELEASE) {
    tapState.pressed = false;
    if (hasPosition && std::hypot(x - tapState.x, y - tapState.y) > kTapSlop) tapState.moved = true;
  }
}

inline bool TapAllowed() {
  return !tapState.moved && !tapState.stoppedScroll;
}

} // namespace Contrail
