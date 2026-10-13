#pragma once

#include "app.hpp"

class PeerData;

namespace Contrail {

// AdwAvatar for a chat: initials until the userpic is downloaded, then the picture.
// Saved Messages and Replies get their icons.
GtkWidget *MakeAvatar(not_null<PeerData *> peer, int size, bool forSavedMessages = false);
void BindAvatar(AdwAvatar *avatar, not_null<PeerData *> peer, bool forSavedMessages = false);

}  // namespace Contrail
