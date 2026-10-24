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
#include <string>
#include <vector>

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

// libshumate's drag and pinch keep the place they hold under the fingers in one variable. When the second finger
// comes, the drag starts again on it (after the pinch began) and overwrites the pinch's place: the two then move
// the map to their own places in turn, 200 pixels apart with every event, and the swipe on that finger flings the
// map when the pinch ends. During a pinch libshumate's drag and swipe are blocked, until the next first touch.
struct PinchGuard {
  ShumateMap *view = nullptr;
  GtkGesture *zoom = nullptr;
  std::vector<GtkGesture *> blocked;  // libshumate's drag and swipe.
  bool blocking = false;

  void block() {
    if (blocking) return;
    blocking = true;
    for (auto *gesture : blocked) {
      g_signal_handlers_block_matched(gesture, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, view);
    }
  }
  void unblock() {
    if (!blocking) return;
    blocking = false;
    for (auto *gesture : blocked) {
      g_signal_handlers_unblock_matched(gesture, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, view);
    }
  }
};

// Called before the map's own gestures see the event (capture phase).
void PinchGuardEvent(GtkWidget *map, GdkEventType type) {
  auto *guard = static_cast<PinchGuard *>(g_object_get_data(G_OBJECT(map), "contrail-pinch-guard"));
  if (!guard) return;
  // A touch with no gesture going on starts from no fingers: libshumate's own again (its drag and swipe start on
  // this touch). Not when the last finger of a pinch is lifted: the swipe would fling the map.
  if (type != GDK_TOUCH_BEGIN || !guard->blocking) return;
  const auto active = [](GtkGesture *gesture) { return gesture && gtk_gesture_is_active(gesture); };
  if (active(guard->zoom) || std::any_of(guard->blocked.begin(), guard->blocked.end(), active)) return;
  guard->unblock();
}

void SetupPinchGuard(GtkWidget *map) {
  auto *view = View(map);
  auto *guard = AttachData(map, "contrail-pinch-guard", std::make_unique<PinchGuard>());
  guard->view = view;
  GtkGesture *zoom = nullptr;
  auto *controllers = gtk_widget_observe_controllers(GTK_WIDGET(view));
  const auto count = g_list_model_get_n_items(controllers);
  for (guint i = 0; i != count; ++i) {
    auto *controller = GTK_EVENT_CONTROLLER(g_list_model_get_item(controllers, i));
    if (GTK_IS_GESTURE_ZOOM(controller)) {
      zoom = guard->zoom = GTK_GESTURE(controller);
    } else if (GTK_IS_GESTURE_DRAG(controller) || GTK_IS_GESTURE_SWIPE(controller)) {
      guard->blocked.push_back(GTK_GESTURE(controller));  // The map holds them as long as the guard.
    }
    g_object_unref(controller);
  }
  g_object_unref(controllers);
  if (zoom) {
    Connect<void(GtkGesture *, GdkEventSequence *)>(zoom, "begin", [guard](GtkGesture *, GdkEventSequence *) { guard->block(); });
  }
}

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

// CONTRAIL_MAP_TRACE=1: every touch on a map, every gesture of libshumate's and every change of the view, with
// the map's size, to find out what moves it.
bool Tracing() {
  static const auto result = g_getenv("CONTRAIL_MAP_TRACE") != nullptr;
  return result;
}

const char *EventName(GdkEventType type) {
  switch (type) {
    case GDK_TOUCH_BEGIN: return "touch-begin";
    case GDK_TOUCH_UPDATE: return "touch-update";
    case GDK_TOUCH_END: return "touch-end";
    case GDK_TOUCH_CANCEL: return "touch-cancel";
    case GDK_BUTTON_PRESS: return "press";
    case GDK_BUTTON_RELEASE: return "release";
    case GDK_MOTION_NOTIFY: return "motion";
    case GDK_SCROLL: return "scroll";
    default: return nullptr;
  }
}

