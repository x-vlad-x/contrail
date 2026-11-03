#include "call_notification.hpp"
#include "call_shell.hpp"

#include <gio/gio.h>
#include <memory>

using namespace Contrail;
namespace {

void Wait(const std::function<bool()> &condition) {
  auto *loop = g_main_loop_new(nullptr, FALSE);
  auto pending = std::pair(loop, &condition);
  const auto poll = g_timeout_add(1, +[](gpointer data) -> gboolean {
    auto &pending = *static_cast<std::pair<GMainLoop *, const std::function<bool()> *> *>(data);
    if ((*pending.second)()) g_main_loop_quit(pending.first);
    return G_SOURCE_CONTINUE;
  }, &pending);
  const auto timeout = g_timeout_add_seconds(5, +[](gpointer) -> gboolean {
    g_error("The shell did not finish the call action"); return G_SOURCE_REMOVE;
  }, nullptr);
  g_main_loop_run(loop);
  g_source_remove(poll);
  g_source_remove(timeout);
  g_main_loop_unref(loop);
}

struct Shell {
  GDBusMethodInvocation *ring = nullptr;
  GDBusMethodInvocation *notification = nullptr;
  int ended = 0;
  int closed = 0;
  guint32 lastEnded = 0;
  guint32 lastClosed = 0;
  ~Shell() { g_clear_object(&ring); g_clear_object(&notification); }

  static void Method(GDBusConnection *, const char *, const char *, const char *, const char *name,
                     GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data) {
    auto &self = *static_cast<Shell *>(data);
    if (g_str_equal(name, "TriggerFeedback")) self.ring = G_DBUS_METHOD_INVOCATION(g_object_ref(invocation));
    else if (g_str_equal(name, "Notify")) self.notification = G_DBUS_METHOD_INVOCATION(g_object_ref(invocation));
    else if (g_str_equal(name, "EndFeedback")) {
      ++self.ended;
      g_variant_get(parameters, "(u)", &self.lastEnded);
      g_dbus_method_invocation_return_value(invocation, nullptr);
    } else if (g_str_equal(name, "CloseNotification")) {
      ++self.closed;
      g_variant_get(parameters, "(u)", &self.lastClosed);
      g_dbus_method_invocation_return_value(invocation, nullptr);
    }
  }
  void reply(GDBusMethodInvocation *&invocation, guint32 id) {
    g_assert_nonnull(invocation);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", id));
    g_clear_object(&invocation);
  }
};

constexpr char kFeedbackXml[] = R"xml(<node><interface name="org.sigxcpu.Feedback">
<method name="TriggerFeedback"><arg type="s" direction="in"/><arg type="s" direction="in"/>
<arg type="a{sv}" direction="in"/><arg type="i" direction="in"/><arg type="u" direction="out"/></method>
<method name="EndFeedback"><arg type="u" direction="in"/></method>
</interface></node>)xml";
constexpr char kNotificationXml[] = R"xml(<node><interface name="org.freedesktop.Notifications">
<method name="Notify"><arg type="s" direction="in"/><arg type="u" direction="in"/>
<arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="in"/>
<arg type="as" direction="in"/><arg type="a{sv}" direction="in"/><arg type="i" direction="in"/>
<arg type="u" direction="out"/></method><method name="CloseNotification"><arg type="u" direction="in"/></method>
</interface></node>)xml";

void Own(GDBusConnection *bus, const char *name) {
  auto *reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", name, 0u), G_VARIANT_TYPE("(u)"),
      G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
  g_assert_nonnull(reply);
  g_variant_unref(reply);
}
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, nullptr);
  auto *testBus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(testBus);
  auto *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
  g_assert_nonnull(bus);
  Own(bus, "org.sigxcpu.Feedback");
  Own(bus, "org.freedesktop.Notifications");
  auto shell = Shell();
  auto *feedback = g_dbus_node_info_new_for_xml(kFeedbackXml, nullptr);
  auto *notifications = g_dbus_node_info_new_for_xml(kNotificationXml, nullptr);
  const auto vtable = GDBusInterfaceVTable{Shell::Method, nullptr, nullptr};
  const auto ringObject = g_dbus_connection_register_object(bus, "/org/sigxcpu/Feedback", feedback->interfaces[0],
      &vtable, &shell, nullptr, nullptr);
  const auto notificationObject = g_dbus_connection_register_object(bus, "/org/freedesktop/Notifications",
      notifications->interfaces[0], &vtable, &shell, nullptr, nullptr);
  g_assert_cmpuint(ringObject, !=, 0);
  g_assert_cmpuint(notificationObject, !=, 0);
  // The application can close a call while feedbackd or Phosh still has its start request in flight.
  {
    auto ringer = Ringer();
    ringer.start();
    Wait([&] { return shell.ring; });
  }
  shell.reply(shell.ring, 9);
  Wait([&] { return shell.ended == 1; });
  g_assert_cmpuint(shell.lastEnded, ==, 9);
  {
    auto notification = CallNotification({});
    notification.showIncoming("Caller", "Incoming", nullptr);
    Wait([&] { return shell.notification; });
  }
  shell.reply(shell.notification, 42);
  Wait([&] { return shell.closed == 1; });
  g_assert_cmpuint(shell.lastClosed, ==, 42);
  // Hanging up before the reply also closes the event when the owner itself stays alive.
  {
    auto ringer = Ringer();
    ringer.start();
    Wait([&] { return shell.ring; });
    ringer.stop();
    shell.reply(shell.ring, 10);
    Wait([&] { return shell.ended == 2; });
    g_assert_cmpuint(ringer.eventId(), ==, 0);
  }
  {
    auto notification = CallNotification({});
    notification.showIncoming("Caller", "Incoming", nullptr);
    Wait([&] { return shell.notification; });
    notification.close();
    shell.reply(shell.notification, 43);
    Wait([&] { return shell.closed == 2; });
    g_assert_cmpuint(notification.id(), ==, 0);
  }
  g_dbus_connection_unregister_object(bus, ringObject);
  g_dbus_connection_unregister_object(bus, notificationObject);
  g_dbus_node_info_unref(feedback);
  g_dbus_node_info_unref(notifications);
  g_dbus_connection_close_sync(bus, nullptr, nullptr);
  g_object_unref(bus);
  while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);
  g_test_dbus_down(testBus);
  g_object_unref(testBus);
  g_print("call-shell-test: PASS\n");
}
