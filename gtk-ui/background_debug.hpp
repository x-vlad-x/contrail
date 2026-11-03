// The debug commands of the settings and the app in the background: bg-, settings-, notify-, proxy-, mem-.
#pragma once

#include <string>

namespace Contrail {

class MainWindow;

// False when the command isn't one of these.
bool RunBackgroundCommand(MainWindow &window, const std::string &name, const std::string &rest);

}  // namespace Contrail
