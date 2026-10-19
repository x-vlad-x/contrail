#pragma once

#include <gtk/gtk.h>

#include <string>

namespace Contrail {

// A photo that can be zoomed (1x to 6x) and moved: pinch keeps the point between the fingers under
// them, a drag moves a zoomed photo within its edges, a double tap zooms to 2.5x around the tap and
// back, Ctrl+wheel zooms around the pointer. The photo is drawn scaled by the GPU, never re-laid out.
// See zoom_geometry.hpp for the arithmetic.
GtkWidget *ZoomViewNew();

// A new photo of `width` x `height` (its own size): the view starts from the whole photo.
void ZoomViewSetPhoto(GtkWidget *view, GdkTexture *texture, int width, int height);
// A better picture of the same photo (the full size after the thumbnail): the view stays where it is.
void ZoomViewSetTexture(GtkWidget *view, GdkTexture *texture);

// For the debug actions: the zoom view under `root`, and where it is (scale, the photo's rectangle, the
// point of the photo in the middle of the view, the texture's size).
GtkWidget *ZoomViewFind(GtkWidget *root);
std::string ZoomViewState(GtkWidget *view);

}  // namespace Contrail
