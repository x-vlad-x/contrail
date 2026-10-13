#pragma once

#include "app.hpp"

namespace Contrail {

// Remote control over D-Bus for testing on the phone (CONTRAIL_DEBUG=1), see debug.cpp.
void RegisterDebugActions(MainWindow &window);

}  // namespace Contrail
