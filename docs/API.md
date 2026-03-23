# AI Trap — HTTP & SSE API Contract

Base URL: `http://{trap_ip}`

All HTTP responses include CORS headers (`Access-Control-Allow-Origin: *`).

---

## HTTP API (default port 8080)

### GET /api/trap

Trap identity — static configuration values.

**Response 200**
```json
{
  "id":       "trap_001",
  "location": "north_meadow"
}
```

---

### GET /api/capture

Current detection state.

**Response 200**
```json
{
  "active":    true,
  "sessionId": "20260314_153042"
}
```

`sessionId` is `null` when no capture session is active.

---

### POST /api/capture

Start or stop detection.

**Request body**
```json
{ "active": true }
```

**Response 200**
```json
{ "active": true }
```

Fires a `capture` SSE event to all connected clients when the state changes.

---

### GET /api/status

Current trap status — combines identity, capture state, and runtime metrics.

**Response 200**
```json
{
  "id":         "trap_001",
  "location":   "north_meadow",
  "capturing":  true,
  "sessionId":  "20260314_153042",
  "uptime_s":   22440,
  "fps":        18.3,
  "detections": 247,
  "tracks":     12,
  "db_mb":      1.4,
  "sse_clients": 2
}
```

`sessionId` is `null` when no capture session is active.

---

### GET /api/events

Redirects (307) to the SSE server on port 8081.
Connect directly to `http://{trap_ip}:8081/api/events` for the event stream.

---

### GET /api/crops

List of saved crop images with metadata. Files that have been deleted from disk
(synced=2) are excluded.

**Response 200** — array of crop objects
```json
[
  {
    "file":          "insect_42.jpg",
    "bytes":         18432,
    "trackId":       42,
    "conf":          0.8721,
    "timestampUs":   1741234567890123,
    "label":         "insect",
    "session":       "20260314_153042",
    "temperatureC":  24.50,
    "humidityPct":   61.00,
    "pressureHpa":   1013.25
  }
]
```

Environmental fields (`temperatureC`, `humidityPct`, `pressureHpa`) are `null`
when no sensor data was available at capture time.

Results are ordered newest first.

---

### GET /api/crops/{filename}

Download a single JPEG crop file.

`{filename}` may be a bare filename (`insect_42.jpg`) or a single-level
session-relative path (`20260314_153042/insect_42.jpg`).

**Response 200** — `image/jpeg` body
**Response 404** — file not found

Path traversal (`..` in filename, or more than one `/`) returns 404.

---

### GET /api/wifi

Current WiFi status.

**Response 200**
```json
{
  "mode":      "station",
  "ssid":      "MyNetwork",
  "connected": true,
  "ip":        "192.168.1.42"
}
```

`mode` is `"ap"` when the trap is running its own access point, `"station"`
when connected to an external network, or `"unknown"` when wifi management is
unavailable.

---

### POST /api/wifi

Provision WiFi — switch the trap to station mode and connect to the given
network. The response is returned before the connection attempt completes; poll
`GET /api/wifi` to observe the result.

**Request body**
```json
{ "ssid": "MyNetwork", "password": "secret" }
```

**Response 200** — `{"status": "ok"}`
**Response 400** — `{"error": "ssid is required"}` or `{"error": "<reason>"}`
**Response 400** — `{"error": "wifi management not available"}`

---

### POST /api/wifi/reset

Switch the trap back to AP mode.

**Response 200** — `{"status": "ok"}`
**Response 400** — `{"error": "<reason>"}` or `{"error": "wifi management not available"}`

---

### POST /api/sync/session

Open a new sync session. Returns the session ID and count of unsynced crops.

**Response 200**
```json
{
  "sessionId": "a3f9c12b4e7d",
  "pending":   11
}
```

**Response 503** — `{"error": "sync unavailable"}`

---

### GET /api/sync/session/{sessionId}

Get the full manifest for an open session.

