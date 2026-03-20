# AI Trap — Data Flow Strategy

## Overview

The phone app sits in the middle of the data flow, acting as a field data mule
between the trap and the laptop. The trap has no direct path to the laptop.

```
Trap (RPi5 / Luckfox)  ──►  Phone App  ──►  Laptop
                        USB-C            removable media
                        field            home / office
```

---

## Trap → Phone (field visit)

The phone connects to the trap via **USB-C cable**. The trap presents itself as a
USB Ethernet gadget (RNDIS/CDC-ECM), giving the phone a point-to-point network
link to the trap's existing HTTP REST API — no WiFi needed, no pairing.

The Flutter app runs the existing pull-sync protocol:

1. `POST /api/sync/session` — open session, get count of pending crops
2. `GET /api/sync/session/{id}` — fetch manifest (file list + metadata)
3. `GET /api/crops/{session}/{file}` — download each JPEG
4. `POST /api/sync/ack` — confirm receipt
5. `DELETE /api/sync/session/{id}` — trap deletes files, frees disk

The phone stores the JPEGs and a sidecar metadata record per crop:

```json
{
  "trapId": "trap-001",
  "sessionKey": "20260314_153042",
  "file": "20260314_153042/insect_042.jpg",
  "trackId": 42,
  "label": "insect",
  "confidence": 0.91,
  "timestampUs": 1741960242000000,
  "capturedAt": "2026-03-14T15:30:42Z",
  "width": 224,
  "height": 224,
  "bytes": 18432
}
```

---

## Phone → Laptop (removable media)

The phone is a temporary store. The bulk data transfer to the laptop uses
removable media — the specific medium depends on the hardware platform.

### Raspberry Pi 5

The RPi5 has four free USB-A ports alongside USB-C power, so a **USB stick**
fits naturally:

- Trap mirrors unsynced crops to the stick when inserted (background export service)
- Crops are marked exported in SQLite; trap retains internal copy until confirmed
- In the field: pull stick, pocket it
- At home: plug stick into laptop — ingest service detects new volume, reads
  session manifests, imports

The phone USB-C connection is used for monitoring and control only; the stick
carries the bulk data independently.

### Luckfox Pico Zero

The Luckfox has a **single USB-C OTG port** — it can be a USB device (gadget
mode, for the phone) or a USB host (for a USB stick), but not both simultaneously.
A USB stick therefore conflicts with the phone connection.

The natural solution is the **microSD card** — the Luckfox already boots from and
stores crops on it. The card itself becomes the removable media:

- Pull the microSD from the Luckfox in the field
- Plug into phone via USB-C SD adapter → Flutter app reads for field preview
- Plug into laptop (built-in reader or dongle) → direct ingest

This is cleaner than a USB stick: no extra hardware, faster transfer via the
laptop's built-in SD reader, and the same card that holds the OS holds the data.
The trap is offline while the card is out — expected behaviour for this class
of field instrument.

| | RPi5 | Luckfox Pico Zero |
|---|---|---|
| Removable media | USB stick (USB-A port) | microSD card |
| Phone connection | USB-C gadget (independent) | USB-C OTG (conflicts with stick) |
| Laptop ingest | New USB volume detected | New SD card volume detected |

---

## Laptop ingest and ID pipeline

The laptop watches for a new volume to mount (launchd/systemd path watcher),
finds the session manifests, and ingests crops into the pipeline:

```
New volume mounted
        │
        ▼
Ingest service
• reads manifest.json per session
• copies JPEGs to ingest/<trapId>/<sessionKey>/
• writes pending rows to detections table
        │
        ▼
ID worker queue
        │
        ▼
Identification model
• runs classifier per crop
• writes ranked species candidates + confidence to identifications table
        │
        ▼
Server-side database (PostgreSQL / SQLite)
```

---

## Server-side database schema

```sql
-- One row per physical trap
traps (id, name, serial, lat, lon, location, created_at)

-- One row per capture session
capture_sessions (id, trap_id, session_key, started_at, ended_at, crop_count, synced_at)

-- One row per saved crop
detections (
    id, trap_id, session_id, track_id,
    file_key,           -- "<sessionKey>/<filename>"
    image_path,         -- local path to stored JPEG
    width, height,
    trap_class,         -- raw class from trap model  e.g. "insect"
    trap_confidence,
    timestamp_us, captured_at, received_at
)

-- ID results — one row per model run (keeps history if re-run)
identifications (
    id, detection_id,
    model_name, model_version,
    rank,               -- 1 = top hit
    taxon_id, taxon_name,
    confidence, run_at
)

-- Normalised taxonomy (GBIF backbone)
taxa (id, gbif_id, kingdom, phylum, class, order, family, genus, species, common_name)

-- Human review / verification
verifications (id, detection_id, taxon_id, reviewer, note, verified_at)
```

---

## On-device enrichment (Luckfox NPU)

The Luckfox Pico Zero's RV1103 NPU (0.5 TOPS) is already used for primary
detection via RKNN. YOLOv11n is small and unlikely to fully saturate it,
leaving headroom for a second, lighter model.

A MobileNetV3-small or EfficientNet-lite0 classifier running on the saved crop
(224×224, rare event — not every frame) could produce order-level classification
on-device before the data leaves the trap:

```
Frame stream → YOLOv11n (NPU) → ByteTracker → confirmed track
                                                     │
                                              CropSaver saves JPEG
                                                     │
                                         Classifier (NPU, on crop)
                                                     │
                                         label="Lepidoptera", conf=0.87
                                         written to SQLite + sidecar JSON
```

This enriches the data at every stage:

| Without on-device classifier | With on-device classifier |
|---|---|
| `label: "insect"` | `label: "Lepidoptera", conf: 0.87` |
| Phone shows "342 insects" | Phone shows "231 moths, 67 flies, 44 beetles" |
| Laptop must ID everything | Laptop refines to species level only |

The laptop ID pipeline can then use the order prediction to select a specialist
model — a Lepidoptera-only classifier will outperform a generalist — and the
phone app can display a meaningful field summary without any connectivity.

---

## Query and analysis

Once the DB is populated, representative queries include:

- **Activity curves** — detections per hour/night by species
- **Phenology** — species first/last date per year, flight period
- **Trap comparison** — diversity index, abundance per site
- **Session summaries** — crops received, identified, unidentified %
- **Review queue** — detections where confidence < threshold, no human verification

Recommended tooling: **Grafana** (PostgreSQL datasource) for dashboards,
**Jupyter + pandas** for analysis, **FastAPI** for a REST layer if the phone
app needs to query historical data.

---

## Build order

1. **Sync agent** — Python service implementing pull protocol, stores crops, writes `detections` rows
2. **Laptop receiver service** — watches for new volume, triggers ingest
3. **ID worker** — BioClip or fine-tuned EfficientNet, writes `identifications` rows
4. **DB schema + migrations** — Alembic-managed PostgreSQL
5. **Basic dashboard** — Metabase or Grafana (zero-code, quick win)
6. **Phone offload UI** — Flutter screens for pending batches and offload status
7. **On-device classifier** (optional) — RKNN MobileNet, order-level only
