#pragma once

#include "app.hpp"

#include "base/basic_types.h"

#include <utility>
#include <vector>

namespace Contrail {

// Interactive maps, with libshumate (as GNOME Maps) and OpenStreetMap's tiles. As OpenStreetMap's tile policy
// asks: the requests name Contrail in their User-Agent, the tiles are cached on disk (libshumate's cache) and the
// attribution stays on the map. Built without libshumate (CONTRAIL_SHUMATE), there are no maps in the app and
// locations open in the system's map application.
[[nodiscard]] bool MapsAvailable();

#ifdef CONTRAIL_SHUMATE

// A ShumateSimpleMap: pinch, drag, the zoom buttons, the scale and the attribution.
[[nodiscard]] GtkWidget *MakeMap();
void MapCenterOn(GtkWidget *map, double lat, double lon, double zoom, bool animate);
// A whole level in or out, animated, around the middle of the map (the phone apps' + and -).
void MapZoomBy(GtkWidget *map, int levels);
[[nodiscard]] GtkWidget *MakeMapZoomButtons(GtkWidget *map);
void MapCenter(GtkWidget *map, double *lat, double *lon);
[[nodiscard]] double MapZoom(GtkWidget *map);
[[nodiscard]] double MapRotation(GtkWidget *map);
// A widget on the map at a place (its child's center there), on the map's own marker layer.
GtkWidget *MapAddMarker(GtkWidget *map, GtkWidget *child, double lat, double lon);
void MapMoveMarker(GtkWidget *marker, double lat, double lon);
void MapRemoveMarker(GtkWidget *map, GtkWidget *marker);
// Shows all the points (FitPoints in geo.hpp) as soon as the map has its size, with room around them for the pins.
void MapShowAll(GtkWidget *map, const std::vector<std::pair<double, double>> &points, double maxZoom, bool animate);
// Calls `touched` when the user starts moving or zooming the map (not when the app moves it).
void MapOnUserMove(GtkWidget *map, Fn<void()> touched);
// Calls `moved` when the middle of the map changes.
void MapOnCenterChanged(GtkWidget *map, Fn<void()> moved);

#endif  // CONTRAIL_SHUMATE

// A pin with its tip in the middle of the widget (over the place, as a marker or in the middle of a map).
[[nodiscard]] GtkWidget *MakeMapPin(GtkWidget *inside = nullptr);
// The device's position: a dot in a circle as large as the fix's accuracy.
[[nodiscard]] GtkWidget *MakePositionDot();

// "geo:" for the system's map application (GNOME Maps, Organic Maps...).
void OpenInMapsApp(double lat, double lon);
// 55.75580, 37.61730
[[nodiscard]] QString FormatCoordinates(double lat, double lon);
// 120 m, 2.4 km
[[nodiscard]] QString FormatDistance(double meters);

}  // namespace Contrail
