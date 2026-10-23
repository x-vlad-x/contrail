#include "map_view.hpp"

#include "text.hpp"

#include "contrail_version.h"

#ifdef CONTRAIL_SHUMATE
// The header has no G_BEGIN_DECLS (libshumate 1.4).
extern "C" {
#include <shumate/shumate-user-agent.h>
}
#include <shumate/shumate.h>
#endif  // CONTRAIL_SHUMATE

#include <cmath>

namespace Contrail {

bool MapsAvailable() {
#ifdef CONTRAIL_SHUMATE
  return true;
#else
  return false;
#endif
}

#ifdef CONTRAIL_SHUMATE
namespace {

ShumateMapSource *OpenStreetMap() {
  static auto *source = [] {
    // OpenStreetMap blocks library defaults: the requests say which application makes them.
    shumate_set_user_agent((std::string("Contrail/") + kContrailVersion + " (+" + kProjectUrl + ")").c_str());
    auto *registry = shumate_map_source_registry_new_with_defaults();
    auto *result = shumate_map_source_registry_get_by_id(registry, SHUMATE_MAP_SOURCE_OSM_MAPNIK);
    g_object_ref(result);
    g_object_unref(registry);
    return result;
  }();
  return source;
}

ShumateMarkerLayer *MarkerLayer(GtkWidget *map) {
  return SHUMATE_MARKER_LAYER(g_object_get_data(G_OBJECT(map), "contrail-markers"));
}

}  // namespace

GtkWidget *MakeMap() {
  auto *map = shumate_simple_map_new();
  shumate_simple_map_set_map_source(map, OpenStreetMap());
  auto *viewport = shumate_simple_map_get_viewport(map);
  shumate_viewport_set_max_zoom_level(viewport, 19);
  // The scale would cover the attribution on a phone's width (the phone apps show none either).
  gtk_widget_set_visible(GTK_WIDGET(shumate_simple_map_get_scale(map)), FALSE);
  auto *markers = shumate_marker_layer_new(viewport);
  shumate_simple_map_add_overlay_layer(map, SHUMATE_LAYER(markers));
  g_object_set_data(G_OBJECT(map), "contrail-markers", markers);
  gtk_widget_add_css_class(GTK_WIDGET(map), "contrail-map");
  gtk_widget_set_hexpand(GTK_WIDGET(map), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(map), TRUE);
  return GTK_WIDGET(map);
}

void MapCenterOn(GtkWidget *map, double lat, double lon, double zoom, bool animate) {
  auto *simple = SHUMATE_SIMPLE_MAP(map);
  auto *view = shumate_simple_map_get_map(simple);
  if (animate && gtk_widget_get_mapped(map)) {
    shumate_map_go_to_full(view, lat, lon, zoom);
  } else {
    shumate_viewport_set_zoom_level(shumate_simple_map_get_viewport(simple), zoom);
    shumate_map_center_on(view, lat, lon);
  }
}

void MapCenter(GtkWidget *map, double *lat, double *lon) {
  auto *location = SHUMATE_LOCATION(shumate_simple_map_get_viewport(SHUMATE_SIMPLE_MAP(map)));
  *lat = shumate_location_get_latitude(location);
  *lon = shumate_location_get_longitude(location);
}

double MapZoom(GtkWidget *map) {
  return shumate_viewport_get_zoom_level(shumate_simple_map_get_viewport(SHUMATE_SIMPLE_MAP(map)));
}

GtkWidget *MapAddMarker(GtkWidget *map, GtkWidget *child, double lat, double lon) {
  auto *marker = shumate_marker_new();
  shumate_marker_set_child(marker, child);
  shumate_location_set_location(SHUMATE_LOCATION(marker), lat, lon);
  shumate_marker_layer_add_marker(MarkerLayer(map), marker);
  return GTK_WIDGET(marker);
}

void MapMoveMarker(GtkWidget *marker, double lat, double lon) {
  shumate_location_set_location(SHUMATE_LOCATION(marker), lat, lon);
}

void MapRemoveMarker(GtkWidget *map, GtkWidget *marker) {
  shumate_marker_layer_remove_marker(MarkerLayer(map), SHUMATE_MARKER(marker));
}

void MapOnUserMove(GtkWidget *map, Fn<void()> touched) {
  // Every press, touch and wheel turn on the map, before the map's own gestures take them.
  auto *events = gtk_event_controller_legacy_new();
  gtk_event_controller_set_propagation_phase(events, GTK_PHASE_CAPTURE);
  Connect<gboolean(GtkEventControllerLegacy *, GdkEvent *)>(events, "event", [=](GtkEventControllerLegacy *, GdkEvent *event) -> gboolean {
    const auto type = gdk_event_get_event_type(event);
    if (type == GDK_TOUCH_UPDATE || type == GDK_SCROLL || type == GDK_TOUCHPAD_PINCH
        || (type == GDK_MOTION_NOTIFY && (gdk_event_get_modifier_state(event) & GDK_BUTTON1_MASK))) {
      touched();
    }
    return FALSE;
  });
  gtk_widget_add_controller(map, events);
}

void MapOnCenterChanged(GtkWidget *map, Fn<void()> moved) {
  auto *viewport = shumate_simple_map_get_viewport(SHUMATE_SIMPLE_MAP(map));
  Connect<void(GObject *, GParamSpec *)>(viewport, "notify::longitude", [=](GObject *, GParamSpec *) { moved(); });
  Connect<void(GObject *, GParamSpec *)>(viewport, "notify::latitude", [=](GObject *, GParamSpec *) { moved(); });
}

#endif  // CONTRAIL_SHUMATE

GtkWidget *MakeMapPin(GtkWidget *inside) {
  // The widget is twice as high as the pin: its middle, which goes over the place, is the pin's tip.
  constexpr auto kSize = 44;
  auto *overlay = gtk_overlay_new();
  auto *pin = gtk_image_new_from_icon_name("contrail-map-pin-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(pin), kSize);
  gtk_widget_add_css_class(pin, "map-pin");
  gtk_overlay_set_child(GTK_OVERLAY(overlay), pin);
  if (inside) {
    gtk_widget_set_halign(inside, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(inside, GTK_ALIGN_START);
    gtk_widget_set_margin_top(inside, 1);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), inside);
    gtk_widget_add_css_class(overlay, "map-pin-avatar");
  }
  gtk_widget_set_margin_bottom(overlay, kSize);
  gtk_widget_set_can_target(overlay, FALSE);
  gtk_widget_set_halign(overlay, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(overlay, GTK_ALIGN_CENTER);
  return overlay;
}

GtkWidget *MakePositionDot() {
  auto *dot = adw_bin_new();
  gtk_widget_add_css_class(dot, "map-position");
  gtk_widget_set_size_request(dot, 18, 18);
  gtk_widget_set_can_target(dot, FALSE);
  gtk_widget_set_halign(dot, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
  return dot;
}

void OpenInMapsApp(double lat, double lon) {
  OpenUri(("geo:" + QString::number(lat, 'f', 6) + ',' + QString::number(lon, 'f', 6)).toStdString());
}

QString FormatCoordinates(double lat, double lon) {
  return QString::number(lat, 'f', 5) + u", "_q + QString::number(lon, 'f', 5);
}

QString FormatDistance(double meters) {
  if (meters < 1000.) {
    return QString::number(int(std::lround(meters))) + ' ' + Translated("m", "м", "м");
  }
  const auto km = meters / 1000.;
  return QLocale().toString(km, 'f', km < 10. ? 1 : 0) + ' ' + Translated("km", "км", "км");
}

}  // namespace Contrail
