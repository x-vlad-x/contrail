#pragma once

#include "album_layout.hpp"

#include <adwaita.h>

#include <algorithm>
#include <memory>
#include <vector>

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

// A bin whose width doesn't depend on the height offered: the message bubble in its row. A horizontal
// GtkBox (the row with the avatar) asks its children for their width at the row's height; a vertical
// box (the bubble) answers that with a binary search over widths, which assumes its children get no
// taller as it gets wider. Pictures do (MakeMediaFrame): with a picture and text in one bubble the
// search can stop converging, and GTK spins in it forever (a link preview on a 413 px wide phone
// screen: 226 px high at 195 px, 235 at 198, 225 at 240).
inline GtkWidget *MakeHeightForWidthBin(GtkWidget *child) {
  auto *bin = adw_bin_new();
  gtk_widget_set_layout_manager(
      bin, gtk_custom_layout_new(
               [](GtkWidget *) { return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH; },
               [](GtkWidget *widget, GtkOrientation orientation, int forSize, int *minimum, int *natural,
                  int *minimumBaseline, int *naturalBaseline) {
                 auto *child = adw_bin_get_child(ADW_BIN(widget));
                 if (!child || !gtk_widget_should_layout(child)) {
                   return;
                 }
                 if (orientation == GTK_ORIENTATION_HORIZONTAL) {
                   forSize = -1;
                 } else if (forSize >= 0) {
                   // The bubble never gets wider than its natural width (it doesn't expand), but GTK 4.24's
                   // GtkBox asks for its height at the whole width of the row and then gives it only that.
                   int naturalWidth = 0;
                   gtk_widget_measure(child, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &naturalWidth, nullptr,
                                      nullptr);
                   forSize = std::min(forSize, naturalWidth);
                 }
                 gtk_widget_measure(child, orientation, forSize, minimum, natural, minimumBaseline,
                                    naturalBaseline);
               },
               [](GtkWidget *widget, int width, int height, int baseline) {
                 if (auto *child = adw_bin_get_child(ADW_BIN(widget))) {
                   gtk_widget_allocate(child, width, height, baseline, nullptr);
                 }
               }));
  // A child that expands (the voice player's column, a location's map) mustn't make the bubble
  // expand: its row (halign START/END) would measure the bubble's height at the whole width of the
  // chat and then give it only its natural width, and the text, taller there, would overflow the
  // bubble into the next messages on a wide window.
  gtk_widget_set_hexpand(bin, FALSE);
  adw_bin_set_child(ADW_BIN(bin), child);
  return bin;
}

// An album: its cells placed as Ui::LayoutMediaGroup laid them out for `size`, scaled down together on
// a screen narrower than that. The children are the cells, in the order of `cells`.
inline GtkWidget *MakeAlbumGrid(const std::vector<GtkWidget *> &children, std::vector<QRect> cells, QSize size) {
  struct Grid {
    std::vector<QRect> cells;
    QSize size;
  };
  // A box only for disposing of its children: the layout is its own.
  auto *grid = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  auto *data = new Grid{std::move(cells), size};
  g_object_set_data_full(G_OBJECT(grid), "contrail-album", data, [](gpointer p) { delete static_cast<Grid *>(p); });
  gtk_widget_set_layout_manager(
      grid, gtk_custom_layout_new(
                [](GtkWidget *) { return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH; },
                [](GtkWidget *widget, GtkOrientation orientation, int forSize, int *minimum, int *natural, int *, int *) {
                  const auto grid = static_cast<Grid *>(g_object_get_data(G_OBJECT(widget), "contrail-album"));
                  if (orientation == GTK_ORIENTATION_HORIZONTAL) {
                    *minimum = std::min(grid->size.width(), kMediaMinWidth);
                    *natural = grid->size.width();
                  } else {
                    // As MakeMediaFrame: the height for no width is the largest one.
                    *minimum = *natural = AlbumHeightForWidth(grid->size, forSize < 0 ? grid->size.width() : forSize);
                  }
                },
                [](GtkWidget *widget, int width, int height, int baseline) {
                  const auto grid = static_cast<Grid *>(g_object_get_data(G_OBJECT(widget), "contrail-album"));
                  const auto shown = std::min(width, grid->size.width());
                  const auto shownHeight = AlbumHeightForWidth(grid->size, shown);
                  auto i = 0;
                  for (auto *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
                    if (i >= int(grid->cells.size())) break;
                    const auto rect = ScaleAlbumCell(grid->cells[i++], grid->size, shown, shownHeight);
                    const auto origin = graphene_point_t{float(rect.x()), float(rect.y())};
                    gtk_widget_allocate(child, rect.width(), rect.height(), -1, gsk_transform_translate(nullptr, &origin));
                  }
                }));
  for (auto *child : children) {
    gtk_box_append(GTK_BOX(grid), child);
  }
  gtk_widget_add_css_class(grid, "album");
  gtk_widget_set_overflow(grid, GTK_OVERFLOW_HIDDEN);
  return grid;
}

} // namespace Contrail
