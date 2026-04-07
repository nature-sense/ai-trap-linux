"""
Generate ai-trap-linux architecture presentation as a PDF.
Slide-style layout: 1280x720 (16:9), dark theme.
"""

from reportlab.lib.pagesizes import landscape
from reportlab.pdfgen import canvas
from reportlab.lib import colors
from reportlab.lib.units import inch
from reportlab.platypus import Paragraph, Table, TableStyle
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.enums import TA_LEFT, TA_CENTER

# --- Dimensions ---
W, H = 1280, 720

# --- Palette ---
BG       = colors.HexColor("#0D1117")   # near-black
PANEL    = colors.HexColor("#161B22")   # card
BORDER   = colors.HexColor("#30363D")   # subtle border
ACCENT   = colors.HexColor("#238636")   # green accent
ACCENT2  = colors.HexColor("#1F6FEB")   # blue accent
GOLD     = colors.HexColor("#D29922")   # yellow/amber
TEXT     = colors.HexColor("#E6EDF3")   # primary text
MUTED    = colors.HexColor("#8B949E")   # secondary text
RED      = colors.HexColor("#DA3633")
PURPLE   = colors.HexColor("#8957E5")
TEAL     = colors.HexColor("#39D353")

FONT     = "Helvetica"
FONT_B   = "Helvetica-Bold"
FONT_O   = "Helvetica-Oblique"

# ── helpers ──────────────────────────────────────────────────────────────────

def bg(c):
    c.setFillColor(BG)
    c.rect(0, 0, W, H, fill=1, stroke=0)

def slide_number(c, n, total):
    c.setFont(FONT, 11)
    c.setFillColor(MUTED)
    c.drawRightString(W - 30, 18, f"{n} / {total}")

def accent_bar(c, x=0, y=None, w=None, h=4, color=ACCENT):
    if y is None: y = H - 2
    if w is None: w = W
    c.setFillColor(color)
    c.rect(x, y - h, w, h, fill=1, stroke=0)

def title_block(c, title, subtitle=None, y=H - 80):
    c.setFont(FONT_B, 32)
    c.setFillColor(TEXT)
    c.drawString(60, y, title)
    if subtitle:
        c.setFont(FONT_O, 16)
        c.setFillColor(MUTED)
        c.drawString(62, y - 30, subtitle)
    # underline
    c.setStrokeColor(ACCENT)
    c.setLineWidth(2)
    c.line(60, y - 38, 60 + len(title) * 18, y - 38)

def card(c, x, y, w, h, fill=PANEL, radius=8):
    c.setFillColor(fill)
    c.setStrokeColor(BORDER)
    c.setLineWidth(1)
    c.roundRect(x, y, w, h, radius, fill=1, stroke=1)

def pill(c, x, y, label, bg_col=ACCENT, text_col=TEXT, font_size=12):
    tw = len(label) * 7.2 + 16
    c.setFillColor(bg_col)
    c.roundRect(x, y, tw, 22, 5, fill=1, stroke=0)
    c.setFont(FONT_B, font_size)
    c.setFillColor(text_col)
    c.drawString(x + 8, y + 6, label)
    return tw

def bullet_list(c, items, x, y, line_h=24, font_size=14, color=TEXT, muted_color=MUTED, indent=20):
    for item in items:
        if isinstance(item, tuple):
            text, col = item
        else:
            text, col = item, color
        c.setFillColor(ACCENT)
        c.circle(x + 6, y + 5, 3, fill=1, stroke=0)
        c.setFont(FONT, font_size)
        c.setFillColor(col)
        c.drawString(x + indent, y, text)
        y -= line_h
    return y

def section_label(c, text, x, y, col=MUTED):
    c.setFont(FONT_B, 10)
    c.setFillColor(col)
    c.drawString(x, y, text.upper())

def arrow_h(c, x1, y, x2, col=MUTED):
    c.setStrokeColor(col)
    c.setLineWidth(1.5)
    c.line(x1, y, x2, y)
    # arrowhead via path
    c.setFillColor(col)
    p = c.beginPath()
    p.moveTo(x2, y)
    p.lineTo(x2 - 8, y + 4)
    p.lineTo(x2 - 8, y - 4)
    p.close()
    c.drawPath(p, fill=1, stroke=0)

def arrow_v(c, x, y1, y2, col=MUTED):
    c.setStrokeColor(col)
    c.setLineWidth(1.5)
    c.line(x, y1, x, y2)
    c.setFillColor(col)
    p = c.beginPath()
    p.moveTo(x, y2)
    p.lineTo(x - 4, y2 + 8)
    p.lineTo(x + 4, y2 + 8)
    p.close()
    c.drawPath(p, fill=1, stroke=0)

def box_label(c, x, y, w, h, label, sublabel=None, fill=PANEL, label_col=TEXT, sub_col=MUTED, font_size=13):
    card(c, x, y, w, h, fill=fill)
    c.setFont(FONT_B, font_size)
    c.setFillColor(label_col)
    cx = x + w / 2
    cy = y + h / 2 + (8 if sublabel else 0)
    c.drawCentredString(cx, cy, label)
    if sublabel:
        c.setFont(FONT_O, 10)
        c.setFillColor(sub_col)
        c.drawCentredString(cx, y + h / 2 - 10, sublabel)

# =============================================================================
# SLIDES
# =============================================================================

def slide_title(c, n, total):
    bg(c)
    accent_bar(c, color=ACCENT)

    # Big logo/icon area
    c.setFillColor(PANEL)
    c.roundRect(60, H - 380, 180, 180, 12, fill=1, stroke=0)
    c.setFont(FONT_B, 72)
    c.setFillColor(ACCENT)
    c.drawCentredString(150, H - 260, "🪲")   # insect emoji — fallback gracefully

    # Title
    c.setFont(FONT_B, 52)
    c.setFillColor(TEXT)
    c.drawString(270, H - 200, "ai-trap-linux")

    c.setFont(FONT, 22)
    c.setFillColor(MUTED)
    c.drawString(272, H - 240, "Embedded AI Insect Detection System")

    # Divider
    c.setStrokeColor(BORDER)
    c.setLineWidth(1)
    c.line(270, H - 260, W - 60, H - 260)

    # Subtitle bullets
    items = [
        "Real-time YOLO11n inference on edge hardware",
        "Raspberry Pi CM5 (aarch64) + Luckfox Pico Zero (armv7)",
        "NCNN CPU inference — optional Rockchip NPU acceleration",
        "REST / SSE / MJPEG APIs  •  SQLite storage  •  Cloud sync",
    ]
    y = H - 300
    for item in items:
        c.setFillColor(ACCENT)
        c.circle(276, y + 5, 3, fill=1, stroke=0)
        c.setFont(FONT, 16)
        c.setFillColor(MUTED)
        c.drawString(290, y, item)
        y -= 28

    # Footer
    c.setFont(FONT, 12)
    c.setFillColor(MUTED)
    c.drawString(60, 22, "NatureSense  •  2026")
    slide_number(c, n, total)
    c.showPage()


