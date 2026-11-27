#!/usr/bin/env python3
"""What a desktop gives the app on the session bus, for tools/td-ui-test.sh: a notification server and a tray.

Every call is written to the log given as the first argument, one line each:
    notify id=N replaces=N summary=... body=... actions=a,b hints=k,k sound=... icon=...
    close id=N
    tray-register name=...
The test answers as a person would through org.contrail.FakeDesktop (on the same connection, so that the app sees
the signals coming from the notification server):
    Invoke(u id, s action)   ActionInvoked, as a tap on the notification or its button
    Reply(u id, s text)      NotificationReplied, the text typed into it
    Dismiss(u id)            NotificationClosed with reason 2, as a swipe
"""
import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

LOG = open(sys.argv[1], "a", buffering=1, encoding="utf-8")

XML = """<node>
  <interface name="org.freedesktop.Notifications">
    <method name="GetCapabilities"><arg type="as" direction="out"/></method>
    <method name="GetServerInformation">
      <arg type="s" direction="out"/><arg type="s" direction="out"/><arg type="s" direction="out"/><arg type="s" direction="out"/>
    </method>
    <method name="Notify">
      <arg type="s" direction="in"/><arg type="u" direction="in"/><arg type="s" direction="in"/>
      <arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="as" direction="in"/>
      <arg type="a{sv}" direction="in"/><arg type="i" direction="in"/><arg type="u" direction="out"/>
    </method>
    <method name="CloseNotification"><arg type="u" direction="in"/></method>
    <signal name="NotificationClosed"><arg type="u"/><arg type="u"/></signal>
    <signal name="ActionInvoked"><arg type="u"/><arg type="s"/></signal>
    <signal name="NotificationReplied"><arg type="u"/><arg type="s"/></signal>
  </interface>
  <interface name="org.kde.StatusNotifierWatcher">
    <method name="RegisterStatusNotifierItem"><arg type="s" direction="in"/></method>
    <property name="IsStatusNotifierHostRegistered" type="b" access="read"/>
  </interface>
  <interface name="org.contrail.FakeDesktop">
    <method name="Invoke"><arg type="u" direction="in"/><arg type="s" direction="in"/></method>
    <method name="Reply"><arg type="u" direction="in"/><arg type="s" direction="in"/></method>
    <method name="Dismiss"><arg type="u" direction="in"/></method>
  </interface>
</node>"""

INFO = Gio.DBusNodeInfo.new_for_xml(XML)
state = {"next": 1, "open": set()}


def log(line: str) -> None:
    LOG.write(line.replace("\n", "\\n") + "\n")


def emit(bus, name, signature, values):
    bus.emit_signal(None, "/org/freedesktop/Notifications", "org.freedesktop.Notifications", name,
                    GLib.Variant(signature, values))


def method(bus, sender, path, interface, name, params, invocation):
    args = params.unpack()
    if name == "GetCapabilities":
        invocation.return_value(GLib.Variant("(as)", (["body", "actions", "inline-reply", "persistence", "sound"],)))
    elif name == "GetServerInformation":
        invocation.return_value(GLib.Variant("(ssss)", ("fake", "contrail", "1", "1.2")))
    elif name == "Notify":
        app, replaces, icon, summary, body, actions, hints, _timeout = args
        nid = replaces if replaces in state["open"] else state["next"]
        if nid == state["next"]:
            state["next"] += 1
        state["open"].add(nid)
        sound = hints.get("sound-name", "") if not hints.get("suppress-sound") else "suppressed"
        log(f"notify id={nid} replaces={replaces} summary={summary} body={body} actions={','.join(actions[::2])} "
            f"hints={','.join(sorted(hints))} sound={sound} icon={icon or hints.get('image-path', '')}")
        invocation.return_value(GLib.Variant("(u)", (nid,)))
    elif name == "CloseNotification":
        nid = args[0]
        log(f"close id={nid}")
        invocation.return_value(None)
        if nid in state["open"]:
            state["open"].discard(nid)
            emit(bus, "NotificationClosed", "(uu)", (nid, 3))
    elif name == "RegisterStatusNotifierItem":
        log(f"tray-register name={args[0] or sender}")
        invocation.return_value(None)
    elif name == "Invoke":
        invocation.return_value(None)
        emit(bus, "ActionInvoked", "(us)", (args[0], args[1]))
    elif name == "Reply":
        invocation.return_value(None)
        emit(bus, "NotificationReplied", "(us)", (args[0], args[1]))
    elif name == "Dismiss":
        invocation.return_value(None)
        state["open"].discard(args[0])
        emit(bus, "NotificationClosed", "(uu)", (args[0], 2))


def prop(bus, sender, path, interface, name):
    return GLib.Variant("b", True)


def acquired(bus, name):
    for interface in INFO.interfaces:
        path = "/StatusNotifierWatcher" if interface.name == "org.kde.StatusNotifierWatcher" else \
            "/org/freedesktop/Notifications"
        bus.register_object(path, interface, method, prop, None)
    for owned in ("org.freedesktop.Notifications", "org.kde.StatusNotifierWatcher", "org.contrail.FakeDesktop"):
        Gio.bus_own_name_on_connection(bus, owned, Gio.BusNameOwnerFlags.NONE, None, None)
    log("ready")


Gio.bus_get(Gio.BusType.SESSION, None, lambda _source, result: acquired(Gio.bus_get_finish(result), None))
GLib.MainLoop().run()
