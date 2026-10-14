#include "qt_integration.hpp"

#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>
#include <QtCore/QTimer>
#include <cstdio>

int main(int argc, char **argv) {
  const auto baseline = argc > 1;
  QApplication app(argc, argv);
  if (!baseline) Contrail::KeepGtkApplicationRunning();
  QWidget hiddenCoreWindow;
  QWidget callPanel;
  callPanel.show();
  auto survived = false;
  QTimer::singleShot(0, &callPanel, &QWidget::close);
  QTimer::singleShot(100, &app, [&] {
    survived = true;
    app.quit();
  });
  app.exec();
  if (survived == baseline) return 1;
  std::puts(baseline ? "Qt baseline: closing the call panel quits the application"
      : "Qt regression: application survives closing the call panel");
}