**Response 200**
```json
{
  "sessionId": "a3f9c12b4e7d",
  "pending":   11,
  "crops": [
    {
      "file":          "insect_42.jpg",
      "bytes":         18432,
      "trackId":       42,
      "label":         "insect",
      "conf":          0.8721,
      "timestampUs":   1741234567890123,
      "temperatureC":  24.50,
      "humidityPct":   61.00,
      "pressureHpa":   1013.25
    }
  ]
}
```

Environmental fields are `null` when not available.

**Response 404** — `{"error": "session not found"}`

---

### POST /api/sync/ack

Acknowledge downloaded files (mark synced). Safe to call per-file or in batches.

**Request body**
```json
{
  "sessionId": "a3f9c12b4e7d",
  "files":     ["insect_42.jpg", "insect_43.jpg"]
}
```

**Response 200**
```json
{ "acked": 2 }
```

**Response 400** — `{"error": "sessionId and files required"}`
**Response 503** — `{"error": "sync unavailable"}`

---

### DELETE /api/sync/session/{sessionId}

Close session and delete all acknowledged (synced=1) files from disk.

**Response 200**
```json
{
  "deleted":    11,
  "bytesFreed": 204800,
  "notFound":   0
}
```

---

### POST /api/config/location

Set GPS coordinates. Applied immediately to new crop EXIF data.

**Request body**
```json
{ "lat": 13.7563, "lon": 100.5018 }
```

**Response 200** — `{"status": "ok"}`

---

### POST /api/config/threshold

Set the YOLO confidence threshold (0 < value < 1).

**Request body**
```json
{ "value": 0.45 }
```

**Response 200** — `{"status": "ok"}`
**Response 400** — `{"error": "value must be in (0,1)"}`

---

### POST /api/af/trigger

Trigger a one-shot autofocus scan.

**Response 200** — `{"status": "ok"}`

---

## SSE Event Stream (default port 8081)

Connect to `http://{trap_ip}:8081/api/events`
(`text/event-stream`, standard SSE protocol)

All events carry a `ts` field — Unix timestamp in **milliseconds**.

---

### detection

Fired once per confirmed track on the frame it is first confirmed.

```json
{
  "type":    "detection",
  "trackId": 42,
  "class":   "insect",
  "conf":    0.872,
  "bbox":    [120, 80, 340, 260],
  "frameId": 1234,
  "ts":      1741234567890
}
```

`bbox` — `[x1, y1, x2, y2]` in original image pixels.

---

### crop_saved

Fired when CropSaver writes (or replaces with a higher-confidence) JPEG for a track.

```json
{
  "type":    "crop_saved",
  "trackId": 42,
  "class":   "insect",
  "conf":    0.872,
  "file":    "insect_42.jpg",
  "w":       320,
  "h":       240,
  "ts":      1741234567890
}
```

Fetch the image at `GET /api/crops/{file}`.

---

### stats

Periodic summary, pushed every 30 seconds.

```json
{
  "type":     "stats",
  "today":    247,
  "uptime_s": 22440,
  "fps":      18.3,
  "tracks":   12,
  "db_mb":    1.4,
  "ts":       1741234567890
}
```

---

### health

System health, pushed every 30 seconds alongside `stats`.

```json
{
  "type":     "health",
  "temp_c":   42.1,
  "af_state": 2,
  "lens_pos": 2.40,
  "ts":       1741234567890
}
```

`af_state` — libcamera AF state integer (0 = idle, 1 = scanning, 2 = focused, 3 = failed).

---

### capture

Fired when detection is started or stopped via `POST /api/capture`.

```json
{
  "type":   "capture",
  "active": true,
  "ts":     1741234567890
}
```

---

## Typical sync flow

```
POST /api/sync/session          → { sessionId, pending }
GET  /api/sync/session/{id}     → { crops: [...] }
GET  /api/crops/{file}          → JPEG  (repeat per file)
POST /api/sync/ack              → { acked }  (can batch or per-file)
DELETE /api/sync/session/{id}   → { deleted, bytesFreed }
```
