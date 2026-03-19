#!/usr/bin/env python3
"""
ble_gatt.py — BLE GATT server for AI Trap WiFi management (Raspberry Pi 5)

Registers a GATT service with BlueZ via D-Bus and advertises the trap over BLE.
Communicates with the C++ trap binary via a Unix socket (/tmp/ai-trap-ble.sock).

GATT Service  UUID: 0000FFF0-0000-1000-8000-00805F9B34FB
  FFF1  WiFi State  (read + notify)  — JSON string
  FFF2  WiFi Command (write)         — JSON string

WiFi State JSON (sent by C++, notified to BLE client):
  {"event":"state","mode":"ap","ssid":"ai-trap-001","connected":false,"ip":"192.168.4.1"}

WiFi Command JSON (written by BLE client, forwarded to C++):
  {"cmd":"start_ap"}
  {"cmd":"connect","ssid":"MyNet","password":"secret"}
  {"cmd":"shutdown"}

Dependencies (Raspberry Pi OS Bookworm):
  sudo apt install python3-dbus python3-gi bluez

Usage:
  python3 ble_gatt.py [--name AI-Trap-001] [--hci hci0] [--socket /tmp/ai-trap-ble.sock]
"""

import argparse
import dbus
import dbus.exceptions
import dbus.mainloop.glib
import dbus.service
import json
import os
import socket
import sys
import threading
import time

from gi.repository import GLib

# ─────────────────────────────────────────────────────────────────────────────
#  BlueZ D-Bus constants
# ─────────────────────────────────────────────────────────────────────────────

BLUEZ_SERVICE           = "org.bluez"
GATT_MANAGER_IFACE      = "org.bluez.GattManager1"
LE_ADV_MANAGER_IFACE    = "org.bluez.LEAdvertisingManager1"
LE_ADV_IFACE            = "org.bluez.LEAdvertisement1"
GATT_SERVICE_IFACE      = "org.bluez.GattService1"
GATT_CHAR_IFACE         = "org.bluez.GattCharacteristic1"
DBUS_OM_IFACE           = "org.freedesktop.DBus.ObjectManager"
DBUS_PROP_IFACE         = "org.freedesktop.DBus.Properties"

# ─────────────────────────────────────────────────────────────────────────────
#  UUIDs  (16-bit base UUIDs in 128-bit form)
# ─────────────────────────────────────────────────────────────────────────────

SERVICE_UUID    = "0000fff0-0000-1000-8000-00805f9b34fb"
WIFI_STATE_UUID = "0000fff1-0000-1000-8000-00805f9b34fb"
WIFI_CMD_UUID   = "0000fff2-0000-1000-8000-00805f9b34fb"

APP_PATH  = "/com/aitrap"
SVC_PATH  = "/com/aitrap/service0"
CHAR_STATE_PATH = "/com/aitrap/service0/char0"
CHAR_CMD_PATH   = "/com/aitrap/service0/char1"
ADV_PATH  = "/com/aitrap/advertisement0"

# ─────────────────────────────────────────────────────────────────────────────
#  Unix socket client — talks to C++ BleCtrl
# ─────────────────────────────────────────────────────────────────────────────

class TrapSocket:
    """Connects to the C++ BleCtrl Unix socket and exchanges state/commands."""

    def __init__(self, path, on_state):
        self._path     = path
        self._on_state = on_state   # callback(state_dict)
        self._sock     = None
        self._buf      = ""
        self._lock     = threading.Lock()
        self._running  = True
        self._thread   = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        while self._running:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(self._path)
                with self._lock:
                    self._sock = s
                print(f"[ble] connected to trap socket {self._path}")
                self._recv_loop(s)
            except Exception as e:
                print(f"[ble] socket error: {e} — retrying in 5s")
            finally:
                with self._lock:
                    self._sock = None
            if self._running:
                time.sleep(5)

    def _recv_loop(self, s):
        buf = ""
        while self._running:
            try:
                data = s.recv(512)
                if not data:
                    break
                buf += data.decode("utf-8", errors="replace")
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    line = line.strip()
                    if line:
                        try:
                            msg = json.loads(line)
                            if msg.get("event") == "state":
                                self._on_state(msg)
                        except json.JSONDecodeError:
                            print(f"[ble] bad JSON: {line}")
            except OSError:
                break

    def send(self, obj):
        msg = json.dumps(obj) + "\n"
        with self._lock:
            if self._sock:
                try:
                    self._sock.sendall(msg.encode())
                except OSError as e:
                    print(f"[ble] send error: {e}")

    def stop(self):
        self._running = False
        with self._lock:
            if self._sock:
                self._sock.close()

# ─────────────────────────────────────────────────────────────────────────────
#  GATT Application (ObjectManager)
# ─────────────────────────────────────────────────────────────────────────────