def slide_hardware(c, n, total):
    bg(c)
    accent_bar(c, color=ACCENT2)
    title_block(c, "Hardware Targets", "Two platforms, one shared codebase")
    slide_number(c, n, total)

    # --- Pi CM5 card ---
    card(c, 60, 160, 530, 350)
    c.setFont(FONT_B, 18)
    c.setFillColor(ACCENT2)
    c.drawString(80, 488, "Raspberry Pi Compute Module 5")
    c.setFont(FONT, 13)
    c.setFillColor(MUTED)
    c.drawString(80, 468, "Production: CM5-NANO-A  |  Dev: CM5-IO-BASE-A")

    rows_pi = [
        ("SoC",       "Broadcom BCM2712  (aarch64, 4× Cortex-A76)"),
        ("Camera",    "IMX708  —  libcamera stack  (cam1)"),
        ("Binary",    "yolo_libcamera"),
        ("Inference", "NCNN CPU  (ASIMD / FP16 / dot-product)"),
        ("WiFi mgr",  "NetworkManager"),
        ("Storage",   "eMMC  (no SD card)  —  flashed via rpiboot"),
        ("OS",        "Raspberry Pi OS / Debian Trixie 64-bit Lite"),
        ("Devices",   "trap006 (IO-BASE-A),  trap001 (Nano, WiFi)"),
    ]
    y = 438
    for k, v in rows_pi:
        c.setFont(FONT_B, 12)
        c.setFillColor(GOLD)
        c.drawString(80, y, k + ":")
        c.setFont(FONT, 12)
        c.setFillColor(TEXT)
        c.drawString(175, y, v)
        y -= 22

    # thermal warning pill
    c.setFillColor(RED)
    c.roundRect(80, 170, 240, 22, 5, fill=1, stroke=0)
    c.setFont(FONT_B, 11)
    c.setFillColor(TEXT)
    c.drawString(88, 176, "Nano: thermal throttle risk — cap framerate")

    # --- Luckfox card ---
    card(c, 620, 160, 530, 350)
    c.setFont(FONT_B, 18)
    c.setFillColor(PURPLE)
    c.drawString(640, 488, "Luckfox Pico Zero")
    c.setFont(FONT, 13)
    c.setFillColor(MUTED)
    c.drawString(640, 468, "Ultra-compact edge node  —  RV1106G3")

    rows_lf = [
        ("SoC",       "Rockchip RV1106G3  (armv7, Cortex-A7)"),
        ("Camera",    "IMX415  —  V4L2 interface"),
        ("Binary",    "yolo_v4l2"),
        ("Inference", "NCNN CPU  or  RKNN NPU  (10× faster)"),
        ("WiFi mgr",  "hostapd / wpa_supplicant"),
        ("Toolchain", "arm-rockchip830-linux-uclibcgnueabihf"),
        ("OS",        "Buildroot Linux  (uclibc)"),
        ("NPU",       "Optional: -Duse_rknn=true at build time"),
    ]
    y = 438
    for k, v in rows_lf:
        c.setFont(FONT_B, 12)
        c.setFillColor(GOLD)
        c.drawString(640, y, k + ":")
        c.setFont(FONT, 12)
        c.setFillColor(TEXT)
        c.drawString(730, y, v)
        y -= 22

    c.setFillColor(ACCENT)
    c.roundRect(640, 170, 220, 22, 5, fill=1, stroke=0)
    c.setFont(FONT_B, 11)
    c.setFillColor(TEXT)
    c.drawString(648, 176, "NPU: ~10x faster than CPU inference")

    c.showPage()


def slide_system_architecture(c, n, total):
    bg(c)
    accent_bar(c, color=PURPLE)
    title_block(c, "System Architecture", "Top-level component view")
    slide_number(c, n, total)

    # Three layers: capture, pipeline, output

    layer_y = [480, 320, 160]
    layer_labels = ["CAPTURE LAYER", "PROCESSING PIPELINE", "OUTPUT LAYER"]
    layer_cols   = [ACCENT2, ACCENT, GOLD]

    for i, (ly, ll, lc) in enumerate(zip(layer_y, layer_labels, layer_cols)):
        c.setFillColor(lc)
        c.setFont(FONT_B, 9)
        c.drawString(62, ly + 88, ll)
        c.setStrokeColor(lc)
        c.setLineWidth(0.5)
        c.setDash(4, 3)
        c.roundRect(60, ly, W - 120, 80, 6, fill=0, stroke=1)
        c.setDash()

    # Capture layer boxes
    boxes_cap = [
        (90,  490, 180, 52, "libcamera\ncapture", ACCENT2),
        (300, 490, 180, 52, "V4L2\ncapture",      PURPLE),
        (510, 490, 180, 52, "NCNN\nCPU infer",    ACCENT),
        (720, 490, 180, 52, "RKNN NPU\n(optional)", GOLD),
        (930, 490, 180, 52, "Frame\nbuffer",       PANEL),
    ]
    for bx, by, bw, bh, label, col in boxes_cap:
        card(c, bx, by, bw, bh, fill=col)
        lines = label.split("\n")
        cy = by + bh / 2 + 8 if len(lines) > 1 else by + bh / 2
        for line in lines:
            c.setFont(FONT_B, 12)
            c.setFillColor(BG if col not in [PANEL] else TEXT)
            c.drawCentredString(bx + bw / 2, cy, line)
            cy -= 16

    # Pipeline layer
    pipe_boxes = [
        (90,  330, 140, 52, "YOLO\nDecoder"),
        (260, 330, 140, 52, "NMS\nFilter"),
        (430, 330, 140, 52, "ByteTracker"),
        (600, 330, 140, 52, "Crop\nSaver"),
        (770, 330, 140, 52, "SQLite\nStorage"),
        (940, 330, 140, 52, "Sync\nManager"),
    ]
    for i, (bx, by, bw, bh, label) in enumerate(pipe_boxes):
        fill = PANEL
        card(c, bx, by, bw, bh, fill=fill)
        lines = label.split("\n")
        cy = by + bh / 2 + 8 if len(lines) > 1 else by + bh / 2
        for line in lines:
            c.setFont(FONT_B, 12)
            c.setFillColor(TEXT)
            c.drawCentredString(bx + bw / 2, cy, line)
            cy -= 16
        if i < len(pipe_boxes) - 1:
            nx = pipe_boxes[i+1][0]
            arrow_h(c, bx + bw + 2, by + bh / 2, nx - 2, col=MUTED)

    # Output layer
    out_boxes = [
        (90,  170, 200, 52, "HTTP REST\n:8080",  ACCENT2),
        (320, 170, 200, 52, "SSE Events\n:8081", ACCENT),
        (550, 170, 200, 52, "MJPEG Stream\n:9000", PURPLE),
        (780, 170, 200, 52, "JPEG Crops\n/opt/ai-trap/crops", GOLD),
        (1010,170, 170, 52, "SQLite DB\n/opt/ai-trap/data", PANEL),
    ]
    for bx, by, bw, bh, label, col in out_boxes:
        card(c, bx, by, bw, bh, fill=col)
        lines = label.split("\n")
        cy = by + bh / 2 + 8 if len(lines) > 1 else by + bh / 2
        for line in lines:
            c.setFont(FONT_B, 11)
            c.setFillColor(BG if col != PANEL else TEXT)
            c.drawCentredString(bx + bw / 2, cy, line)
            cy -= 16

    # Vertical arrows between layers
    arrow_v(c, 180, 490, 385, col=MUTED)
    arrow_v(c, 510, 490, 385, col=MUTED)
    arrow_v(c, 360, 330, 225, col=MUTED)
    arrow_v(c, 700, 330, 225, col=MUTED)

    c.showPage()


