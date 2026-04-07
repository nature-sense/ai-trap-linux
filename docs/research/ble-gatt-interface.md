# AI Trap — BLE GATT Interface

The trap advertises a BLE GATT service that lets a mobile app manage WiFi
provisioning without any physical access to the device. BLE remains active
even when WiFi is fully shut down, so it acts as the wake-up channel for
battery-constrained deployments.

---

## Advertising

| Field | Value |
|---|---|
| Advertising type | `ADV_IND` (connectable undirected) |
| Interval | 100 ms |
| Address type | Public |
| AD — Flags | `0x06` (LE General Discoverable, BR/EDR Not Supported) |
| AD — 16-bit UUID list | `0xFFF0` (WiFi Service) |
| AD — Complete Local Name | `AI-Trap-<trap_id>` (e.g. `AI-Trap-001`) |

The device name is configured at runtime from `trap_id` in `trap_config.toml`.
Maximum name length in the advertising packet is 20 characters.

Advertising resumes automatically after a client disconnects.

---

## GATT Attribute Table

```
Handle  UUID    Properties          Description
──────  ──────  ──────────────────  ─────────────────────────────────────
0x0001  0x2800  —                   Primary Service Declaration → 0xFFF0
0x0002  0x2803  —                   Characteristic Declaration (WiFi State)
0x0003  0xFFF1  Read, Notify        WiFi State Value
0x0004  0x2902  Read, Write         Client Characteristic Config (CCCD)
0x0005  0x2803  —                   Characteristic Declaration (WiFi Command)
0x0006  0xFFF2  Write, Write-No-Rsp WiFi Command Value
```

All UUIDs are 16-bit. Service group spans handles `0x0001`–`0x0006`.

---

## Characteristic: WiFi State — `0xFFF1` (handle `0x0003`)

### Properties
- **Read** — poll the current WiFi state at any time.
- **Notify** — subscribe via the CCCD to receive unsolicited updates whenever
  the WiFi state changes (mode switch, IP acquired, shutdown).

### Value format
UTF-8 JSON string, length bounded by the negotiated ATT MTU (default 23 bytes,
up to 512 bytes after MTU exchange).

```json
{
  "mode":      "ap" | "station" | "off" | "unknown",
  "ssid":      "<string>",
  "connected": true | false,
  "ip":        "<IPv4 string or empty>"
}
```

### Field details

| Field | Type | Description |
|---|---|---|
| `mode` | string | Current WiFi operating mode |
| `ssid` | string | AP SSID (AP mode) or associated network SSID (station mode). Empty when `mode` is `"off"`. |
| `connected` | boolean | `true` once DHCP has been acquired in station mode. Always `false` in AP and off modes. |
| `ip` | string | Current IPv4 address on `wlan0`. Empty string when no address is assigned. |

### Mode values

| `mode` | Meaning |
|---|---|
| `"ap"` | Trap is running its own access point. App should connect to the trap SSID and then use the HTTP API. |
| `"station"` | Trap is connected to an external WiFi network. |
| `"off"` | WiFi has been shut down (power saving). Send a `start_ap` or `connect` command to wake it. |
| `"unknown"` | State could not be determined (e.g. during a mode transition). |

### Example — AP mode active
```json
{"mode":"ap","ssid":"ai-trap-001","connected":false,"ip":"192.168.4.1"}
```

### Example — Connected to home network
```json
{"mode":"station","ssid":"HomeNet","connected":true,"ip":"192.168.1.45"}
```

### Example — WiFi off
```json
{"mode":"off","ssid":"","connected":false,"ip":""}
```

### Enabling notifications (CCCD, handle `0x0004`)

Write `0x0001` (little-endian) to handle `0x0004` to enable notifications.
Write `0x0000` to disable.

```
Write handle 0x0004  →  01 00   (enable notifications)
Write handle 0x0004  →  00 00   (disable notifications)
```

The server sends a Handle Value Notify PDU (`0x1B`) to the connected client
whenever `notifyStateChanged()` is called internally — this happens:
- immediately after processing any WiFi command on `0xFFF2`
- when the WiFi inactivity timer fires and shuts WiFi down

---

## Characteristic: WiFi Command — `0xFFF2` (handle `0x0006`)

### Properties
- **Write** (`ATT_OP_WRITE_REQ`, expects Write Response)
- **Write Without Response** (`ATT_OP_WRITE_CMD`, no response)

### Value format
UTF-8 JSON object. The `cmd` field selects the operation.

---

### Command: `start_ap`

