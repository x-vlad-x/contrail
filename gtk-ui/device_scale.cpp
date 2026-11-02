#include "device_scale.hpp"

#include <algorithm>
#include <cmath>

namespace Contrail {

double DeviceScale(GtkWidget *widget) {
  auto scale = 1.;
  if (auto *native = widget ? gtk_widget_get_native(widget) : nullptr) {
    if (auto *surface = gtk_native_get_surface(native)) scale = gdk_surface_get_scale(surface);
  }
  auto *display = widget ? gtk_widget_get_display(widget) : gdk_display_get_default();
  if (auto *monitors = display ? gdk_display_get_monitors(display) : nullptr) {
    const auto count = g_list_model_get_n_items(monitors);
    for (auto i = 0u; i < count; ++i) {
      auto *monitor = GDK_MONITOR(g_list_model_get_item(monitors, i));
      scale = std::max(scale, gdk_monitor_get_scale(monitor));
      g_object_unref(monitor);
    }
  }
  return std::max(scale, 1.);
}

int DevicePixels(GtkWidget *widget, int size) {
  // A hair below a whole number is that number: 32 × 1.75 is 56, not 57 from float rounding.
  return std::max(int(std::ceil(size * DeviceScale(widget) - 0.01)), 1);
}

}  // namespace Contrail