def slide_detection_pipeline(c, n, total):
    bg(c)
    accent_bar(c, color=ACCENT)
    title_block(c, "Detection Pipeline", "Camera frame to confirmed insect track")
    slide_number(c, n, total)

    # Pipeline flow diagram
    stages = [
        ("Camera\nFrame",   BG,     TEXT,   "libcamera / V4L2\n1536x864 or 2304x1296"),
        ("Pre-process",     ACCENT2, BG,    "Resize to 320x320\nNormalize RGB"),
        ("NCNN / RKNN\nInfer", ACCENT, BG,  "YOLO11n forward pass\nSingle-class: insects"),
        ("Decode\nBoxes",   PANEL,  TEXT,   "Anchor-grid or E2E\nformat auto-detected"),
        ("NMS",             PANEL,  TEXT,   "IoU threshold\nConf threshold: 0.45"),
        ("ByteTracker",     PURPLE, TEXT,   "Kalman filter\nHigh + low pass match"),
        ("Confirmed\nTrack",GOLD,   BG,     "min_hits = 3 frames\nTrack ID assigned"),
    ]

    bw, bh = 140, 70
    gap = 18
    total_w = len(stages) * bw + (len(stages) - 1) * gap
    start_x = (W - total_w) / 2
    y_box = 380

    for i, (label, fill, tcol, desc) in enumerate(stages):
        bx = start_x + i * (bw + gap)
        card(c, bx, y_box, bw, bh, fill=fill)
        lines = label.split("\n")
        cy = y_box + bh / 2 + 8 if len(lines) > 1 else y_box + bh / 2
        for line in lines:
            c.setFont(FONT_B, 12)
            c.setFillColor(tcol)
            c.drawCentredString(bx + bw / 2, cy, line)
            cy -= 16
        # description below
        dlines = desc.split("\n")
        dy = y_box - 20
        for dl in dlines:
            c.setFont(FONT, 10)
            c.setFillColor(MUTED)
            c.drawCentredString(bx + bw / 2, dy, dl)
            dy -= 14
        # arrow
        if i < len(stages) - 1:
            ax1 = bx + bw + 2
            ax2 = bx + bw + gap - 2
            arrow_h(c, ax1, y_box + bh / 2, ax2, col=MUTED)

    # After confirmed track
    y_out = 230
    out_items = [
        ("Crop JPEG saved\n(async queue)", 120,  y_out, PANEL),
        ("SQLite row\ninserted",           360,  y_out, PANEL),
        ("SSE event\nemitted",             590,  y_out, PANEL),
        ("HTTP API\nserves crops",         820,  y_out, PANEL),
        ("Cloud sync\nmanifest updated",  1050,  y_out, PANEL),
    ]
    c.setFont(FONT_B, 11)
    c.setFillColor(MUTED)
    c.drawCentredString(W/2, 290, "On confirmed track:")

    # Arrow down
    cx_track = start_x + 6 * (bw + gap) + bw / 2
    arrow_v(c, cx_track, y_box, 260, col=GOLD)

    for label, ox, oy, fill in out_items:
        card(c, ox, oy, 185, 52, fill=fill)
        lines = label.split("\n")
        cy2 = oy + 26 + 8 if len(lines) > 1 else oy + 26
        for line in lines:
            c.setFont(FONT, 11)
            c.setFillColor(TEXT)
            c.drawCentredString(ox + 92, cy2, line)
            cy2 -= 14

    # Bottom note
    c.setFont(FONT_O, 11)
    c.setFillColor(MUTED)
    c.drawCentredString(W/2, 150, "Crop saver limits saves per track: max 3 crops, re-save only if confidence improves by >5%")

    c.showPage()


def slide_capture(c, n, total):
    bg(c)
    accent_bar(c, color=ACCENT2)
    title_block(c, "Camera Capture Subsystem", "Platform-specific backends, unified frame interface")
    slide_number(c, n, total)

    # Left: libcamera
    card(c, 60, 160, 530, 390)
    c.setFont(FONT_B, 16)
    c.setFillColor(ACCENT2)
    c.drawString(80, 528, "libcamera_capture  (Pi 5)")
    c.setFont(FONT, 12)
    c.setFillColor(MUTED)
    c.drawString(80, 510, "firmware/src/io/libcamera_capture.cpp  —  607 loc")

    items_lc = [
        "IMX708 via modern libcamera stack",
        "2x2 binned capture: 1536x864 (stable) or 2304x1296",
        "Autofocus: manual / auto / continuous modes",
        "Lens position configurable (normal / macro / full range)",
        "ISP tuning: brightness, contrast, saturation, sharpness",
        "Framerate + DMA buffer count configurable in TOML",
        "Sets LIBCAMERA_IPA_MODULE_PATH to system paths",
        "Env: LIBCAMERA_IPA_PROXY_PATH (avoids bundled IPA panic)",
    ]
    y = 488
    for item in items_lc:
        c.setFillColor(ACCENT2)
        c.circle(86, y + 5, 3, fill=1, stroke=0)
        c.setFont(FONT, 12)
        c.setFillColor(TEXT)
        c.drawString(100, y, item)
        y -= 24

    c.setFillColor(RED)
    c.roundRect(80, 170, 340, 22, 5, fill=1, stroke=0)
    c.setFont(FONT_B, 10)
    c.setFillColor(TEXT)
    c.drawString(88, 176, "2304x1296 → kernel hard-reset on CM5 (35 MB NCNN mat)")

    # Right: v4l2
    card(c, 620, 160, 530, 390)
    c.setFont(FONT_B, 16)
    c.setFillColor(PURPLE)
    c.drawString(640, 528, "v4l2_capture  (Luckfox)")
    c.setFont(FONT, 12)
    c.setFillColor(MUTED)
    c.drawString(640, 510, "firmware/src/io/v4l2_capture.cpp  —  485 loc")

    items_v4 = [
        "IMX415 via legacy V4L2 kernel interface",
        "Capture resolution: 1536x864 (stable on RV1106G3)",
        "MMAP-based zero-copy buffer management",
        "Framerate and buffer count configurable",
        "Outputs raw frames → NCNN pre-processor",
    ]
    y = 488
    for item in items_v4:
        c.setFillColor(PURPLE)
        c.circle(646, y + 5, 3, fill=1, stroke=0)
        c.setFont(FONT, 12)
        c.setFillColor(TEXT)
        c.drawString(660, y, item)
        y -= 24

    # RKNN box
    card(c, 640, 290, 490, 120, fill=colors.HexColor("#1C1025"))
    c.setFont(FONT_B, 14)
    c.setFillColor(GOLD)
    c.drawString(660, 390, "rknn_infer  (optional NPU)")
    c.setFont(FONT, 12)
    c.setFillColor(MUTED)
    c.drawString(660, 372, "firmware/src/io/rknn_infer.cpp  —  183 loc")
    items_rk = [
        "Rockchip NPU runtime via librknnrt.so",
        "~10x faster than NCNN CPU on RV1106G3",
        "Enabled with -Duse_rknn=true at configure time",
        "Model: firmware/models/yolo11n-320/model.rknn",
    ]
    y = 354
    for item in items_rk:
        c.setFillColor(GOLD)
        c.circle(666, y + 5, 3, fill=1, stroke=0)
        c.setFont(FONT, 12)
        c.setFillColor(TEXT)
        c.drawString(680, y, item)
        y -= 20

    c.setFillColor(ACCENT)
    c.roundRect(640, 170, 340, 22, 5, fill=1, stroke=0)
    c.setFont(FONT_B, 10)
    c.setFillColor(TEXT)
    c.drawString(648, 176, "CPU fallback always available — NPU is additive")

    c.showPage()


