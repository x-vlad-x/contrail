#pragma once

#include "app.hpp"

#include <vector>

enum class SendMediaType;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
struct PreparedGroup;
} // namespace Ui

namespace Api {
struct SendAction;
} // namespace Api

namespace Contrail {

// Sends files group by group (an album of up to ten, or a file on its own), each group once the one
// before it got its place in the chat from the server. An album waits for all its uploads: a small
// file chosen after it would come first otherwise. The queue belongs to the session, so closing the
// chat or the window doesn't stop it; groups sent to one chat keep their order across sends.
void SendFileGroups(not_null<Main::Session *> session, std::vector<Ui::PreparedGroup> &&groups, SendMediaType type,
                    const Api::SendAction &action);

}  // namespace Contrail