class Application(dbus.service.Object):
    def __init__(self, bus):
        self.path = APP_PATH
        self.services = []
        dbus.service.Object.__init__(self, bus, self.path)

    def get_path(self):
        return dbus.ObjectPath(self.path)

    def add_service(self, service):
        self.services.append(service)

    @dbus.service.method(DBUS_OM_IFACE, out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        response = {}
        for service in self.services:
            response[service.get_path()] = service.get_properties()
            for char in service.get_characteristics():
                response[char.get_path()] = char.get_properties()
        return response

# ─────────────────────────────────────────────────────────────────────────────
#  GATT Service
# ─────────────────────────────────────────────────────────────────────────────

class Service(dbus.service.Object):
    def __init__(self, bus, index, uuid, primary):
        self.path = f"{APP_PATH}/service{index}"
        self.uuid = uuid
        self.primary = primary
        self.chars = []
        dbus.service.Object.__init__(self, bus, self.path)

    def get_path(self):
        return dbus.ObjectPath(self.path)

    def get_characteristics(self):
        return self.chars

    def add_characteristic(self, char):
        self.chars.append(char)

    def get_properties(self):
        return {
            GATT_SERVICE_IFACE: {
                "UUID":    dbus.String(self.uuid),
                "Primary": dbus.Boolean(self.primary),
            }
        }

    @dbus.service.method(DBUS_PROP_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return self.get_properties().get(interface, {})

# ─────────────────────────────────────────────────────────────────────────────
#  GATT Characteristic base
# ─────────────────────────────────────────────────────────────────────────────

class Characteristic(dbus.service.Object):
    def __init__(self, bus, index, uuid, flags, service):
        self.path    = f"{service.path}/char{index}"
        self.uuid    = uuid
        self.flags   = flags
        self.service = service
        self.notifying = False
        dbus.service.Object.__init__(self, bus, self.path)

    def get_path(self):
        return dbus.ObjectPath(self.path)

    def get_properties(self):
        return {
            GATT_CHAR_IFACE: {
                "Service": self.service.get_path(),
                "UUID":    dbus.String(self.uuid),
                "Flags":   dbus.Array(self.flags, signature="s"),
            }
        }

    @dbus.service.method(DBUS_PROP_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return self.get_properties().get(interface, {})

    @dbus.service.method(GATT_CHAR_IFACE, in_signature="a{sv}", out_signature="ay")
    def ReadValue(self, options):
        raise dbus.exceptions.DBusException(
            "org.bluez.Error.NotSupported", "ReadValue not implemented")

    @dbus.service.method(GATT_CHAR_IFACE, in_signature="aya{sv}")
    def WriteValue(self, value, options):
        raise dbus.exceptions.DBusException(
            "org.bluez.Error.NotSupported", "WriteValue not implemented")

    @dbus.service.method(GATT_CHAR_IFACE)
    def StartNotify(self):
        self.notifying = True
        print(f"[ble] StartNotify on {self.uuid}")

    @dbus.service.method(GATT_CHAR_IFACE)
    def StopNotify(self):
        self.notifying = False
        print(f"[ble] StopNotify on {self.uuid}")

    @dbus.service.signal(DBUS_PROP_IFACE, signature="sa{sv}as")
    def PropertiesChanged(self, interface, changed, invalidated):
        pass

    def notify_value(self, value_bytes):
        """Send a PropertiesChanged signal to trigger a GATT notification."""
        if self.notifying:
            self.PropertiesChanged(
                GATT_CHAR_IFACE,
                {"Value": dbus.Array(value_bytes, signature="y")},
                [])

# ─────────────────────────────────────────────────────────────────────────────
#  WiFi State Characteristic  (FFF1 — read + notify)
# ─────────────────────────────────────────────────────────────────────────────

class WifiStateCharacteristic(Characteristic):
    def __init__(self, bus, service):
        super().__init__(bus, 0, WIFI_STATE_UUID,
                         ["read", "notify"], service)
        self._state_json = "{}"

    def set_state(self, state_dict):
        payload = {k: v for k, v in state_dict.items() if k != "event"}
        self._state_json = json.dumps(payload)
        raw = list(self._state_json.encode("utf-8"))
        self.notify_value(raw)
        print(f"[ble] WiFi state: {self._state_json}")

    @dbus.service.method(GATT_CHAR_IFACE, in_signature="a{sv}", out_signature="ay")
    def ReadValue(self, options):
        return dbus.Array(list(self._state_json.encode("utf-8")), signature="y")

# ─────────────────────────────────────────────────────────────────────────────
#  WiFi Command Characteristic  (FFF2 — write)
# ─────────────────────────────────────────────────────────────────────────────

class WifiCmdCharacteristic(Characteristic):
    def __init__(self, bus, service, trap_sock):
        super().__init__(bus, 1, WIFI_CMD_UUID,
                         ["write", "write-without-response"], service)
        self._trap = trap_sock

    @dbus.service.method(GATT_CHAR_IFACE, in_signature="aya{sv}")
    def WriteValue(self, value, options):
        raw = bytes(value).decode("utf-8", errors="replace").strip()
        print(f"[ble] WiFi cmd received: {raw}")
        try:
            obj = json.loads(raw)
            self._trap.send(obj)
        except json.JSONDecodeError:
            print(f"[ble] bad command JSON: {raw}")

# ─────────────────────────────────────────────────────────────────────────────
#  LE Advertisement
# ─────────────────────────────────────────────────────────────────────────────

class Advertisement(dbus.service.Object):
    def __init__(self, bus, device_name):
        dbus.service.Object.__init__(self, bus, ADV_PATH)
        self._name = device_name

    @dbus.service.method(DBUS_PROP_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        if interface != LE_ADV_IFACE:
            raise dbus.exceptions.DBusException("org.bluez.Error.InvalidArguments")
        return {
            "Type":        dbus.String("peripheral"),
            "LocalName":   dbus.String(self._name),
            "ServiceUUIDs": dbus.Array([SERVICE_UUID], signature="s"),
            "Includes":    dbus.Array(["tx-power"], signature="s"),
        }

    @dbus.service.method(LE_ADV_IFACE)
    def Release(self):
        print("[ble] advertisement released")

# ─────────────────────────────────────────────────────────────────────────────
#  Helpers
# ─────────────────────────────────────────────────────────────────────────────

def find_adapter(bus, hci):
    """Return the D-Bus path for the requested HCI adapter."""
    remote_om  = dbus.Interface(bus.get_object(BLUEZ_SERVICE, "/"),
                                DBUS_OM_IFACE)
    objects    = remote_om.GetManagedObjects()
    for path, ifaces in objects.items():
        if GATT_MANAGER_IFACE in ifaces and path.endswith(hci):
            return path
    # Fall back to first adapter found
    for path, ifaces in objects.items():
        if GATT_MANAGER_IFACE in ifaces:
            return path
    return None

# ─────────────────────────────────────────────────────────────────────────────
#  main
# ─────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="AI Trap BLE GATT server")
    parser.add_argument("--name",   default="AI-Trap", help="BLE device name")
    parser.add_argument("--hci",    default="hci0",    help="HCI adapter (hci0)")
    parser.add_argument("--socket", default="/tmp/ai-trap-ble.sock",
                        help="Path to C++ BleCtrl Unix socket")
    args = parser.parse_args()

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus     = dbus.SystemBus()
    mainloop = GLib.MainLoop()

    # ── Build GATT objects ──────────────────────────────────────────────────

    app  = Application(bus)
    svc  = Service(bus, 0, SERVICE_UUID, primary=True)

    # We need wifi_char before trap_sock (trap_sock callback references wifi_char)
    wifi_char = None

    def on_state(state_dict):
        if wifi_char:
            GLib.idle_add(wifi_char.set_state, state_dict)

    trap = TrapSocket(args.socket, on_state)

    wifi_char = WifiStateCharacteristic(bus, svc)
    cmd_char  = WifiCmdCharacteristic(bus, svc, trap)

    svc.add_characteristic(wifi_char)
    svc.add_characteristic(cmd_char)
    app.add_service(svc)

    # ── Find BlueZ adapter ──────────────────────────────────────────────────

    adapter_path = find_adapter(bus, args.hci)
    if not adapter_path:
        print(f"[ble] ERROR: no BlueZ adapter found for {args.hci}")
        sys.exit(1)
    print(f"[ble] using adapter: {adapter_path}")

    # Make sure adapter is powered on
    adapter_props = dbus.Interface(
        bus.get_object(BLUEZ_SERVICE, adapter_path),
        DBUS_PROP_IFACE)
    try:
        adapter_props.Set("org.bluez.Adapter1", "Powered", dbus.Boolean(True))
    except Exception as e:
        print(f"[ble] warning: could not power adapter: {e}")

    # ── Register GATT application ───────────────────────────────────────────

    gatt_mgr = dbus.Interface(
        bus.get_object(BLUEZ_SERVICE, adapter_path),
        GATT_MANAGER_IFACE)

    def on_register_ok():
        print("[ble] GATT application registered")

    def on_register_err(error):
        print(f"[ble] GATT registration failed: {error}")
        mainloop.quit()

    gatt_mgr.RegisterApplication(
        app.get_path(), {},
        reply_handler=on_register_ok,
        error_handler=on_register_err)

    # ── Register LE advertisement ───────────────────────────────────────────

    adv     = Advertisement(bus, args.name)
    adv_mgr = dbus.Interface(
        bus.get_object(BLUEZ_SERVICE, adapter_path),
        LE_ADV_MANAGER_IFACE)

    def on_adv_ok():
        print(f"[ble] advertising as '{args.name}'")

    def on_adv_err(error):
        print(f"[ble] advertisement failed: {error}")

    adv_mgr.RegisterAdvertisement(
        ADV_PATH, {},
        reply_handler=on_adv_ok,
        error_handler=on_adv_err)

    print(f"[ble] GATT server running. Ctrl-C to stop.")
    try:
        mainloop.run()
    except KeyboardInterrupt:
        pass
    finally:
        trap.stop()


if __name__ == "__main__":
    main()