def slide_inference_tracking(c, n, total):
    bg(c)
    accent_bar(c, color=PURPLE)
    title_block(c, "Inference, Decoding & Tracking", "YOLO11n → NMS → ByteTracker")
    slide_number(c, n, total)

    # Three cards across
    cards = [
        {
            "title": "NCNN Inference",
            "file":  "NCNN  (Tencent/ncnn submodule)",
            "col":   ACCENT,
            "x":     60,
            "items": [
                "YOLO11n model — single class (insects)",
                "Input: 320×320 RGB normalized",
                "ASIMD dot-product + FP16 on aarch64",
                "NEON intrinsics on armv7 (Luckfox)",
                "Statically linked — zero system deps",
                "OpenMP parallelism (libgomp)",
                "Model files: model.ncnn.param + .bin",
            ]
        },
        {
            "title": "Decoder  (decoder.cpp)",
            "file":  "358 loc  —  firmware/src/pipeline/",
            "col":   ACCENT2,
            "x":     460,
            "items": [
                "Supports two YOLO output formats:",
                "  • Anchor-grid (default YOLO11n)",
                "  • End-to-end (E2E detection head)",
                "Format auto-detected via tensor shape",
                "Confidence threshold filtering",
                "Box sanity checks: min/max dims,",
                "  aspect ratio, area ratio limits",
            ]
        },
        {
            "title": "ByteTracker  (tracker.cpp)",
            "file":  "307 loc  —  firmware/src/pipeline/",
            "col":   PURPLE,
            "x":     860,
            "items": [
                "Lightweight Kalman filter tracking",
                "Two-pass detection matching:",
                "  • High-confidence detections first",
                "  • Low-confidence second pass",
                "Track confirmed after N=3 frames",
                "Track deleted after M=30 missed frames",
                "Outputs: ID, age, confidence, bbox",
            ]
        },
    ]

    for card_data in cards:
        cx = card_data["x"]
        card(c, cx, 160, 370, 400)
        c.setFont(FONT_B, 15)
        c.setFillColor(card_data["col"])
        c.drawString(cx + 16, 542, card_data["title"])
        c.setFont(FONT_O, 11)
        c.setFillColor(MUTED)
        c.drawString(cx + 16, 524, card_data["file"])
        c.setStrokeColor(BORDER)
        c.setLineWidth(0.5)
        c.line(cx + 16, 516, cx + 354, 516)

        y = 498
        for item in card_data["items"]:
            if item.startswith("  •"):
                c.setFont(FONT, 11)
                c.setFillColor(MUTED)
                c.drawString(cx + 36, y, item.strip())
            elif item.endswith(":"):
                c.setFont(FONT_B, 12)
                c.setFillColor(TEXT)
                c.drawString(cx + 20, y, item)
            else:
                c.setFillColor(card_data["col"])
                c.circle(cx + 24, y + 5, 3, fill=1, stroke=0)
                c.setFont(FONT, 12)
                c.setFillColor(TEXT)
                c.drawString(cx + 36, y, item)
            y -= 22

    # Bottom NMS note
    card(c, 60, 110, 1160, 40, fill=PANEL)
    c.setFont(FONT, 13)
    c.setFillColor(MUTED)
    c.drawCentredString(W/2, 125, "NMS between Decoder and ByteTracker: IoU-based suppression removes overlapping detections before tracking")

    c.showPage()


def slide_storage_sync(c, n, total):
    bg(c)
    accent_bar(c, color=GOLD)
    title_block(c, "Storage & Sync", "JPEG crops, SQLite detections, cloud sync protocol")
    slide_number(c, n, total)

    # crop_saver
    card(c, 60, 380, 360, 240)
    c.setFont(FONT_B, 15)
    c.setFillColor(ACCENT)
    c.drawString(80, 600, "crop_saver.cpp  (298 loc)")
    items = [
        "Async JPEG encoding — never stalls pipeline",
        "Configurable queue depth",
        "Per-track save limit: max 3 crops",
        "Re-save guard: conf delta > 5%",
        "JPEG quality: default 90",
        "Output: /opt/ai-trap/crops/",
    ]
    y = 580
    for item in items:
        c.setFillColor(ACCENT)
        c.circle(74, y + 5, 3, fill=1, stroke=0)
        c.setFont(FONT, 12)
        c.setFillColor(TEXT)
        c.drawString(88, y, item)
        y -= 24

    # exif_writer
    card(c, 60, 160, 360, 200)
    c.setFont(FONT_B, 15)
    c.setFillColor(GOLD)
    c.drawString(80, 340, "exif_writer.cpp  (296 loc)")
    items2 = [
        "GPS lat/lon/alt from TOML config",
        "Timestamp embedded in EXIF",
        "YOLO model version tag",
        "Session ID for grouping captures",
    ]
    y = 320
    for item in items2:
        c.setFillColor(GOLD)
        c.circle(74, y + 5, 3, fill=1, stroke=0)
        c.setFont(FONT, 12)
        c.setFillColor(TEXT)
        c.drawString(88, y, item)
        y -= 24

    # persistence
    card(c, 450, 380, 360, 240)
    c.setFont(FONT_B, 15)
    c.setFillColor(ACCENT2)
    c.drawString(470, 600, "persistence.cpp  (499 loc)")
    items3 = [
        "SQLite3 — statically linked",
        "Schema: timestamp, track_id,",
        "  x1,y1,x2,y2, confidence, class",
        "WAL mode + periodic checkpoints",
        "Graceful corrupted-DB recovery",
        "Path: /opt/ai-trap/data/detections.db",
    ]
    y = 580
    for item in items3:
        if item.startswith("  "):
            c.setFont(FONT, 12)
            c.setFillColor(MUTED)
            c.drawString(480 + 14, y, item.strip())
        else:
            c.setFillColor(ACCENT2)
            c.circle(464, y + 5, 3, fill=1, stroke=0)
            c.setFont(FONT, 12)
            c.setFillColor(TEXT)
            c.drawString(478, y, item)
        y -= 22

    # sync_manager
    card(c, 450, 160, 360, 200)
    c.setFont(FONT_B, 15)
    c.setFillColor(PURPLE)
    c.drawString(470, 340, "sync_manager.cpp  (494 loc)")
    items4 = [
        "Stateless pull-based sync protocol",
        "Cloud gateway calls REST to pull crops",
        "Session-based tracking of synced items",
        "Unsynced crop manifest generation",
        "No SSH/SCP — pure HTTP",
    ]
    y = 320
    for item in items4:
        c.setFillColor(PURPLE)
        c.circle(464, y + 5, 3, fill=1, stroke=0)
        c.setFont(FONT, 12)
        c.setFillColor(TEXT)
        c.drawString(478, y, item)
        y -= 24

    # Flow arrows
    card(c, 840, 160, 380, 460, fill=PANEL)
    c.setFont(FONT_B, 14)
    c.setFillColor(TEXT)
    c.drawString(860, 600, "Sync REST Endpoints")
    c.setFont(FONT_B, 9)
    c.setFillColor(MUTED)
    endpoints = [
        ("POST", "/api/sync/session",           "Create sync session"),
        ("GET",  "/api/sync/session/{id}",       "Get unsynced manifest"),
        ("GET",  "/api/crops/{path}",            "Download crop JPEG"),
        ("POST", "/api/sync/ack",               "Acknowledge synced crops"),
        ("DEL",  "/api/sync/session/{id}",       "Close session"),
    ]
    y = 575
    for method, path, desc in endpoints:
        col = ACCENT if method == "GET" else (GOLD if method == "POST" else RED)
        c.setFillColor(col)
        c.roundRect(858, y, 45, 16, 3, fill=1, stroke=0)
        c.setFont(FONT_B, 9)
        c.setFillColor(BG)
        c.drawString(862, y + 4, method)
        c.setFont(FONT, 11)
        c.setFillColor(TEXT)
        c.drawString(910, y + 3, path)
        c.setFont(FONT_O, 10)
        c.setFillColor(MUTED)
        c.drawString(862, y - 9, desc)
        y -= 40

    c.showPage()


