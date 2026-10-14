#pragma once

#include <adwaita.h>

#include <algorithm>

namespace Contrail {

inline void ConfigureMessageWrapping(GtkLabel *label) {
  gtk_label_set_wrap(label, TRUE);
  gtk_label_set_wrap_mode(label, PANGO_WRAP_WORD_CHAR);
  // Bound the natural paragraph width while keeping the minimum small on a phone.
  gtk_label_set_max_width_chars(label, 48);
}

// GtkLabel with links handles presses itself: it claims every touch anywhere on its text (even off
// the links), which cancels the scrolling of the chat. Switches its click and selection gestures off
// (they come back with every gtk_label_set_markup() that has links); the gestures marked
// "contrail-own" stay. Links are then opened by an own click gesture, see MakeTextLabel().
inline void DisableLabelGestures(GtkWidget *label) {
  auto *controllers = gtk_widget_observe_controllers(label);
  const auto count = g_list_model_get_n_items(controllers);
  for (guint i = 0; i != count; ++i) {
    auto *controller = GTK_EVENT_CONTROLLER(g_list_model_get_item(controllers, i));
    if (GTK_IS_GESTURE(controller) && !g_object_get_data(G_OBJECT(controller), "contrail-own")) {
      gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_NONE);
    }
    g_object_unref(controller);
  }
  g_object_unref(controllers);
}

// Media never get narrower than this unless their own width is smaller.
constexpr auto kMediaMinWidth = 120;

inline GtkWidget *MakeMediaPicture(int width, int height) {
  auto *picture = gtk_picture_new();
  gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
  gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
  auto *placeholder = gdk_paintable_new_empty(width, height);
  gtk_picture_set_paintable(GTK_PICTURE(picture), placeholder);
  g_object_unref(placeholder);
  gtk_widget_set_overflow(picture, GTK_OVERFLOW_HIDDEN);
  // The size the picture is shown at, for scaling the images put into it.
  g_object_set_data(G_OBJECT(picture), "contrail-media-width", GINT_TO_POINTER(width));
  g_object_set_data(G_OBJECT(picture), "contrail-media-height", GINT_TO_POINTER(height));
  return picture;
}

// A box for a picture with a fixed aspect ratio: up to `width` wide, narrower on a small screen,
// the height follows the width. The size doesn't depend on the picture inside, so rows keep their
// height while thumbnails and photos load. GtkListBox gives rows their *minimum* height, so the
// minimum height is the real one here.
inline GtkWidget *MakeMediaFrame(GtkWidget *picture, int width, int height) {
  auto *frame = adw_bin_new();
  g_object_set_data(G_OBJECT(frame), "contrail-media-width", GINT_TO_POINTER(std::max(width, 1)));
  g_object_set_data(G_OBJECT(frame), "contrail-media-height", GINT_TO_POINTER(std::max(height, 1)));
  gtk_widget_set_layout_manager(
      frame, gtk_custom_layout_new(
                 [](GtkWidget *) { return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH; },
                 [](GtkWidget *widget, GtkOrientation orientation, int forSize, int *minimum, int *natural, int *,
                    int *) {
                   const auto w = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "contrail-media-width"));
                   const auto h = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "contrail-media-height"));
                   if (orientation == GTK_ORIENTATION_HORIZONTAL) {
                     *minimum = std::min(w, kMediaMinWidth);
                     *natural = w;
                   } else {
                     // GTK requires the height for no width to be the largest one: wider than its
                     // own width (in a wide bubble) the picture is cropped instead of growing.
                     const auto forWidth = (forSize < 0) ? w : std::min(forSize, w);
                     *minimum = *natural = std::max(1, int(double(forWidth) * h / w + 0.5));
                   }
                 },
                 [](GtkWidget *widget, int width, int height, int baseline) {
                   if (auto *child = adw_bin_get_child(ADW_BIN(widget))) {
                     gtk_widget_allocate(child, width, height, baseline, nullptr);
                   }
                 }));
  adw_bin_set_child(ADW_BIN(frame), picture);
  return frame;
}

} // namespace Contrail
