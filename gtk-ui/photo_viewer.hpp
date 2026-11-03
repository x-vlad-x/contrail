#pragma once

#include "td/store.hpp"

#include <gtk/gtk.h>

#include <cstdint>
#include <string>

namespace Contrail {

class ChatView;
class MainWindow;

// The photo of a message over the chat, as 0.3.1 showed it: black, with Save in the header, zoomed by a pinch up to
// 6x, moved by a drag, a double tap and Ctrl+wheel (zoom_view.hpp). The blurred thumbnail and the sizes already
// downloaded come first, the largest size is downloaded and takes their place. It closes with the chat; its
// textures go when it closes.
void ShowPhotoViewer(ChatView &chat, std::int64_t message, const Td::api::photo &photo);

// For the debug actions: "state" (the size shown, the zoom, the textures alive), "save", "close", or what zoom_view.hpp's
// ZoomViewDebug takes.
void PhotoViewerDebug(MainWindow &window, const std::string &action);

}  // namespace Contrail
