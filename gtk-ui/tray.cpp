#include "tray.hpp"

#include "app.hpp"

#include "lang/lang_keys.h"

#include <QtCore/QtEndian>
#include <QtGui/QPainter>
#include <QtSvg/QSvgRenderer>
#include <algorithm>
#include <unistd.h>

namespace Contrail {
namespace {

constexpr auto kItemPath = "/StatusNotifierItem";
constexpr auto kMenuPath = "/StatusNotifierItem/Menu";
constexpr auto kItemInterface = "org.kde.StatusNotifierItem";
constexpr auto kMenuInterface = "com.canonical.dbusmenu";
constexpr auto kWatcherName = "org.kde.StatusNotifierWatcher";

enum MenuItem { kRoot = 0, kOpen = 1, kSeparator = 2, kQuit = 3 };

constexpr auto kIntrospection = R"xml(<node>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="WindowId" type="i" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconThemePath" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <property name="OverlayIconName" type="s" access="read"/>
    <property name="AttentionIconName" type="s" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <method name="ContextMenu"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="Activate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="SecondaryActivate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="Scroll"><arg name="delta" type="i" direction="in"/><arg name="orientation" type="s" direction="in"/></method>
    <signal name="NewTitle"/>
    <signal name="NewIcon"/>
    <signal name="NewToolTip"/>
    <signal name="NewStatus"><arg name="status" type="s"/></signal>
  </interface>
  <interface name="com.canonical.dbusmenu">
    <property name="Version" type="u" access="read"/>
    <property name="TextDirection" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconThemePath" type="as" access="read"/>
    <method name="GetLayout">
      <arg name="parentId" type="i" direction="in"/><arg name="recursionDepth" type="i" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="revision" type="u" direction="out"/><arg name="layout" type="(ia{sv}av)" direction="out"/>
    </method>
    <method name="GetGroupProperties">
      <arg name="ids" type="ai" direction="in"/><arg name="propertyNames" type="as" direction="in"/>
      <arg name="properties" type="a(ia{sv})" direction="out"/>
    </method>
    <method name="GetProperty">
      <arg name="id" type="i" direction="in"/><arg name="name" type="s" direction="in"/>
      <arg name="value" type="v" direction="out"/>
    </method>
    <method name="Event">
      <arg name="id" type="i" direction="in"/><arg name="eventId" type="s" direction="in"/>
      <arg name="data" type="v" direction="in"/><arg name="timestamp" type="u" direction="in"/>
    </method>
    <method name="EventGroup">
      <arg name="events" type="a(isvu)" direction="in"/><arg name="idErrors" type="ai" direction="out"/>
    </method>
    <method name="AboutToShow"><arg name="id" type="i" direction="in"/><arg name="needUpdate" type="b" direction="out"/></method>
    <method name="AboutToShowGroup">
      <arg name="ids" type="ai" direction="in"/>
      <arg name="updatesNeeded" type="ai" direction="out"/><arg name="idErrors" type="ai" direction="out"/>
    </method>
    <signal name="ItemsPropertiesUpdated"><arg name="updatedProps" type="a(ia{sv})"/><arg name="removedProps" type="a(ias)"/></signal>
    <signal name="LayoutUpdated"><arg name="revision" type="u"/><arg name="parent" type="i"/></signal>
    <signal name="ItemActivationRequested"><arg name="id" type="i"/><arg name="timestamp" type="u"/></signal>
  </interface>
</node>)xml";

GDBusNodeInfo *Introspection() {
  static auto *info = g_dbus_node_info_new_for_xml(kIntrospection, nullptr);
  return info;
}

// The app's icon, drawn by the app: an AppImage's icon theme isn't visible to the panel.
QString IconFile() {
  auto *display = gdk_display_get_default();
  if (!display) return {};
  auto *icon = gtk_icon_theme_lookup_icon(gtk_icon_theme_get_for_display(display), kAppId, nullptr, 64, 1,
      GTK_TEXT_DIR_NONE, GtkIconLookupFlags(0));
  QString result;
  if (auto *file = gtk_icon_paintable_get_file(icon)) {
    if (auto *path = g_file_get_path(file)) {
      result = QString::fromUtf8(path);
      g_free(path);
    }
    g_object_unref(file);
  }
  g_object_unref(icon);
  return result;
}

const QString &AppIconFile() {
  static const auto result = IconFile();
  return result;
}

GVariant *IconPixmaps(int unread) {
  static QSvgRenderer renderer(AppIconFile());
  auto builder = g_variant_builder_new(G_VARIANT_TYPE("a(iiay)"));
  for (const auto size : {16, 22, 24, 32, 48, 64}) {
    if (!renderer.isValid()) break;
    auto image = QImage(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    auto painter = QPainter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    renderer.render(&painter, QRectF(0, 0, size, size));
    if (unread > 0) {
      // A badge with the number of unread chats, as Telegram's own tray icon has.
      const auto text = unread > 99 ? QStringLiteral("99+") : QString::number(unread);
      auto font = painter.font();
      font.setBold(true);
      font.setPixelSize(std::max(7, size * (text.size() > 2 ? 3 : 4) / 10));
      painter.setFont(font);
      const auto height = std::max(9, size * 11 / 20);
      const auto width = std::max(height, painter.fontMetrics().horizontalAdvance(text) + height / 2);
      const auto badge = QRectF(size - width, size - height, width, height);
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(0xe5, 0x39, 0x35));
      painter.drawRoundedRect(badge, height / 2., height / 2.);
      painter.setPen(Qt::white);
      painter.drawText(badge, Qt::AlignCenter, text);
    }
    painter.end();
    image = image.convertToFormat(QImage::Format_ARGB32);
    QByteArray bytes(size * size * 4, Qt::Uninitialized);
    auto *to = reinterpret_cast<quint32 *>(bytes.data());
    for (auto y = 0; y != size; ++y) {
      const auto *from = reinterpret_cast<const quint32 *>(image.constScanLine(y));
      for (auto x = 0; x != size; ++x) *to++ = qToBigEndian(from[x]); // ARGB in network byte order.
    }
    g_variant_builder_add(builder, "(ii@ay)", size, size,
        g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, bytes.constData(), bytes.size(), 1));
  }
  auto *result = g_variant_builder_end(builder);
  g_variant_builder_unref(builder);
  return result;
}

