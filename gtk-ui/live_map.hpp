#pragma once

#include "app.hpp"

#include "base/basic_types.h"

#include <vector>

class HistoryItem;

namespace Data {
class Thread;
} // namespace Data

namespace Contrail {

class MainWindow;

// Everyone sharing a live location in a chat on one map, so that people can find each other, as the phone apps
// show it: their pins, this device's own position, a list with how far away each of them is, and sharing one's
// own live location (or stopping it) from there. The map shows everybody until it is moved by hand. While it is
// open the chat's live locations are asked from the server every half a minute; the device's position is taken
// while it is open if a live location goes to this chat from here, otherwise after "my location" is tapped.
void ShowLiveMap(not_null<MainWindow *> window, not_null<Data::Thread *> thread);

// "Alice, Bob and you" for the chat's live location bar.
[[nodiscard]] QString LiveLocationNames(const std::vector<not_null<HistoryItem *>> &items);

}  // namespace Contrail
