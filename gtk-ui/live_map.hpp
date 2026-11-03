#pragma once

#include "app.hpp"
#include "td/store.hpp"

#include <string>

namespace Contrail {

class MainWindow;

// Everyone sharing a live location in a chat on one map, so that people can find each other, as the phone apps
// show it: their pins, this device's own position, a list with how far away each of them is, and sharing one's
// own live location (or stopping it) from there. The map shows everybody until it is moved by hand. While it is
// open the chat's live locations are asked for every half a minute; the device's position is taken while it is
// open if a live location goes to this chat from here, otherwise after "my location" is tapped. Without maps,
// the list alone.
void ShowLiveMap(MainWindow &window, Td::ChatId chat);

// The debug commands live-… for the shared map shown (debug.cpp). False when it isn't one of them.
bool LiveMapDebugCommand(MainWindow &window, const std::string &name, const std::string &rest);

}  // namespace Contrail
