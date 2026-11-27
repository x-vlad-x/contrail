#pragma once

#include "app.hpp"
#include "location.hpp"
#include "td/store.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace Contrail {

class MainWindow;

// Where a location goes from the picker: the chat (or the topic of it), with the reply chosen in the field.
struct LocationTarget {
  Td::ChatId chat = 0;
  std::int32_t topic = 0;
  std::int64_t replyTo = 0;
  // Sends the content as the chat sends a message (to the topic, as a reply); the picker closes after it.
  std::function<void(Td::api::object_ptr<Td::api::InputMessageContent> content)> send;
};

// Sending a location, as the phone apps do: a map (with libshumate) following the device's position, "Send My
// Current Location" (or the place under the pin after the map was moved), a live location for 15 minutes,
// an hour, 8 hours or until it is stopped, and places nearby or found by a search (Telegram's venue bot).
// The location service runs only while the picker is on the screen and the window is active.
void ShowLocationPicker(MainWindow &window, LocationTarget target);

// A location from a message on a map: the place, the address, how far away it is (after "my location" was
// tapped: the device's position isn't asked for otherwise), "Open in Maps". A live location moves on the map
// as it is updated, and one being shared from here can be stopped. Without maps, opens the system's map app.
void ShowLocation(MainWindow &window, const Td::api::message &message);

// A round button over a map.
[[nodiscard]] GtkWidget *MapButton(const char *icon, const std::string &tooltip);
#ifdef CONTRAIL_SHUMATE
// + and - in the top right corner, where libshumate has its own.
[[nodiscard]] GtkWidget *MakeZoomButtons(GtkWidget *map);
#endif  // CONTRAIL_SHUMATE
[[nodiscard]] std::string MyLocationText();
[[nodiscard]] std::string FailureText(LocationService::Failure failure);
// Live locations can't go to channels and to Saved Messages, as in the phone apps.
[[nodiscard]] bool CanShareLive(const Td::Store &store, const Td::Chat &chat);
// 15 minutes, an hour, 8 hours or until it is turned off.
void ChooseLivePeriod(GtkWidget *parent, std::function<void(std::int32_t period)> chosen);

// What a message's location is: a place, a venue or a live location; null for other messages.
struct LocationInfo {
  double lat = 0.;
  double lon = 0.;
  std::string title;        // A venue's name.
  std::string address;
  std::int32_t livePeriod = 0;  // 0: not live. kUntilOff: until it is stopped.
};
[[nodiscard]] bool MessageLocation(const Td::api::message &message, LocationInfo *info);
// A live location's state, as Telegram Desktop shows it.
[[nodiscard]] bool LiveLocationActive(const Td::api::message &message);
[[nodiscard]] std::string LiveLocationUpdated(const Td::api::message &message);  // "updated 5 minutes ago"
[[nodiscard]] std::string LiveLocationRemaining(const Td::api::message &message);  // "14m", "8h", "∞"
[[nodiscard]] double LiveLocationProgress(const Td::api::message &message);  // The part of its time left.
// Only what the maps read: the location, the dates and the sender (a live location's message is kept while it moves).
[[nodiscard]] Td::api::object_ptr<Td::api::message> CopyLocationMessage(const Td::api::message &message);
// Whoever sent it: a user, or the chat of a channel.
[[nodiscard]] std::string SenderName(const Td::Store &store, const Td::api::message &message);
[[nodiscard]] GtkWidget *MakeSenderAvatar(Td::Store &store, const Td::api::message &message, int size);

// The debug commands loc-… for the picker or the map shown (debug.cpp). False when it isn't one of them.
bool LocationDebugCommand(MainWindow &window, const std::string &name, const std::string &rest);

constexpr auto kUntilOff = std::int32_t(0x7FFFFFFF);

}  // namespace Contrail
