#pragma once

#include "app.hpp"

class PeerData;

namespace Contrail {

// AdwAvatar for a chat: initials until the userpic is downloaded, then the picture.
// Saved Messages and Replies get their icons.
GtkWidget *MakeAvatar(not_null<PeerData *> peer, int size, bool forSavedMessages = false);
void BindAvatar(AdwAvatar *avatar, not_null<PeerData *> peer, bool forSavedMessages = false);
// The window was closed: the cached pictures go (the avatars showing them already went).
void ReleaseAvatars();
[[nodiscard]] int64 AvatarCacheBytes();

}  // namespace Contrail
