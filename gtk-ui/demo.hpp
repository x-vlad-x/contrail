#pragma once

namespace Contrail {

// Screenshots without anybody's data: signs a fictional account in locally and fills it with made-up
// English chats, a group and a channel with generated pictures. Only for a separate, empty data
// directory of a process without network access (see tools/demo.sh): the core keeps everything in
// memory and the server never hears of it. Started by the "demo" debug command.
void StartDemo();
// The demo runs offline: no "Connecting..." in the header.
[[nodiscard]] bool DemoActive();

}  // namespace Contrail