def slide_servers(c, n, total):
    bg(c)
    accent_bar(c, color=TEAL)
    title_block(c, "HTTP, SSE & MJPEG Servers", "Three independent servers — REST, real-time events, live video")
    slide_number(c, n, total)

    servers = [
        {
            "title": "http_server.cpp  (758 loc)",
            "port":  ":8080",
            "col":   ACCENT2,
            "x":     60,
            "items": [
                "REST API — JSON responses, CORS headers",
                "GET  /api/trap         Trap ID + location",
                "POST /api/capture      Start/stop detection",
                "GET  /api/status       FPS, uptime, counts",
                "GET  /api/crops        List + download JPEGs",
                "POST /api/config/*     Location, threshold",
                "POST /api/af/trigger   One-shot autofocus",
                "GET  /api/events       Redirects → SSE :8081",
                "POST /api/sync/*       Cloud sync session",
            ]
        },
        {
            "title": "sse_server.cpp  (241 loc)",
            "port":  ":8081",
            "col":   ACCENT,
            "x":     460,
            "items": [
                "Server-Sent Events — long-poll HTTP/1.1",
                "Event: detection       Raw YOLO detection",
                "Event: track_confirmed Track after min_hits",
                "Event: crop_saved      JPEG written to disk",
                "Event: capture_start   Session begin",
                "Event: capture_stop    Session end",
                "Max clients: 8 (configurable)",
                "Backpressure queue depth configurable",
                "Flutter app subscribes for live updates",
            ]
        },
        {
            "title": "mjpeg_streamer.cpp  (350 loc)",
            "port":  ":9000",
            "col":   PURPLE,
            "x":     860,
            "items": [
                "Always-on MJPEG live stream",
                "Downscaled: 640x480 (default)",
                "JPEG quality: 75 (saves CPU vs crops)",
                "Independent from detection pipeline",
                "Multipart/x-mixed-replace boundary",
                "Viewable in browser or Flutter app",
                "Port configurable in TOML",
                "Does NOT require capture to be running",
            ]
        },
    ]

    for srv in servers:
        cx = srv["x"]
        card(c, cx, 155, 370, 420)

        # Port pill
        pill(c, cx + 16, 548, srv["port"], bg_col=srv["col"])

        c.setFont(FONT_B, 14)
        c.setFillColor(srv["col"])
        c.drawString(cx + 16, 528, srv["title"])

        c.setStrokeColor(BORDER)
        c.setLineWidth(0.5)
        c.line(cx + 16, 520, cx + 354, 520)

        y = 502
        for item in srv["items"]:
            if any(item.startswith(m) for m in ("GET", "POST", "DEL", "Event:")):
                parts = item.split(None, 1)
                kw, rest = (parts[0], parts[1]) if len(parts) > 1 else (parts[0], "")
                col2 = ACCENT if kw in ("GET", "Event:") else (GOLD if kw == "POST" else RED)
                c.setFillColor(col2)
                c.roundRect(cx + 20, y, 50, 16, 3, fill=1, stroke=0)
                c.setFont(FONT_B, 9)
                c.setFillColor(BG)
                c.drawString(cx + 23, y + 4, kw)
                c.setFont(FONT, 11)
                c.setFillColor(TEXT)
                c.drawString(cx + 76, y + 3, rest)
            else:
                c.setFillColor(srv["col"])
                c.circle(cx + 24, y + 5, 3, fill=1, stroke=0)
                c.setFont(FONT, 12)
                c.setFillColor(TEXT)
                c.drawString(cx + 36, y, item)
            y -= 22

    c.showPage()


def slide_common_services(c, n, total):
    bg(c)
    accent_bar(c, color=GOLD)
    title_block(c, "Common Services", "WiFi provisioning, BLE GATT, e-Paper display")
    slide_number(c, n, total)

    svcs = [
        {
            "title": "wifi_manager.cpp",
            "sub":   "475 loc  —  firmware/src/common/",
            "col":   ACCENT2,
            "x": 60, "y": 280, "w": 360, "h": 260,
            "items": [
                "AP / Station mode switching",
                "Pi 5: NetworkManager backend",
                "Luckfox: hostapd + wpa_supplicant",
                "AP SSID/password from TOML",
                "Credential file path configurable",
                "Managed mode flag in config",
            ]
        },
        {
            "title": "ble_gatt_server.cpp",
            "sub":   "612 loc  —  firmware/src/common/",
            "col":   PURPLE,
            "x": 450, "y": 280, "w": 360, "h": 260,
            "items": [
                "BLE GATT provisioning server",
                "Raw HCI sockets — no libbluetooth",
                "No dynamic dep on BlueZ runtime",
                "Exposes WiFi credential GATT service",
                "Flutter app pairs + sends credentials",
                "Device ready confirmation characteristic",
            ]
        },
        {
            "title": "epaper_display.cpp",
            "sub":   "634 loc  —  firmware/src/common/",
            "col":   TEAL,
            "x": 840, "y": 280, "w": 360, "h": 260,
            "items": [
                "Waveshare 2.13\" e-Paper HAT",
                "SPI interface — GPIO configurable",
                "250×122 px B&W bitmap rendering",
                "Shows trap ID, detection counts",
                "Power-efficient (holds image off)",
                "Enable/disable flag in TOML",
            ]
        },
        {
            "title": "config_loader.h",
            "sub":   "~16 KB header  —  firmware/src/common/",
            "col":   GOLD,
            "x": 60, "y": 155, "w": 1140, "h": 100,
            "items": [
                "Header-only TOML parser → struct TrapConfig",
                "All subsystem settings in one type-safe struct",
                "Loaded at startup from /opt/ai-trap/config/trap_config.toml",
                "Runtime changes via POST /api/config/* (confidence threshold, GPS coords)",
            ],
            "horizontal": True,
        },
    ]

    for svc in svcs:
        card(c, svc["x"], svc["y"], svc["w"], svc["h"])
        c.setFont(FONT_B, 15)
        c.setFillColor(svc["col"])
        c.drawString(svc["x"] + 16, svc["y"] + svc["h"] - 22, svc["title"])
        c.setFont(FONT_O, 11)
        c.setFillColor(MUTED)
        c.drawString(svc["x"] + 16, svc["y"] + svc["h"] - 40, svc["sub"])
        c.setStrokeColor(BORDER)
        c.setLineWidth(0.5)
        c.line(svc["x"] + 16, svc["y"] + svc["h"] - 48, svc["x"] + svc["w"] - 16, svc["y"] + svc["h"] - 48)

        if svc.get("horizontal"):
            y = svc["y"] + svc["h"] - 70
            x0 = svc["x"] + 16
            for item in svc["items"]:
                c.setFillColor(svc["col"])
                c.circle(x0 + 4, y + 5, 3, fill=1, stroke=0)
                c.setFont(FONT, 12)
                c.setFillColor(TEXT)
                c.drawString(x0 + 16, y, item)
                x0 += 285
        else:
            y = svc["y"] + svc["h"] - 68
            for item in svc["items"]:
                c.setFillColor(svc["col"])
                c.circle(svc["x"] + 22, y + 5, 3, fill=1, stroke=0)
                c.setFont(FONT, 12)
                c.setFillColor(TEXT)
                c.drawString(svc["x"] + 36, y, item)
                y -= 24

    c.showPage()


