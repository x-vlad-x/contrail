// How many device pixels a widget's pixel takes. Main thread only.
#pragma once

#include <gtk/gtk.h>

namespace Contrail {

// The surface's own scale, which can be fractional: 1.74 on the FLX1s, where GTK's whole-number scale factor says 2
// and a texture that size has a third more pixels than the screen shows. No smaller than any monitor's scale, so a
// picture decoded for one screen stays sharp when the window moves to another. 1 or more.
[[nodiscard]] double DeviceScale(GtkWidget *widget);

// A size in widget pixels, in device pixels: rounded up, so the picture is never smaller than its place.
[[nodiscard]] int DevicePixels(GtkWidget *widget, int size);

}  // namespace Contrail