GVariant *String(const QString &value) {
  return g_variant_new_string(value.toUtf8().constData());
}

} // namespace

TrayIcon::TrayIcon(Callbacks callbacks)
    : _callbacks(std::move(callbacks)), _cancellable(g_cancellable_new()),
      _serviceName(QStringLiteral("org.kde.StatusNotifierItem-%1-1").arg(getpid())) {
  g_bus_get(G_BUS_TYPE_SESSION, _cancellable, +[](GObject *, GAsyncResult *result, gpointer data) {
    auto *bus = g_bus_get_finish(result, nullptr);
    if (!bus) return; // Cancelled or no session bus: nothing to show anyway.
    static_cast<TrayIcon *>(data)->_bus = bus;
    static_cast<TrayIcon *>(data)->registerObjects();
  }, this);
}

TrayIcon::~TrayIcon() {
  g_cancellable_cancel(_cancellable);
  if (_watcherId) g_bus_unwatch_name(_watcherId);
  if (_ownerId) g_bus_unown_name(_ownerId);
  if (_bus) {
    if (_itemId) g_dbus_connection_unregister_object(_bus, _itemId);
    if (_menuId) g_dbus_connection_unregister_object(_bus, _menuId);
    g_object_unref(_bus);
  }
  g_object_unref(_cancellable);
}