def slide_build_system(c, n, total):
    bg(c)
    accent_bar(c, color=ACCENT2)
    title_block(c, "Build System", "Meson + Ninja  —  cross-compilation for two targets")
    slide_number(c, n, total)

    # Left: options
    card(c, 60, 160, 430, 420)
    c.setFont(FONT_B, 16)
    c.setFillColor(ACCENT2)
    c.drawString(80, 562, "Meson Options  (meson_options.txt)")

    options = [
        ("-Dtarget=",         "libcamera  |  v4l2",         "Selects hardware platform"),
        ("-Duse_rknn=",       "true  |  false",             "Enable Rockchip NPU (Luckfox)"),
        ("-Dpi5_sysroot=",    "/path/to/sysroot",           "Pi 5 cross-compile root"),
        ("-Dluckfox_sysroot=","/path/to/sysroot",           "Luckfox cross-compile root"),
    ]
    y = 540
    for flag, val, desc in options:
        c.setFont(FONT_B, 12)
        c.setFillColor(GOLD)
        c.drawString(80, y, flag)
        tw = len(flag) * 7.2
        c.setFont(FONT, 12)
        c.setFillColor(TEXT)
        c.drawString(80 + tw, y, val)
        c.setFont(FONT_O, 10)
        c.setFillColor(MUTED)
        c.drawString(80, y - 14, desc)
        y -= 36

    # CPU flags
    c.setFont(FONT_B, 13)
    c.setFillColor(ACCENT)
    c.drawString(80, 390, "CPU-Specific Flags")
    c.setFont(FONT_B, 11)
    c.setFillColor(ACCENT2)
    c.drawString(80, 370, "aarch64 (Pi 5):")
    c.setFont(FONT, 11)
    c.setFillColor(TEXT)
    c.drawString(80, 352, "-march=armv8.2-a+dotprod+fp16")
    c.setFont(FONT_O, 10)
    c.setFillColor(MUTED)
    c.drawString(80, 336, "ASIMD dot-product, FP16 NCNN kernels")

    c.setFont(FONT_B, 11)
    c.setFillColor(PURPLE)
    c.drawString(80, 314, "armv7 (Luckfox):")
    c.setFont(FONT, 11)
    c.setFillColor(TEXT)
    c.drawString(80, 296, "-march=armv7-a -mfpu=neon-vfpv4")
    c.setFont(FONT_O, 10)
    c.setFillColor(MUTED)
    c.drawString(80, 280, "NEON intrinsics for NCNN inference")

    # Linking strategy
    c.setFont(FONT_B, 13)
    c.setFillColor(ACCENT)
    c.drawString(80, 252, "Linking Strategy")
    static_items = [
        ("Static:", "NCNN, SQLite3, libstdc++, libgcc_s"),
        ("Dynamic:", "libcamera (Pi only), glibc, libgomp1"),
    ]
    y = 232
    for k, v in static_items:
        c.setFont(FONT_B, 12)
        c.setFillColor(GOLD)
        c.drawString(80, y, k)
        c.setFont(FONT, 12)
        c.setFillColor(TEXT)
        c.drawString(160, y, v)
        y -= 22

    c.setFont(FONT_O, 10)
    c.setFillColor(MUTED)
    c.drawString(80, 176, "Static linking = self-contained binary, no dep hell")

    # Right: submodules + deps
    card(c, 520, 160, 700, 420)
    c.setFont(FONT_B, 16)
    c.setFillColor(PURPLE)
    c.drawString(540, 562, "Submodules & Dependencies")

    subs = [
        ("subprojects/ncnn/",     "Tencent/ncnn",           "Neural network inference library — built from source"),
        ("firmware/models/",      "nature-sense/ai-trap-models", "NCNN + RKNN weights (yolo11n-320, 1 class)"),
        ("api/",                  "nature-sense/ai-trap-api",    "OpenAPI 3.1 spec + Flutter Dart client gen"),
    ]
    y = 538
    for path, repo, desc in subs:
        c.setFillColor(PANEL)
        c.roundRect(538, y - 4, 666, 38, 5, fill=1, stroke=0)
        c.setFont(FONT_B, 12)
        c.setFillColor(GOLD)
        c.drawString(548, y + 20, path)
        c.setFont(FONT, 11)
        c.setFillColor(ACCENT2)
        c.drawString(548, y + 6, repo)
        c.setFont(FONT_O, 10)
        c.setFillColor(MUTED)
        c.drawString(548, y - 8, desc)
        y -= 56

    c.setFont(FONT_B, 14)
    c.setFillColor(ACCENT)
    c.drawString(540, 368, "Runtime Prerequisites  (apt)")

    apt_items = [
        ("libcamera0.7",      "Camera framework (Pi OS repo)"),
        ("libcamera-ipa",     "IPA modules (system paths)"),
        ("libsqlite3-0",      "SQLite runtime"),
        ("libgomp1",          "OpenMP for NCNN parallelism"),
    ]
    y = 348
    for pkg, desc in apt_items:
        c.setFillColor(ACCENT)
        c.roundRect(538, y, 120, 18, 4, fill=1, stroke=0)
        c.setFont(FONT_B, 10)
        c.setFillColor(BG)
        c.drawString(543, y + 5, pkg)
        c.setFont(FONT, 11)
        c.setFillColor(TEXT)
        c.drawString(668, y + 4, desc)
        y -= 28

    c.setFont(FONT_B, 14)
    c.setFillColor(GOLD)
    c.drawString(540, 222, "Quick Build  (Pi 5 native)")
    cmds = [
        "meson setup build -Dtarget=libcamera",
        "cd build && ninja",
        "./scripts/make-package.sh",
    ]
    y = 202
    for cmd in cmds:
        card(c, 538, y - 4, 660, 22, fill=colors.HexColor("#0D1117"))
        c.setStrokeColor(ACCENT)
        c.setLineWidth(0.5)
        c.roundRect(538, y - 4, 660, 22, 4, fill=0, stroke=1)
        c.setFont(FONT, 11)
        c.setFillColor(TEAL)
        c.drawString(544, y + 4, "$ " + cmd)
        y -= 30

    c.showPage()


def slide_cicd(c, n, total):
    bg(c)
    accent_bar(c, color=ACCENT)
    title_block(c, "CI / CD  —  GitHub Actions", "Automated cross-compilation for both targets")
    slide_number(c, n, total)

    workflows = [
        {
            "name": "build-pi5.yml",
            "title": "Pi 5  —  aarch64",
            "trigger": "push to main / PR",
            "output": "ai-trap-pi5-{sha}.tar.gz",
            "col": ACCENT2,
            "x": 60,
            "steps": [
                "Install aarch64-linux-gnu GCC toolchain",
                "Fetch libcamera .debs from Pi OS repo",
                "Extract libcamera into cross-sysroot",
                "Build SQLite3 (static) → cached by version",
                "Build NCNN (static) → cached by version",
                "Install BlueZ headers",
                "Generate Meson CI cross-file on-the-fly",
                "meson setup + ninja build",
                "Strip binary, copy config + systemd files",
                "Create tarball artifact (retained 30 days)",
            ]
        },
        {
            "name": "build-luckfox.yml",
            "title": "Luckfox  —  armv7 uclibc",
            "trigger": "push to main / PR / workflow_dispatch",
            "output": "yolo_v4l2 binary",
            "col": PURPLE,
            "x": 660,
            "steps": [
                "Sparse-clone Luckfox SDK → cache by commit SHA",
                "Toolchain: arm-rockchip830-linux-uclibcgnueabihf",
                "Build SQLite3 cross-compiled → cached",
                "Build NCNN cross-compiled → cached",
                "Install BlueZ headers from host",
                "Generate Meson CI cross-file (uclibc target)",
                "meson setup + ninja build",
                "Strip binary",
                "Upload yolo_v4l2 artifact",
                "build-luckfox-ci.sh: trigger + download locally",
            ]
        },
    ]

    for wf in workflows:
        cx = wf["x"]
        card(c, cx, 155, 560, 440)

        c.setFont(FONT_B, 18)
        c.setFillColor(wf["col"])
        c.drawString(cx + 18, 572, wf["title"])

        c.setFont(FONT_B, 11)
        c.setFillColor(MUTED)
        c.drawString(cx + 18, 554, wf["name"])

        pill(c, cx + 18, 530, "Trigger: " + wf["trigger"], bg_col=PANEL, text_col=MUTED)
        pill(c, cx + 18, 508, "Output: " + wf["output"], bg_col=PANEL, text_col=ACCENT)

        c.setStrokeColor(BORDER)
        c.setLineWidth(0.5)
        c.line(cx + 18, 500, cx + 542, 500)

        y = 480
        for i, step in enumerate(wf["steps"]):
            c.setFillColor(wf["col"])
            c.roundRect(cx + 18, y + 2, 18, 14, 3, fill=1, stroke=0)
            c.setFont(FONT_B, 9)
            c.setFillColor(BG)
            c.drawCentredString(cx + 27, y + 5, str(i+1))
            c.setFont(FONT, 12)
            c.setFillColor(TEXT)
            c.drawString(cx + 44, y + 3, step)
            y -= 22

    # Cache note
    card(c, 60, 110, 1160, 35, fill=PANEL)
    c.setFont(FONT_B, 12)
    c.setFillColor(GOLD)
    c.drawString(80, 122, "Cache strategy:")
    c.setFont(FONT, 12)
    c.setFillColor(TEXT)
    c.drawString(200, 122, "Toolchain keyed on SDK commit SHA  •  Sysroot keyed on NCNN + SQLite versions  •  Cache hit: ~5 min vs ~30 min cold")

    c.showPage()


