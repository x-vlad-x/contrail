#pragma once

#include "app.hpp"
#include "td/store.hpp"

#include <cstdint>

namespace Contrail {

// AdwAvatar for a chat or a user: initials until the small picture is downloaded, then the picture. Binding again
// repoints a recycled avatar. Saved Messages gets its icon when asked for.
GtkWidget *MakeAvatar(Td::Store &store, Td::ChatId chat, int size, bool forSavedMessages = false);
GtkWidget *MakeUserAvatar(Td::Store &store, Td::UserId user, int size);
void BindAvatar(AdwAvatar *avatar, Td::Store &store, Td::ChatId chat, bool forSavedMessages = false);
void BindUserAvatar(AdwAvatar *avatar, Td::Store &store, Td::UserId user);
// The avatar shows something else now: a download still running must not land in it.
void UnbindAvatar(AdwAvatar *avatar);
// The cached pictures go; avatars still showing one keep it until they go too.
void ReleaseAvatars();
[[nodiscard]] std::int64_t AvatarCacheBytes();

}  // namespace Contrail
