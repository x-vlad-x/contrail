#pragma once

#include <gio/gio.h>
#include <QtCore/QString>
#include <functional>

namespace Contrail {

// An icon in the desktop's notification area (StatusNotifierItem, as KDE Plasma, GNOME with the
// AppIndicator extension, Xfce and most panels show it) while the app runs in the background.
// Phone shells have no such area: there, opening the app from the launcher brings the window back.
class TrayIcon final {
public:
  struct Callbacks {
    std::function<void()> toggle;
    std::function<void()> open;
    std::function<void()> quit;
  };
  explicit TrayIcon(Callbacks callbacks);
  ~TrayIcon();
  TrayIcon(const TrayIcon &) = delete;
  TrayIcon &operator=(const TrayIcon &) = delete;

  // The number of unread chats: a badge on the icon and in its tooltip.
  void setUnread(int count);

private:
  static void HandleMethod(GDBusConnection *, const char *, const char *, const char *interface, const char *method,
      GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data);
  static GVariant *HandleProperty(GDBusConnection *, const char *, const char *, const char *interface,
      const char *property, GError **, gpointer data);
  void registerObjects();
  void registerWithWatcher();
  void emit(const char *path, const char *interface, const char *signal, GVariant *parameters = nullptr);
  [[nodiscard]] GVariant *menuLayout() const;
  [[nodiscard]] QString openLabel() const;
  [[nodiscard]] QString quitLabel() const;

  Callbacks _callbacks;
  GDBusConnection *_bus = nullptr;
  GCancellable *_cancellable = nullptr;
  QString _serviceName;
  guint _itemId = 0;
  guint _menuId = 0;
  guint _ownerId = 0;
  guint _watcherId = 0;
  guint _menuRevision = 1;
  int _unread = 0;
};

} // namespace Contrail