def slide_deployment(c, n, total):
    bg(c)
    accent_bar(c, color=PURPLE)
    title_block(c, "Deployment & Installation", "Package structure, install.sh steps, systemd services")
    slide_number(c, n, total)

    # Package tree
    card(c, 60, 155, 440, 430)
    c.setFont(FONT_B, 15)
    c.setFillColor(ACCENT2)
    c.drawString(80, 568, "Package Structure")
    tree = [
        ("package/",                       0, MUTED),
        ("├── bin/yolo_libcamera",         1, TEAL),
        ("├── models/yolo11n-320/",        1, TEXT),
        ("│   ├── model.ncnn.param",       2, MUTED),
        ("│   └── model.ncnn.bin",         2, MUTED),
        ("├── config/trap_config.toml",    1, GOLD),
        ("└── systemd/",                   1, TEXT),
        ("    ├── ai-trap.service",        2, ACCENT),
        ("    └── camera-overlay.service", 2, ACCENT),
    ]
    y = 548
    for label, indent, col in tree:
        c.setFont(FONT, 12)
        c.setFillColor(col)
        c.drawString(80 + indent * 18, y, label)
        y -= 22

    # install.sh steps
    c.setFont(FONT_B, 14)
    c.setFillColor(ACCENT)
    c.drawString(80, 400, "install.sh  (305 loc)  steps:")
    steps = [
        "apt install libcamera0.7, libcamera-ipa, libsqlite3-0, libgomp1",
        "Create ai-trap system user  (video group for camera)",
        "Create /opt/ai-trap/{bin,lib,models,crops,data,logs}",
        "Copy binary → /opt/ai-trap/bin/",
        "Copy model files → /opt/ai-trap/models/",
        "Set camera_auto_detect=0 in /boot/firmware/config.txt",
        "Remove stale dtoverlay=imx708 lines  (fixes reboot loop)",
        "Enable persistent journal",
        "systemctl enable + start camera-overlay  ai-trap",
    ]
    y = 380
    for i, step in enumerate(steps):
        c.setFillColor(ACCENT)
        c.roundRect(78, y + 1, 18, 14, 3, fill=1, stroke=0)
        c.setFont(FONT_B, 9)
        c.setFillColor(BG)
        c.drawCentredString(87, y + 4, str(i+1))
        c.setFont(FONT, 11)
        c.setFillColor(TEXT)
        c.drawString(104, y + 2, step)
        y -= 22

    # Services
    card(c, 530, 155, 700, 430)
    c.setFont(FONT_B, 15)
    c.setFillColor(PURPLE)
    c.drawString(550, 568, "Systemd Services")

    # ai-trap.service
    card(c, 548, 360, 664, 188, fill=colors.HexColor("#0D1117"))
    c.setStrokeColor(ACCENT)
    c.setLineWidth(0.5)
    c.roundRect(548, 360, 664, 188, 6, fill=0, stroke=1)
    c.setFont(FONT_B, 13)
    c.setFillColor(ACCENT)
    c.drawString(566, 532, "ai-trap.service")
    c.setFont(FONT_O, 10)
    c.setFillColor(MUTED)
    c.drawString(566, 516, "Main detection + server process")
    details = [
        "Type: simple  |  User: ai-trap  |  Group: video",
        "Wants / After: camera-overlay.service",
        "ExecStartPre: wait /dev/media0  +  sleep 3s",
        "Restart: on-failure  |  RestartSec: 15s",
        "StartLimitIntervalSec in [Unit]  (not [Service])",
        "Environment: LIBCAMERA_IPA_MODULE_PATH, XDG_RUNTIME_DIR",
    ]
    y = 498
    for d in details:
        c.setFont(FONT, 11)
        c.setFillColor(TEXT)
        c.drawString(566, y, d)
        y -= 20

    # camera-overlay.service
    card(c, 548, 168, 664, 170, fill=colors.HexColor("#0D1117"))
    c.setStrokeColor(PURPLE)
    c.setLineWidth(0.5)
    c.roundRect(548, 168, 664, 170, 6, fill=0, stroke=1)
    c.setFont(FONT_B, 13)
    c.setFillColor(PURPLE)
    c.drawString(566, 322, "camera-overlay.service")
    c.setFont(FONT_O, 10)
    c.setFillColor(MUTED)
    c.drawString(566, 306, "Load IMX708 overlay at runtime — avoids CM5 boot power spike")
    details2 = [
        "Type: oneshot  (RemainAfterExit=yes)",
        "After: network-online.target  (power rails stable by then)",
        "ExecStart: /usr/bin/dtoverlay imx708",
        "Do NOT add dtoverlay=imx708 to config.txt",
    ]
    y = 288
    for d in details2:
        c.setFont(FONT, 11)
        c.setFillColor(TEXT)
        c.drawString(566, y, d)
        y -= 22

    c.showPage()


def slide_config(c, n, total):
    bg(c)
    accent_bar(c, color=GOLD)
    title_block(c, "Runtime Configuration", "trap_config.toml  —  all settings in one file")
    slide_number(c, n, total)

    sections = [
        ("[trap]",       ACCENT2, ["id, location_name, gps lat/lon/alt"]),
        ("[model]",      ACCENT,  ["ncnn/rknn model paths", "input_width/height: 320"]),
        ("[detection]",  RED,     ["conf_threshold: 0.45", "nms_threshold", "box size constraints"]),
        ("[tracker]",    PURPLE,  ["min_hits: 3  max_missed: 30", "high/low conf thresholds"]),
        ("[camera]",     ACCENT2, ["width/height (1536x864)", "framerate: 10", "buffer_count: 2"]),
        ("[autofocus]",  TEAL,    ["mode: manual/auto/continuous", "range: normal/macro/full"]),
        ("[crops]",      GOLD,    ["output_dir, jpeg_quality: 90", "max_saves_per_track: 3"]),
        ("[stream]",     PURPLE,  ["port: 9000, width: 640, height: 480", "quality: 75"]),
        ("[sse]",        ACCENT,  ["port: 8081, max_clients: 8", "queue_depth"]),
        ("[api]",        ACCENT2, ["port: 8080"]),
        ("[database]",   GOLD,    ["sqlite_path: /opt/ai-trap/data/detections.db"]),
        ("[wifi]",       TEAL,    ["managed: true/false", "ap_ssid, ap_password", "interface"]),
        ("[display]",    MUTED,   ["enabled: true/false", "spi_device, gpio pins"]),
    ]

    cols = 3
    col_w = (W - 120) // cols
    x0, y0 = 60, 500
    row_h = 90

    for i, (name, col, items) in enumerate(sections):
        col_idx = i % cols
        row_idx = i // cols
        bx = x0 + col_idx * col_w
        by = y0 - row_idx * row_h
        bw = col_w - 16
        bh = row_h - 10

        card(c, bx, by, bw, bh)
        c.setFont(FONT_B, 13)
        c.setFillColor(col)
        c.drawString(bx + 12, by + bh - 20, name)
        y = by + bh - 38
        for item in items:
            c.setFont(FONT, 11)
            c.setFillColor(TEXT)
            c.drawString(bx + 12, y, item)
            y -= 16

    # Footer note
    card(c, 60, 110, 1160, 38, fill=PANEL)
    c.setFont(FONT_B, 11)
    c.setFillColor(GOLD)
    c.drawString(80, 123, "Thermal tip:")
    c.setFont(FONT, 11)
    c.setFillColor(TEXT)
    c.drawString(170, 123, "On Waveshare Nano Base Board A — set framerate=10 and buffer_count=2 to prevent thermal hard-resets during sustained inference.")

    c.showPage()


