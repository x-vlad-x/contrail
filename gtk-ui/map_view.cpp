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

#include <algorithm>
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

constexpr auto kZoomStepMs = 250;  // The zoom buttons' animation.

ShumateMarkerLayer *MarkerLayer(GtkWidget *map) {
  return SHUMATE_MARKER_LAYER(g_object_get_data(G_OBJECT(map), "contrail-markers"));
}

ShumateMap *View(GtkWidget *map) {
  return shumate_simple_map_get_map(SHUMATE_SIMPLE_MAP(map));
}

// A zoom step still going on: the next press of a button goes on from where this one ends.
struct ZoomStep {
  double target = 0.;
  gint64 until = 0;
};

// libshumate rotates the map with two fingers together with pinching, and snaps it back to the north only on
// one side: a pinch with fingers turning a little made the map jump by some 14 degrees. A messenger's map
// stays north up, as in the phone apps.
void DisableRotation(GtkWidget *map) {
  auto *controllers = gtk_widget_observe_controllers(GTK_WIDGET(View(map)));
  const auto count = g_list_model_get_n_items(controllers);
  for (guint i = 0; i != count; ++i) {
    auto *controller = GTK_EVENT_CONTROLLER(g_list_model_get_item(controllers, i));
    if (GTK_IS_GESTURE_ROTATE(controller)) {
      gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_NONE);
    }
    g_object_unref(controller);
  }
  g_object_unref(controllers);
}

}  // namespace

GtkWidget *MakeMap() {
  auto *map = shumate_simple_map_new();
  shumate_simple_map_set_map_source(map, OpenStreetMap());
  auto *viewport = shumate_simple_map_get_viewport(map);
  shumate_viewport_set_max_zoom_level(viewport, 19);
  // The scale would cover the attribution on a phone's width (the phone apps show none either).
  gtk_widget_set_visible(GTK_WIDGET(shumate_simple_map_get_scale(map)), FALSE);
  // libshumate's buttons zoom by a fifth of a level (a step of the mouse wheel): MakeMapZoomButtons() has
  // the phone apps' whole level.
  shumate_simple_map_set_show_zoom_buttons(map, FALSE);
  DisableRotation(GTK_WIDGET(map));
  // A finger on the map takes it over: the app's own moves (following the device's position, a step of the
  // zoom buttons) stop, or the animation would pull the map back during a pinch.
  auto *events = gtk_event_controller_legacy_new();
  gtk_event_controller_set_propagation_phase(events, GTK_PHASE_CAPTURE);
  Connect<gboolean(GtkEventControllerLegacy *, GdkEvent *)>(events, "event", [map](GtkEventControllerLegacy *, GdkEvent *event) -> gboolean {
    const auto type = gdk_event_get_event_type(event);
    if (type == GDK_TOUCH_BEGIN || type == GDK_BUTTON_PRESS || type == GDK_SCROLL || type == GDK_TOUCHPAD_PINCH) {
      shumate_map_stop_go_to(shumate_simple_map_get_map(map));
    }
    return FALSE;
  });
  gtk_widget_add_controller(GTK_WIDGET(map), events);
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
    shumate_map_stop_go_to(view);
    shumate_viewport_set_zoom_level(shumate_simple_map_get_viewport(simple), zoom);
    shumate_map_center_on(view, lat, lon);
  }
}

void MapZoomBy(GtkWidget *map, int levels) {
  auto *viewport = shumate_simple_map_get_viewport(SHUMATE_SIMPLE_MAP(map));
  auto *step = static_cast<ZoomStep *>(g_object_get_data(G_OBJECT(map), "contrail-zoom-step"));
  if (!step) step = AttachData(map, "contrail-zoom-step", std::make_unique<ZoomStep>());
  const auto now = g_get_monotonic_time();
  const auto from = (now < step->until) ? step->target : std::round(shumate_viewport_get_zoom_level(viewport));
  const auto to = std::clamp(from + levels, double(shumate_viewport_get_min_zoom_level(viewport)),
                             double(shumate_viewport_get_max_zoom_level(viewport)));
  auto *location = SHUMATE_LOCATION(viewport);
  shumate_map_go_to_full_with_duration(View(map), shumate_location_get_latitude(location),
                                       shumate_location_get_longitude(location), to, kZoomStepMs);
  *step = ZoomStep{.target = to, .until = now + kZoomStepMs * 1000};
}

GtkWidget *MakeMapZoomButtons(GtkWidget *map) {
  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(box, "linked");
  gtk_widget_add_css_class(box, "map-zoom");
  const auto add = [&](const char *icon, const QString &tooltip, int levels) {
    auto *button = MakeIconButton(icon, U8(tooltip).c_str(), "osd");
    gtk_widget_add_css_class(button, "map-button");
    Connect<void(GtkButton *)>(button, "clicked", [map, levels](GtkButton *) { MapZoomBy(map, levels); });
    gtk_box_append(GTK_BOX(box), button);
  };
  add("zoom-in-symbolic", Translated("Zoom In", "Приблизить", "Наблизити"), 1);
  add("zoom-out-symbolic", Translated("Zoom Out", "Отдалить", "Віддалити"), -1);
  return box;
}

void MapCenter(GtkWidget *map, double *lat, double *lon) {
  auto *location = SHUMATE_LOCATION(shumate_simple_map_get_viewport(SHUMATE_SIMPLE_MAP(map)));
  *lat = shumate_location_get_latitude(location);
  *lon = shumate_location_get_longitude(location);
}

double MapZoom(GtkWidget *map) {
  return shumate_viewport_get_zoom_level(shumate_simple_map_get_viewport(SHUMATE_SIMPLE_MAP(map)));
}

double MapRotation(GtkWidget *map) {
  return shumate_viewport_get_rotation(shumate_simple_map_get_viewport(SHUMATE_SIMPLE_MAP(map)));
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
