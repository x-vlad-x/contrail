#pragma once

#include <QtGui/QGuiApplication>

namespace Contrail {

inline void KeepGtkApplicationRunning() {
  // GTK owns the visible application window. The offscreen Qt call panel is often Qt's last
  // visible window; closing it must not quit the shared event loop.
  QGuiApplication::setQuitOnLastWindowClosed(false);
}

} // namespace Contrail