void TrayIcon::registerObjects() {
  static const GDBusInterfaceVTable vtable = {&TrayIcon::HandleMethod, &TrayIcon::HandleProperty, nullptr, {}};
  auto *info = Introspection();
  _itemId = g_dbus_connection_register_object(_bus, kItemPath,
      g_dbus_node_info_lookup_interface(info, kItemInterface), &vtable, this, nullptr, nullptr);
  _menuId = g_dbus_connection_register_object(_bus, kMenuPath,
      g_dbus_node_info_lookup_interface(info, kMenuInterface), &vtable, this, nullptr, nullptr);
  _ownerId = g_bus_own_name_on_connection(_bus, _serviceName.toUtf8().constData(), G_BUS_NAME_OWNER_FLAGS_NONE,
      nullptr, nullptr, nullptr, nullptr);
  // The panel may start, or restart, after the app.
  _watcherId = g_bus_watch_name_on_connection(_bus, kWatcherName, G_BUS_NAME_WATCHER_FLAGS_NONE,
      +[](GDBusConnection *, const char *, const char *, gpointer data) {
        static_cast<TrayIcon *>(data)->registerWithWatcher();
      }, nullptr, this, nullptr);
}

void TrayIcon::registerWithWatcher() {
  g_dbus_connection_call(_bus, kWatcherName, "/StatusNotifierWatcher", kWatcherName, "RegisterStatusNotifierItem",
      g_variant_new("(s)", _serviceName.toUtf8().constData()), nullptr, G_DBUS_CALL_FLAGS_NONE, -1, _cancellable,
      nullptr, nullptr);
}

void TrayIcon::emit(const char *path, const char *interface, const char *signal, GVariant *parameters) {
  if (_bus && _itemId) g_dbus_connection_emit_signal(_bus, nullptr, path, interface, signal, parameters, nullptr);
  else if (parameters) g_variant_unref(g_variant_ref_sink(parameters));
}

void TrayIcon::setUnread(int count) {
  if (_unread == count) return;
  _unread = count;
  emit(kItemPath, kItemInterface, "NewIcon");
  emit(kItemPath, kItemInterface, "NewTitle");
  emit(kItemPath, kItemInterface, "NewToolTip");
}

QString TrayIcon::openLabel() const {
  // The core's strings name Telegram: this is Contrail.
  return tr::lng_open_from_tray(tr::now).replace(u"Telegram"_q, u"Contrail"_q);
}

QString TrayIcon::quitLabel() const {
  return tr::lng_quit_from_tray(tr::now).replace(u"Telegram"_q, u"Contrail"_q);
}

