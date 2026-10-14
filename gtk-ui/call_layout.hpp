#pragma once

#include "layout.hpp"

namespace Contrail {

inline GtkWidget *MakeCallVideoStage(GtkWidget *remote, GtkWidget *local, GtkWidget **localFrame) {
  gtk_picture_set_can_shrink(GTK_PICTURE(remote), TRUE);
  gtk_picture_set_content_fit(GTK_PICTURE(remote), GTK_CONTENT_FIT_COVER);
  gtk_widget_set_hexpand(remote, TRUE);
  gtk_widget_set_vexpand(remote, TRUE);
  auto *stage = gtk_overlay_new();
  gtk_widget_add_css_class(stage, "call-stage");
  gtk_overlay_set_child(GTK_OVERLAY(stage), remote);
  gtk_picture_set_can_shrink(GTK_PICTURE(local), TRUE);
  gtk_picture_set_content_fit(GTK_PICTURE(local), GTK_CONTENT_FIT_COVER);
  *localFrame = MakeMediaFrame(local, 96, 144);
  gtk_widget_set_halign(*localFrame, GTK_ALIGN_END);
  gtk_widget_set_valign(*localFrame, GTK_ALIGN_START);
  gtk_widget_set_margin_top(*localFrame, 104);
  gtk_widget_set_margin_end(*localFrame, 12);
  gtk_widget_add_css_class(*localFrame, "call-preview");
  gtk_widget_set_overflow(*localFrame, GTK_OVERFLOW_HIDDEN);
  gtk_overlay_add_overlay(GTK_OVERLAY(stage), *localFrame);
  gtk_overlay_set_measure_overlay(GTK_OVERLAY(stage), *localFrame, FALSE);
  gtk_overlay_set_clip_overlay(GTK_OVERLAY(stage), *localFrame, TRUE);
  return stage;
}

} // namespace Contrail