Start the trap's own WiFi access point. The app can then connect to the trap
SSID and interact with the HTTP API.

```json
{"cmd":"start_ap"}
```

**Behaviour**
- Clears any saved station credentials.
- Raises `wlan0` in AP mode (hostapd / NetworkManager hotspot).
- Default AP SSID: `ai-trap-<trap_id>`
- Default AP password: `aiwildlife`
- Default gateway/DNS IP: `192.168.4.1`
- A WiFi State notification is sent after the mode change.

---

### Command: `connect`

Connect the trap to an external WiFi network (station mode).

```json
{"cmd":"connect","ssid":"<network-name>","password":"<passphrase>"}
```

| Field | Required | Description |
|---|---|---|
| `cmd` | yes | Must be `"connect"` |
| `ssid` | yes | Target network SSID |
| `password` | yes | WPA2 passphrase (use `""` for open networks) |

**Behaviour**
- Saves credentials to `/opt/ai-trap/wifi_creds.conf` (persists across reboots).
- Switches `wlan0` to station mode and associates.
- A WiFi State notification is sent; `"connected"` becomes `true` once DHCP
  completes (poll or subscribe to notifications to detect this).

---

### Command: `shutdown`

Shut WiFi down entirely for power saving.

```json
{"cmd":"shutdown"}
```

**Behaviour**
- Brings `wlan0` down. The HTTP API and MJPEG stream become unavailable.
- BLE advertising continues, so the app can reconnect and issue another command
  to wake WiFi.
- Does **not** clear saved credentials; a `connect` after reboot will restore
  station mode.
- A WiFi State notification is sent with `"mode":"off"`.

---

## Inactivity Auto-Shutdown

If no HTTP request is received for `inactivitySeconds` (default 600 s / 10 min),
WiFi shuts down automatically as if `shutdown` had been sent.

- The inactivity timer is reset by every HTTP request to the trap's API.
- A WiFi State notification (`"mode":"off"`) is sent to any connected BLE client
  when the timer fires.
- Configurable in `trap_config.toml` via the `wifi.inactivity_seconds` key.
  Set to `0` to disable the timer entirely.

---

## ATT MTU

The server advertises a maximum supported MTU of 512 bytes. The client should
send an **Exchange MTU Request** early in the connection to negotiate a larger
MTU — this is important for WiFi State values that may exceed the default 23-byte
limit (e.g. when the SSID or IP is long).

Standard MTU exchange:
```
Client → Server:  02 XX XX   (ATT_OP_MTU_REQ, client MTU little-endian)
Server → Client:  03 XX XX   (ATT_OP_MTU_RESP, agreed MTU little-endian)
```

The agreed MTU is `min(client_mtu, 512)`. Values are clamped to a minimum of 23.

---

## Typical Session Flow

```
1.  App scans for BLE devices advertising service UUID 0xFFF0
2.  App finds "AI-Trap-001", connects

3.  App exchanges MTU (optional but recommended — negotiate 185+ bytes)

4.  App subscribes to WiFi State notifications:
      Write 0x0001 → handle 0x0004

5.  App reads current WiFi state:
      Read handle 0x0003
      ← {"mode":"off","ssid":"","connected":false,"ip":""}

6.  App starts AP:
      Write {"cmd":"start_ap"} → handle 0x0006
      ← Notification: {"mode":"ap","ssid":"ai-trap-001","connected":false,"ip":"192.168.4.1"}

7.  App disconnects BLE, connects phone WiFi to "ai-trap-001" (password: aiwildlife)

8.  App uses HTTP API on http://192.168.4.1:8080 to browse images, configure, etc.

9.  (optional) App sends station credentials via HTTP POST /api/wifi
    or via BLE: Write {"cmd":"connect","ssid":"HomeNet","password":"secret"} → handle 0x0006

10. WiFi shuts down after 10 min idle (inactivity timer)
    ← Notification (if still BLE-connected): {"mode":"off", ...}
```

---

## Prerequisites on the Trap

| Requirement | How it is satisfied |
|---|---|
| `bluetoothd` not running | `install.sh` disables and masks `bluetooth.service` |
| `hci0` adapter present | BCM2712 (Pi 5) or AIC8800DC (Luckfox) onboard BLE |
| `hciconfig` in PATH | Part of BlueZ package on both platforms |
| BlueZ headers at compile time | Extracted from BlueZ 5.66 source tarball in CI |
| No `libbluetooth.so` at runtime | Only header macros/inlines used; `ba2str()` replaced with inline formatter |