def slide_known_issues(c, n, total):
    bg(c)
    accent_bar(c, color=RED)
    title_block(c, "Known Issues & Fixes", "Hardware quirks on Raspberry Pi CM5")
    slide_number(c, n, total)

    issues = [
        {
            "title": "CM5 Reboot Loop — Static dtoverlay",
            "sev":   "CRITICAL",
            "col":   RED,
            "x": 60, "y": 390, "w": 545, "h": 190,
            "items": [
                "Symptom: board reboots immediately after fresh install",
                "Root cause: dtoverlay=imx708 in config.txt causes boot power spike",
                "Fix: do NOT add dtoverlay to config.txt",
                "Use camera-overlay.service (After=network-online.target)",
                "install.sh actively removes stale dtoverlay= lines",
            ]
        },
        {
            "title": "Thermal Crash — Waveshare Nano",
            "sev":   "HIGH",
            "col":   GOLD,
            "x": 635, "y": 390, "w": 545, "h": 190,
            "items": [
                "Symptom: hard-reset after ~5 min sustained inference",
                "Root cause: passive heatsink insufficient under camera + YOLO load",
                "Fix: framerate=10, buffer_count=2 in [camera] section",
                "Optional: arm_freq=1500 in config.txt to cap CPU MHz",
                "trap006 (IO-BASE-A) is not affected — better thermal mass",
            ]
        },
        {
            "title": "Kernel Panic — Bundled IPA Modules",
            "sev":   "HIGH",
            "col":   PURPLE,
            "x": 60, "y": 185, "w": 545, "h": 180,
            "items": [
                "Bundled libcamera IPA modules cause kernel panic on CM5",
                "Fix: set LIBCAMERA_IPA_MODULE_PATH to system paths in service",
                "Requires libcamera0.7 and libcamera-ipa from Pi apt repo",
                "install.sh installs these packages automatically",
            ]
        },
        {
            "title": "StartLimitIntervalSec Placement",
            "sev":   "LOW",
            "col":   MUTED,
            "x": 635, "y": 185, "w": 545, "h": 180,
            "items": [
                "Must be in [Unit] section, not [Service]",
                "systemd on Trixie ignores it in [Service]",
                "If misplaced: default 5 attempts / 10s applies",
                "Service stops retrying after limit exceeded",
            ]
        },
    ]

    for issue in issues:
        card(c, issue["x"], issue["y"], issue["w"], issue["h"])

        sev_col = RED if issue["sev"] == "CRITICAL" else (GOLD if issue["sev"] == "HIGH" else MUTED)
        pill(c, issue["x"] + issue["w"] - 90, issue["y"] + issue["h"] - 24,
             issue["sev"], bg_col=sev_col, text_col=BG, font_size=9)

        c.setFont(FONT_B, 14)
        c.setFillColor(issue["col"])
        c.drawString(issue["x"] + 16, issue["y"] + issue["h"] - 22, issue["title"])

        c.setStrokeColor(BORDER)
        c.setLineWidth(0.5)
        c.line(issue["x"] + 16, issue["y"] + issue["h"] - 30,
               issue["x"] + issue["w"] - 16, issue["y"] + issue["h"] - 30)

        y = issue["y"] + issue["h"] - 52
        for item in issue["items"]:
            c.setFillColor(issue["col"])
            c.circle(issue["x"] + 22, y + 5, 3, fill=1, stroke=0)
            c.setFont(FONT, 11)
            c.setFillColor(TEXT)
            c.drawString(issue["x"] + 36, y, item)
            y -= 24

    c.showPage()


def slide_summary(c, n, total):
    bg(c)
    accent_bar(c, color=ACCENT)
    slide_number(c, n, total)

    # Large title
    c.setFont(FONT_B, 46)
    c.setFillColor(TEXT)
    c.drawCentredString(W/2, H - 130, "ai-trap-linux")

    c.setFont(FONT, 20)
    c.setFillColor(MUTED)
    c.drawCentredString(W/2, H - 170, "Real-time embedded insect detection at the edge")

    c.setStrokeColor(BORDER)
    c.setLineWidth(1)
    c.line(200, H - 188, W - 200, H - 188)

    # Key stats
    stats = [
        ("2", "Hardware\nTargets"),
        ("6600+", "Lines of\nC++"),
        ("3", "Git\nSubmodules"),
        ("3", "Output\nServers"),
        ("1", "YOLO Model\nClass"),
        ("2", "CI/CD\nWorkflows"),
    ]

    bw, bh = 170, 100
    gap = 18
    total_sw = len(stats) * bw + (len(stats) - 1) * gap
    sx = (W - total_sw) / 2
    sy = H - 340

    for num, label in stats:
        card(c, sx, sy, bw, bh, fill=PANEL)
        c.setFont(FONT_B, 30)
        c.setFillColor(ACCENT)
        c.drawCentredString(sx + bw/2, sy + bh - 40, num)
        lines = label.split("\n")
        ly = sy + 30
        for line in lines:
            c.setFont(FONT, 11)
            c.setFillColor(MUTED)
            c.drawCentredString(sx + bw/2, ly, line)
            ly -= 16
        sx += bw + gap

    # Component quick-ref
    cols_data = [
        ("Capture", [
            "libcamera_capture  (Pi 5)",
            "v4l2_capture  (Luckfox)",
            "rknn_infer  (NPU, optional)",
        ]),
        ("Pipeline", [
            "decoder  (YOLO decode + NMS)",
            "tracker  (ByteTracker)",
            "crop_saver  (async JPEG)",
            "persistence  (SQLite3)",
            "sync_manager  (cloud)",
            "exif_writer  (GPS EXIF)",
        ]),
        ("Servers", [
            "http_server  :8080",
            "sse_server   :8081",
            "mjpeg_streamer :9000",
        ]),
        ("Common", [
            "wifi_manager",
            "ble_gatt_server",
            "epaper_display",
            "config_loader",
        ]),
    ]

    cx = 60
    for col_title, col_items in cols_data:
        cw = (W - 120) // len(cols_data) - 10
        card(c, cx, 145, cw, 170, fill=PANEL)
        c.setFont(FONT_B, 13)
        c.setFillColor(ACCENT)
        c.drawString(cx + 12, 298, col_title)
        c.setStrokeColor(BORDER)
        c.setLineWidth(0.5)
        c.line(cx + 12, 290, cx + cw - 12, 290)
        y = 272
        for item in col_items:
            c.setFont(FONT, 11)
            c.setFillColor(TEXT)
            c.drawString(cx + 12, y, item)
            y -= 22
        cx += cw + 16

    # Footer
    c.setFont(FONT, 12)
    c.setFillColor(MUTED)
    c.drawCentredString(W/2, 22, "NatureSense  •  ai-trap-linux  •  2026  •  github.com/nature-sense/ai-trap-linux")

    c.showPage()


# =============================================================================
# MAIN
# =============================================================================

def main():
    out = "/Users/steve/naturesense/ai_trap/ai-trap-linux/ai-trap-architecture.pdf"
    c = canvas.Canvas(out, pagesize=(W, H))
    c.setTitle("ai-trap-linux — Architecture & Component Overview")
    c.setAuthor("NatureSense")
    c.setSubject("Embedded AI Insect Detection System")

    slides = [
        slide_title,
        slide_hardware,
        slide_system_architecture,
        slide_detection_pipeline,
        slide_capture,
        slide_inference_tracking,
        slide_storage_sync,
        slide_servers,
        slide_common_services,
        slide_build_system,
        slide_cicd,
        slide_deployment,
        slide_config,
        slide_known_issues,
        slide_summary,
    ]
    total = len(slides)
    for i, slide_fn in enumerate(slides, 1):
        slide_fn(c, i, total)

    c.save()
    print(f"Saved: {out}  ({total} slides)")

if __name__ == "__main__":
    main()