void Trace(GtkWidget *map, const char *what) {
  auto *viewport = shumate_simple_map_get_viewport(SHUMATE_SIMPLE_MAP(map));
  auto *location = SHUMATE_LOCATION(viewport);
  g_message("MapTrace %lld %s | center=%.7f,%.7f zoom=%.4f rotation=%.4f size=%dx%d goto=%u",
            (long long)(g_get_monotonic_time() / 1000), what, shumate_location_get_latitude(location),
            shumate_location_get_longitude(location), shumate_viewport_get_zoom_level(viewport),
            shumate_viewport_get_rotation(viewport), gtk_widget_get_width(map), gtk_widget_get_height(map),
            shumate_map_get_go_to_duration(View(map)));
}

void SetupTrace(GtkWidget *map) {
  auto *viewport = shumate_simple_map_get_viewport(SHUMATE_SIMPLE_MAP(map));
  Connect<void(GObject *, GParamSpec *)>(viewport, "notify", [map](GObject *, GParamSpec *spec) {
    Trace(map, (std::string("view ") + g_param_spec_get_name(spec)).c_str());
  });
  auto *controllers = gtk_widget_observe_controllers(GTK_WIDGET(View(map)));
  const auto count = g_list_model_get_n_items(controllers);
  for (guint i = 0; i != count; ++i) {
    auto *controller = GTK_EVENT_CONTROLLER(g_list_model_get_item(controllers, i));
    if (GTK_IS_GESTURE(controller)) {
      const auto name = std::string(G_OBJECT_TYPE_NAME(controller));
      for (const auto signal : {"begin", "end", "cancel"}) {
        Connect<void(GtkGesture *, GdkEventSequence *)>(controller, signal, [map, name, signal](GtkGesture *, GdkEventSequence *sequence) {
          Trace(map, (name + ' ' + signal + " seq=" + std::to_string(reinterpret_cast<guintptr>(sequence))).c_str());
        });
      }
    }
    g_object_unref(controller);
  }
  g_object_unref(controllers);
  Connect<void(GtkWidget *)>(map, "map", [map](GtkWidget *) { Trace(map, "shown"); });
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
  SetupPinchGuard(GTK_WIDGET(map));
  // A finger on the map takes it over: the app's own moves (following the device's position, a step of the
  // zoom buttons) stop, or the animation would pull the map back during a pinch.
  auto *events = gtk_event_controller_legacy_new();
  gtk_event_controller_set_propagation_phase(events, GTK_PHASE_CAPTURE);
  Connect<gboolean(GtkEventControllerLegacy *, GdkEvent *)>(events, "event", [map](GtkEventControllerLegacy *, GdkEvent *event) -> gboolean {
    const auto type = gdk_event_get_event_type(event);
    if (Tracing()) {
      if (const auto name = EventName(type)) {
        auto x = 0., y = 0.;
        gdk_event_get_position(event, &x, &y);
        Trace(GTK_WIDGET(map), (std::string(name) + " seq=" + std::to_string(reinterpret_cast<guintptr>(gdk_event_get_event_sequence(event)))
                                + " at=" + std::to_string(int(x)) + ',' + std::to_string(int(y))).c_str());
      }
    }
    PinchGuardEvent(GTK_WIDGET(map), type);
    if (type == GDK_TOUCH_BEGIN || type == GDK_BUTTON_PRESS || type == GDK_SCROLL || type == GDK_TOUCHPAD_PINCH) {
      shumate_map_stop_go_to(shumate_simple_map_get_map(map));
    }
    return FALSE;
  });
  gtk_widget_add_controller(GTK_WIDGET(map), events);
  if (Tracing()) SetupTrace(GTK_WIDGET(map));
  auto *markers = shumate_marker_layer_new(viewport);
  shumate_simple_map_add_overlay_layer(map, SHUMATE_LAYER(markers));
  g_object_set_data(G_OBJECT(map), "contrail-markers", markers);
  gtk_widget_add_css_class(GTK_WIDGET(map), "contrail-map");
  gtk_widget_set_hexpand(GTK_WIDGET(map), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(map), TRUE);
  return GTK_WIDGET(map);
}

void MapCenterOn(GtkWidget *map, double lat, double lon, double zoom, bool animate) {
  if (Tracing()) {
    Trace(map, ("app center-on " + std::to_string(lat) + ',' + std::to_string(lon) + " zoom=" + std::to_string(zoom)
                + (animate ? " animated" : "")).c_str());
  }
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
  if (Tracing()) Trace(map, ("app zoom-to " + std::to_string(to)).c_str());
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
