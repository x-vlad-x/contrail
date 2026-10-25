#pragma once

#include "app.hpp"
#include "location.hpp"

#include "base/basic_types.h"

class HistoryItem;
class PeerData;

namespace Data {
class Thread;
class MediaLocation;
} // namespace Data

namespace Contrail {

class MainWindow;

// Sending a location, as the phone apps do: a map (with libshumate) following the device's position, "Send My
// Current Location" (or the place under the pin after the map was moved), a live location for 15 minutes,
// an hour, 8 hours or until it is stopped, and places nearby or found by a search (Telegram's venue bot).
// The location service runs only while the picker is on the screen and the window is active.
void ShowLocationPicker(not_null<MainWindow *> window, not_null<Data::Thread *> thread, FullMsgId replyTo,
                        Fn<void()> sent);

// A location from a message on a map: the place, the address, how far away it is (after "my location" was
// tapped: the device's position isn't asked for otherwise), "Open in Maps". A live location moves on the map
// as it is updated, and one being shared from here can be stopped. Without maps, opens the system's map app.
void ShowLocation(not_null<MainWindow *> window, not_null<HistoryItem *> item);

// A round button over a map.
[[nodiscard]] GtkWidget *MapButton(const char *icon, const QString &tooltip);
#ifdef CONTRAIL_SHUMATE
// + and - in the top right corner, where libshumate has its own.
[[nodiscard]] GtkWidget *MakeZoomButtons(GtkWidget *map);
#endif  // CONTRAIL_SHUMATE
[[nodiscard]] QString MyLocationText();
[[nodiscard]] QString FailureText(LocationService::Failure failure);
// Live locations can't go to channels and to Saved Messages, as in the phone apps.
[[nodiscard]] bool CanShareLive(not_null<PeerData *> peer);
// 15 minutes, an hour, 8 hours or until it is turned off.
void ChooseLivePeriod(GtkWidget *parent, Fn<void(TimeId)> chosen);

// A live location's state, as Telegram Desktop shows it.
[[nodiscard]] Data::MediaLocation *LocationMedia(not_null<const HistoryItem *> item);
[[nodiscard]] bool LiveLocationActive(not_null<const HistoryItem *> item);
[[nodiscard]] QString LiveLocationUpdated(not_null<const HistoryItem *> item);  // "updated 5 minutes ago"
[[nodiscard]] QString LiveLocationRemaining(not_null<const HistoryItem *> item);  // "14m", "8h", "∞"
[[nodiscard]] double LiveLocationProgress(not_null<const HistoryItem *> item);  // The part of its time left.

}  // namespace Contrail