GVariant *TrayIcon::menuLayout() const {
  const auto item = [](int id, GVariant *properties) {
    return g_variant_new("(i@a{sv}@av)", id, properties, g_variant_new_array(G_VARIANT_TYPE_VARIANT, nullptr, 0));
  };
  const auto label = [](const QString &text) {
    auto builder = g_variant_builder_new(G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(builder, "{sv}", "label", String(text));
    auto *result = g_variant_builder_end(builder);
    g_variant_builder_unref(builder);
    return result;
  };
  auto separator = g_variant_builder_new(G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(separator, "{sv}", "type", g_variant_new_string("separator"));
  GVariant *children[] = {
      g_variant_new_variant(item(kOpen, label(openLabel()))),
      g_variant_new_variant(item(kSeparator, g_variant_builder_end(separator))),
      g_variant_new_variant(item(kQuit, label(quitLabel()))),
  };
  g_variant_builder_unref(separator);
  auto root = g_variant_builder_new(G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(root, "{sv}", "children-display", g_variant_new_string("submenu"));
  auto *result = g_variant_new("(i@a{sv}@av)", int(kRoot), g_variant_builder_end(root),
      g_variant_new_array(G_VARIANT_TYPE_VARIANT, children, G_N_ELEMENTS(children)));
  g_variant_builder_unref(root);
  return result;
}

GVariant *TrayIcon::HandleProperty(GDBusConnection *, const char *, const char *, const char *interface,
    const char *property, GError **, gpointer data) {
  const auto self = static_cast<TrayIcon *>(data);
  const auto is = [&](const char *name) { return g_strcmp0(property, name) == 0; };
  if (g_strcmp0(interface, kMenuInterface) == 0) {
    if (is("Version")) return g_variant_new_uint32(3);
    if (is("TextDirection")) return g_variant_new_string("ltr");
    if (is("Status")) return g_variant_new_string("normal");
    return g_variant_new_strv(nullptr, 0);
  }
  const auto title = self->_unread > 0
      ? QStringLiteral("Contrail (%1)").arg(self->_unread)
      : QStringLiteral("Contrail");
  if (is("Category")) return g_variant_new_string("Communications");
  if (is("Id")) return g_variant_new_string(kAppId);
  if (is("Title")) return String(title);
  if (is("Status")) return g_variant_new_string("Active");
  if (is("WindowId")) return g_variant_new_int32(0);
  if (is("IconPixmap")) return IconPixmaps(self->_unread);
  // Panels prefer a theme icon over the pixmaps, which carry the unread badge.
  if (is("IconName")) return g_variant_new_string(AppIconFile().isEmpty() ? kAppId : "");
  if (is("ToolTip")) {
    return g_variant_new("(s@a(iiay)ss)", "", g_variant_new_array(G_VARIANT_TYPE("(iiay)"), nullptr, 0),
        title.toUtf8().constData(), "");
  }
  if (is("ItemIsMenu")) return g_variant_new_boolean(FALSE);
  if (is("Menu")) return g_variant_new_object_path(kMenuPath);
  return g_variant_new_string(""); // IconThemePath, OverlayIconName, AttentionIconName.
}

void TrayIcon::HandleMethod(GDBusConnection *, const char *, const char *, const char *interface,
    const char *method, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data) {
  const auto self = static_cast<TrayIcon *>(data);
  const auto is = [&](const char *name) { return g_strcmp0(method, name) == 0; };
  if (g_strcmp0(interface, kItemInterface) == 0) {
    g_dbus_method_invocation_return_value(invocation, nullptr);
    // The callbacks don't run within this call: quitting destroys the icon.
    if (is("Activate") || is("SecondaryActivate")) self->_callbacks.toggle();
    return;
  }
  const auto activate = [&](int id, const char *event) {
    if (g_strcmp0(event, "clicked") != 0) return;
    if (id == kOpen) self->_callbacks.open();
    else if (id == kQuit) self->_callbacks.quit();
  };
  if (is("GetLayout")) {
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(u@(ia{sv}av))", self->_menuRevision,
        self->menuLayout()));
  } else if (is("GetGroupProperties")) {
    // Every item's properties, from the full layout.
    auto *layout = g_variant_ref_sink(self->menuLayout());
    auto builder = g_variant_builder_new(G_VARIANT_TYPE("a(ia{sv})"));
    auto *children = g_variant_get_child_value(layout, 2);
    for (gsize i = 0, count = g_variant_n_children(children); i != count; ++i) {
      auto *boxed = g_variant_get_child_value(children, i);
      auto *child = g_variant_get_variant(boxed);
      gint32 id = 0;
      GVariant *properties = nullptr;
      g_variant_get(child, "(i@a{sv}@av)", &id, &properties, nullptr);
      g_variant_builder_add(builder, "(i@a{sv})", id, properties);
      g_variant_unref(properties);
      g_variant_unref(child);
      g_variant_unref(boxed);
    }
    g_variant_unref(children);
    g_variant_unref(layout);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(ia{sv}))", builder));
    g_variant_builder_unref(builder);
  } else if (is("GetProperty")) {
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", g_variant_new_string("")));
  } else if (is("Event")) {
    gint32 id = 0;
    const char *event = nullptr;
    g_variant_get(parameters, "(i&svu)", &id, &event, nullptr, nullptr);
    const auto copy = QString::fromUtf8(event);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    activate(id, copy.toUtf8().constData());
  } else if (is("EventGroup")) {
    GVariantIter *events = nullptr;
    g_variant_get(parameters, "(a(isvu))", &events);
    std::vector<std::pair<int, QString>> list;
    gint32 id = 0;
    const char *event = nullptr;
    while (g_variant_iter_next(events, "(i&svu)", &id, &event, nullptr, nullptr)) {
      list.emplace_back(id, QString::fromUtf8(event));
    }
    g_variant_iter_free(events);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(@ai)",
        g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0)));
    for (const auto &[id, event] : list) activate(id, event.toUtf8().constData());
  } else if (is("AboutToShow")) {
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
  } else if (is("AboutToShowGroup")) {
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(@ai@ai)",
        g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0), g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0)));
  } else {
    g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method");
  }
}

} // namespace Contrail
