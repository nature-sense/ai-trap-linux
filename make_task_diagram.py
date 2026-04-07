"""
make_task_diagram.py — FreeRTOS task and queue diagram for ai-trap ESP32-P4
Generates: docs/esp32-p4-task-diagram.pdf
"""

from reportlab.pdfgen import canvas
from reportlab.lib import colors
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics

# ── Page ──────────────────────────────────────────────────────────────────────
W, H = 1100, 720

# ── Palette ───────────────────────────────────────────────────────────────────
WHITE       = colors.white
BLACK       = colors.black
CORE0_BG    = colors.HexColor("#EAF4FB")   # soft blue
CORE0_BD    = colors.HexColor("#1F6FEB")   # blue border
CORE1_BG    = colors.HexColor("#EAFBEA")   # soft green
CORE1_BD    = colors.HexColor("#238636")   # green border
LP_BG       = colors.HexColor("#F5EAFB")   # soft purple
LP_BD       = colors.HexColor("#8957E5")   # purple border
Q_BG        = colors.HexColor("#FFF8E1")   # amber fill
Q_BD        = colors.HexColor("#D29922")   # amber border
EXT_BG      = colors.HexColor("#F0F0F0")   # external grey
EXT_BD      = colors.HexColor("#8B949E")
ISR_BG      = colors.HexColor("#FFF3CD")
RED         = colors.HexColor("#DA3633")
MUTED       = colors.HexColor("#57606A")
DARK        = colors.HexColor("#1C2128")

FONT        = "Helvetica"
FONT_B      = "Helvetica-Bold"
FONT_O      = "Helvetica-Oblique"


# ── Drawing helpers ───────────────────────────────────────────────────────────

def rounded_rect(c, x, y, w, h, r=8, fill=WHITE, stroke=BLACK, lw=1.5):
    c.setFillColor(fill)
    c.setStrokeColor(stroke)
    c.setLineWidth(lw)
    c.roundRect(x, y, w, h, r, fill=1, stroke=1)


def label(c, x, y, text, font=FONT, size=11, color=DARK, align="left"):
    c.setFont(font, size)
    c.setFillColor(color)
    if align == "center":
        c.drawCentredString(x, y, text)
    elif align == "right":
        c.drawRightString(x, y, text)
    else:
        c.drawString(x, y, text)


def pill(c, x, y, text, bg=CORE0_BG, border=CORE0_BD, font_size=9):
    tw = len(text) * 6.2 + 12
    c.setFillColor(bg)
    c.setStrokeColor(border)
    c.setLineWidth(1)
    c.roundRect(x, y, tw, 16, 4, fill=1, stroke=1)
    c.setFont(FONT_B, font_size)
    c.setFillColor(border)
    c.drawCentredString(x + tw / 2, y + 4, text)
    return tw


def arrow(c, x1, y1, x2, y2, color=MUTED, lw=2.0, dash=None):
    c.setStrokeColor(color)
    c.setLineWidth(lw)
    if dash:
        c.setDash(*dash)
    else:
        c.setDash()
    c.line(x1, y1, x2, y2)
    c.setDash()
    # Arrowhead at (x2, y2)
    import math
    angle = math.atan2(y2 - y1, x2 - x1)
    asize = 9
    c.setFillColor(color)
    p = c.beginPath()
    p.moveTo(x2, y2)
    p.lineTo(x2 - asize * math.cos(angle - 0.4), y2 - asize * math.sin(angle - 0.4))
    p.lineTo(x2 - asize * math.cos(angle + 0.4), y2 - asize * math.sin(angle + 0.4))
    p.close()
    c.drawPath(p, fill=1, stroke=0)


def horiz_arrow(c, x1, y, x2, col=MUTED, lw=2.0):
    arrow(c, x1, y, x2, y, color=col, lw=lw)


def vert_arrow(c, x, y1, y2, col=MUTED, lw=2.0, dash=None):
    arrow(c, x, y1, x, y2, color=col, lw=lw, dash=dash)


def queue_box(c, cx, cy, w, h, name, item_type, depth, policy, direction="right"):
    """Draw a queue as a labelled box with depth 'slots'."""
    x = cx - w / 2
    y = cy - h / 2
    rounded_rect(c, x, y, w, h, r=6, fill=Q_BG, stroke=Q_BD, lw=1.5)

    # Queue name
    c.setFont(FONT_B, 11)
    c.setFillColor(colors.HexColor("#6E4F00"))
    c.drawCentredString(cx, y + h - 16, name)

    # Item type
    c.setFont(FONT_O, 9)
    c.setFillColor(MUTED)
    c.drawCentredString(cx, y + h - 30, item_type)

    # Depth slots drawn as small rectangles
    slot_w = min(14, (w - 20) / depth)
    slot_h = 12
    total_slots_w = depth * slot_w + (depth - 1) * 2
    sx = cx - total_slots_w / 2
    sy = y + h / 2 - 8
    for i in range(depth):
        c.setFillColor(colors.HexColor("#FDEFC3"))
        c.setStrokeColor(Q_BD)
        c.setLineWidth(0.8)
        c.rect(sx + i * (slot_w + 2), sy, slot_w, slot_h, fill=1, stroke=1)

    label_depth = f"depth = {depth}"
    c.setFont(FONT, 9)
    c.setFillColor(MUTED)
    c.drawCentredString(cx, sy - 2, label_depth)

    # Full policy
    c.setFont(FONT_B, 8.5)
    col = RED if "drop" in policy.lower() else colors.HexColor("#238636")
    c.setFillColor(col)
    c.drawCentredString(cx, y + 7, policy)


def task_box(c, x, y, w, h, name, pri, core_color, core_bd, detail_lines=None,
             header_extra=None):
    """Draw a task box with priority badge and detail lines."""
    rounded_rect(c, x, y, w, h, r=10, fill=core_color, stroke=core_bd, lw=2)

    # Task name
    c.setFont(FONT_B, 14)
    c.setFillColor(core_bd)
    c.drawCentredString(x + w / 2, y + h - 22, name)

    # Priority pill
    pw = pill(c, x + w / 2 - 35, y + h - 42, f"pri = {pri}",
              bg=core_color, border=core_bd, font_size=9)

    if header_extra:
        c.setFont(FONT_O, 9)
        c.setFillColor(MUTED)
        c.drawCentredString(x + w / 2, y + h - 56, header_extra)

    # Separator
    c.setStrokeColor(core_bd)
    c.setLineWidth(0.5)
    c.setDash(3, 2)
    offset = 60 if header_extra else 48
    c.line(x + 10, y + h - offset, x + w - 10, y + h - offset)
    c.setDash()

    # Detail lines
    if detail_lines:
        dy = y + h - (offset + 16)
        for line in detail_lines:
            c.setFont(FONT, 10)
            c.setFillColor(DARK)
            c.drawCentredString(x + w / 2, dy, line)
            dy -= 15

    return x + w / 2  # centre x


# ── MAIN ──────────────────────────────────────────────────────────────────────

def draw(c):

    # ── Background ───────────────────────────────────────────────────────────
    c.setFillColor(WHITE)
    c.rect(0, 0, W, H, fill=1, stroke=0)

    # ── Title ────────────────────────────────────────────────────────────────
    label(c, W / 2, H - 28, "ai-trap ESP32-P4 — FreeRTOS Tasks & Queues",
          font=FONT_B, size=17, color=DARK, align="center")
    label(c, W / 2, H - 46, "firmware-esp32p4/  ·  ESP-IDF 5.3  ·  C++17",
          font=FONT_O, size=10, color=MUTED, align="center")

    # ─────────────────────────────────────────────────────────────────────────
    # Core regions (background shading + label)
    # Core 0 spans CameraTask + RadioTask + PowerTask
    # Core 1 spans InferenceTask
    # ─────────────────────────────────────────────────────────────────────────

    # Core 0 region — wide background encompassing left + right tasks
    CORE0_TOP  = 570
    CORE0_BOT  = 195
    C0_LEFT    = 18
    C0_RIGHT   = W - 18

    c.setFillColor(colors.HexColor("#F0F6FF"))
    c.setStrokeColor(CORE0_BD)
    c.setLineWidth(1.2)
    c.setDash(6, 3)
    c.roundRect(C0_LEFT, CORE0_BOT, C0_RIGHT - C0_LEFT, CORE0_TOP - CORE0_BOT, 12, fill=1, stroke=1)
    c.setDash()
    label(c, C0_LEFT + 12, CORE0_TOP - 16,
          "Core 0  (PRO_CPU)", font=FONT_B, size=10, color=CORE0_BD)

    # Core 1 region — centred island
    C1_X, C1_Y, C1_W, C1_H = 418, 255, 264, 295
    c.setFillColor(colors.HexColor("#F0FBF0"))
    c.setStrokeColor(CORE1_BD)
    c.setLineWidth(1.5)
    c.setDash(6, 3)
    c.roundRect(C1_X, C1_Y, C1_W, C1_H, 12, fill=1, stroke=1)
    c.setDash()
    label(c, C1_X + 12, C1_Y + C1_H - 16,
          "Core 1  (APP_CPU)", font=FONT_B, size=10, color=CORE1_BD)

    # ─────────────────────────────────────────────────────────────────────────
    # External: OV5647 (top-left) and WiFi Server (top-right)
    # ─────────────────────────────────────────────────────────────────────────

    # OV5647
    OV_X, OV_Y, OV_W, OV_H = 40, 614, 200, 56
    rounded_rect(c, OV_X, OV_Y, OV_W, OV_H, r=6, fill=EXT_BG, stroke=EXT_BD, lw=1.5)
    label(c, OV_X + OV_W / 2, OV_Y + 34, "OV5647", font=FONT_B, size=13,
          color=MUTED, align="center")
    label(c, OV_X + OV_W / 2, OV_Y + 18, "640×480 RAW10  MIPI CSI-2 1-lane",
          font=FONT, size=9, color=MUTED, align="center")
    label(c, OV_X + OV_W / 2, OV_Y + 6, "I²C/SCCB config (ov5647.cpp)",
          font=FONT_O, size=8, color=MUTED, align="center")

    # WiFi Server / NVS
    SRV_W, SRV_H = 190, 56
    SRV_X = W - SRV_W - 40
    SRV_Y = 614
    rounded_rect(c, SRV_X, SRV_Y, SRV_W, SRV_H, r=6, fill=EXT_BG, stroke=EXT_BD, lw=1.5)
    label(c, SRV_X + SRV_W / 2, SRV_Y + 34, "WiFi Server / NVS buffer",
          font=FONT_B, size=12, color=MUTED, align="center")
    label(c, SRV_X + SRV_W / 2, SRV_Y + 18, "HTTP POST  ·  /api/detections",
          font=FONT, size=9, color=MUTED, align="center")
    label(c, SRV_X + SRV_W / 2, SRV_Y + 6, "NVS ring (256 events) on disconnect",
          font=FONT_O, size=8, color=MUTED, align="center")

    # ─────────────────────────────────────────────────────────────────────────
    # CameraTask (Core 0, left)
    # ─────────────────────────────────────────────────────────────────────────

    CAM_X, CAM_Y, CAM_W, CAM_H = 38, 270, 220, 290

    task_box(c, CAM_X, CAM_Y, CAM_W, CAM_H,
             "CameraTask", 6, CORE0_BG, CORE0_BD,
             header_extra="Core 0  ·  stack 6 KB",
             detail_lines=[
                 "OV5647 SCCB init",
                 "CSI controller setup",
                 "ISP: RAW10 → 224×224 YUV420",
                 "2× DMA frame buffers",
                 "frame pool ping-pong",
             ])

    # ISR sub-box inside CameraTask
    ISR_X = CAM_X + 12
    ISR_Y = CAM_Y + 14
    ISR_W = CAM_W - 24
    ISR_H = 52
    rounded_rect(c, ISR_X, ISR_Y, ISR_W, ISR_H, r=5,
                 fill=ISR_BG, stroke=Q_BD, lw=1)
    label(c, ISR_X + ISR_W / 2, ISR_Y + 32, "on_frame_ready()  ISR",
          font=FONT_B, size=9, color=colors.HexColor("#6E4F00"), align="center")
    label(c, ISR_X + ISR_W / 2, ISR_Y + 18, "runs in interrupt context",
          font=FONT_O, size=8.5, color=MUTED, align="center")
    label(c, ISR_X + ISR_W / 2, ISR_Y + 6, "never blocks · no PSRAM access",
          font=FONT_O, size=8.5, color=MUTED, align="center")

    CAM_CX = CAM_X + CAM_W / 2
    CAM_TOP = CAM_Y + CAM_H
    CAM_BOT = CAM_Y

    # ─────────────────────────────────────────────────────────────────────────
    # InferenceTask (Core 1, centre)
    # ─────────────────────────────────────────────────────────────────────────

    INF_X, INF_Y, INF_W, INF_H = 430, 270, 240, 270
    task_box(c, INF_X, INF_Y, INF_W, INF_H,
             "InferenceTask", 5, CORE1_BG, CORE1_BD,
             header_extra="Core 1  ·  stack 12 KB",
             detail_lines=[
                 "PPA: YUV420 → RGB888",
                 "return buf to pool early",
                 "ESPDet-Pico INT8",
                 "RUNTIME_MODE_MULTI_CORE",
                 "conf threshold filter",
                 "emit DetectionEvent",
             ])

    INF_CX  = INF_X + INF_W / 2
    INF_TOP = INF_Y + INF_H
    INF_BOT = INF_Y

    # ─────────────────────────────────────────────────────────────────────────
    # RadioTask (Core 0, right)
    # ─────────────────────────────────────────────────────────────────────────

    RAD_X, RAD_Y, RAD_W, RAD_H = 840, 270, 220, 290
    task_box(c, RAD_X, RAD_Y, RAD_W, RAD_H,
             "RadioTask", 3, CORE0_BG, CORE0_BD,
             header_extra="Core 0  ·  stack 8 KB",
             detail_lines=[
                 "WiFi STA (auto-reconnect)",
                 "HTTP POST JSON event",
                 "NVS ring buffer retry",
                 "LoRa SX1262 (Phase 3)",
                 "coalesce burst events",
             ])

    RAD_CX  = RAD_X + RAD_W / 2
    RAD_TOP = RAD_Y + RAD_H

    # ─────────────────────────────────────────────────────────────────────────
    # Queues between CameraTask ↔ InferenceTask
    # ─────────────────────────────────────────────────────────────────────────

    Q_MID_X = (CAM_X + CAM_W + INF_X) / 2   # midpoint between the two task boxes
    Q_CX    = Q_MID_X

    # g_frame_queue (Camera ISR → InferenceTask) — top
    FQ_CY = CAM_Y + CAM_H - 70
    FQ_W, FQ_H = 170, 90
    queue_box(c, Q_CX, FQ_CY, FQ_W, FQ_H,
              "g_frame_queue", "FrameBuffer*", 2, "drop frame")

    # g_buffer_pool_queue (InferenceTask → CameraTask) — bottom
    BQ_CY = CAM_Y + 80
    BQ_W, BQ_H = 170, 90
    queue_box(c, Q_CX, BQ_CY, BQ_W, BQ_H,
              "g_buffer_pool_queue", "FrameBuffer*", 2, "never full")

    # ─────────────────────────────────────────────────────────────────────────
    # g_detection_queue (InferenceTask → RadioTask)
    # ─────────────────────────────────────────────────────────────────────────

    DQ_MID_X = (INF_X + INF_W + RAD_X) / 2
    DQ_CY    = INF_Y + INF_H / 2
    DQ_W, DQ_H = 160, 90
    queue_box(c, DQ_MID_X, DQ_CY, DQ_W, DQ_H,
              "g_detection_queue", "DetectionEvent", 16, "drop oldest")

    # ─────────────────────────────────────────────────────────────────────────
    # Arrows — camera ↔ queues ↔ inference
    # ─────────────────────────────────────────────────────────────────────────

    # ISR → g_frame_queue (right arrow from ISR box)
    isr_right_x  = ISR_X + ISR_W
    isr_mid_y    = ISR_Y + ISR_H / 2
    fq_left_x    = Q_CX - FQ_W / 2
    fq_mid_y     = FQ_CY
    # Route: right from ISR, then curve up to queue
    # Use two-segment: right then up-right
    horiz_arrow(c, isr_right_x, isr_mid_y, CAM_X + CAM_W,
                col=CORE0_BD, lw=2)
    # from right edge of CameraTask up to frame_queue level
    arrow(c, CAM_X + CAM_W, isr_mid_y,
             fq_left_x, fq_mid_y, color=CORE0_BD, lw=2)

    label(c, (CAM_X + CAM_W + fq_left_x) / 2, fq_mid_y + 6,
          "FrameBuffer*", font=FONT_O, size=8, color=CORE0_BD, align="center")

    # g_frame_queue → InferenceTask
    fq_right_x = Q_CX + FQ_W / 2
    inf_left_y  = fq_mid_y
    horiz_arrow(c, fq_right_x, fq_mid_y, INF_X,
                col=CORE1_BD, lw=2)
    label(c, (fq_right_x + INF_X) / 2, fq_mid_y + 6,
          "pop & process", font=FONT_O, size=8, color=CORE1_BD, align="center")

    # InferenceTask → g_buffer_pool_queue (return after PPA)
    bq_right_x = Q_CX + BQ_W / 2
    bq_mid_y   = BQ_CY
    # from left edge of InferenceTask down to buffer pool queue
    arrow(c, INF_X, INF_Y + 50,
             bq_right_x, bq_mid_y, color=CORE1_BD, lw=2)
    label(c, (INF_X + bq_right_x) / 2 + 10, bq_mid_y + 24,
          "return after PPA", font=FONT_O, size=8, color=CORE1_BD, align="center")

    # g_buffer_pool_queue → CameraTask
    bq_left_x = Q_CX - BQ_W / 2
    cam_right_y = bq_mid_y
    horiz_arrow(c, bq_left_x, bq_mid_y, CAM_X + CAM_W,
                col=CORE0_BD, lw=2)
    label(c, (bq_left_x + CAM_X + CAM_W) / 2, bq_mid_y + 6,
          "free buffer", font=FONT_O, size=8, color=CORE0_BD, align="center")

    # InferenceTask → g_detection_queue
    dq_left_x = DQ_MID_X - DQ_W / 2
    dq_mid_y  = DQ_CY
    horiz_arrow(c, INF_X + INF_W, dq_mid_y, dq_left_x,
                col=CORE1_BD, lw=2)
    label(c, (INF_X + INF_W + dq_left_x) / 2, dq_mid_y + 6,
          "DetectionEvent", font=FONT_O, size=8, color=CORE1_BD, align="center")

    # g_detection_queue → RadioTask
    dq_right_x = DQ_MID_X + DQ_W / 2
    horiz_arrow(c, dq_right_x, dq_mid_y, RAD_X,
                col=CORE0_BD, lw=2)
    label(c, (dq_right_x + RAD_X) / 2, dq_mid_y + 6,
          "consume & POST", font=FONT_O, size=8, color=CORE0_BD, align="center")

    # OV5647 → CameraTask (top)
    ov_mid_x = OV_X + OV_W / 2
    vert_arrow(c, ov_mid_x, OV_Y, CAM_TOP, col=EXT_BD, lw=2)
    label(c, ov_mid_x + 4, (OV_Y + CAM_TOP) / 2 + 4,
          "MIPI CSI-2", font=FONT_O, size=8, color=MUTED)

    # RadioTask → Server (top)
    srv_mid_x = SRV_X + SRV_W / 2
    vert_arrow(c, srv_mid_x, RAD_TOP, SRV_Y, col=EXT_BD, lw=2)
    label(c, srv_mid_x + 4, (RAD_TOP + SRV_Y) / 2 + 4,
          "HTTP POST", font=FONT_O, size=8, color=MUTED)

    # ─────────────────────────────────────────────────────────────────────────
    # PowerTask (Core 0, bottom centre)
    # ─────────────────────────────────────────────────────────────────────────

    PT_W, PT_H = 500, 100
    PT_X = W / 2 - PT_W / 2
    PT_Y = 105
    rounded_rect(c, PT_X, PT_Y, PT_W, PT_H, r=10,
                 fill=CORE0_BG, stroke=CORE0_BD, lw=2)
    label(c, PT_X + PT_W / 2, PT_Y + PT_H - 20,
          "PowerTask", font=FONT_B, size=14, color=CORE0_BD, align="center")
    pw = pill(c, PT_X + PT_W / 2 - 30, PT_Y + PT_H - 38,
              "pri = 1", bg=CORE0_BG, border=CORE0_BD)
    label(c, PT_X + PT_W / 2 + pw / 2 + 6, PT_Y + PT_H - 36,
          "Core 0  ·  stack 4 KB", font=FONT_O, size=9, color=MUTED)

    c.setStrokeColor(CORE0_BD)
    c.setLineWidth(0.5)
    c.setDash(3, 2)
    c.line(PT_X + 14, PT_Y + PT_H - 46, PT_X + PT_W - 14, PT_Y + PT_H - 46)
    c.setDash()

    pt_details = [
        "idle timer · no detection for idle_timeout_s  →  orderly shutdown",
        "drain g_frame_queue + g_detection_queue  →  load LP ULP  →  deep sleep",
    ]
    dy = PT_Y + PT_H - 62
    for line in pt_details:
        label(c, PT_X + PT_W / 2, dy, line, font=FONT, size=9.5,
              color=DARK, align="center")
        dy -= 16

    PT_CX = PT_X + PT_W / 2
    PT_TOP = PT_Y + PT_H

    # Dashed monitoring arrows from PowerTask to the three queues
    # → g_frame_queue
    arrow(c, PT_CX - 90, PT_TOP,
             Q_CX, BQ_CY - BQ_H / 2,
             color=CORE0_BD, lw=1.2, dash=[4, 3])
    # → g_detection_queue
    arrow(c, PT_CX + 90, PT_TOP,
             DQ_MID_X, DQ_CY - DQ_H / 2,
             color=CORE0_BD, lw=1.2, dash=[4, 3])
    # → CameraTask (stop signal)
    arrow(c, PT_X, PT_Y + PT_H / 2,
             CAM_X + CAM_W / 2, CAM_BOT,
             color=RED, lw=1.4, dash=[4, 3])

    # Labels for PowerTask arrows
    label(c, PT_CX - 60, (PT_TOP + BQ_CY - BQ_H / 2) / 2 + 4,
          "monitor", font=FONT_O, size=8, color=MUTED, align="center")
    label(c, PT_CX + 135, (PT_TOP + DQ_CY - DQ_H / 2) / 2,
          "monitor", font=FONT_O, size=8, color=MUTED, align="center")
    label(c, PT_X - 30, PT_Y + PT_H / 2 + 10,
          "stop", font=FONT_B, size=8, color=RED)

    # ─────────────────────────────────────────────────────────────────────────
    # LP Core + PIR (bottom left)
    # ─────────────────────────────────────────────────────────────────────────

    LP_X, LP_Y, LP_W, LP_H = 38, 18, 220, 78
    rounded_rect(c, LP_X, LP_Y, LP_W, LP_H, r=8,
                 fill=LP_BG, stroke=LP_BD, lw=2)
    label(c, LP_X + LP_W / 2, LP_Y + LP_H - 18,
          "LP Core  (ULP)", font=FONT_B, size=12, color=LP_BD, align="center")
    c.setStrokeColor(LP_BD)
    c.setLineWidth(0.5)
    c.setDash(3, 2)
    c.line(LP_X + 10, LP_Y + LP_H - 26, LP_X + LP_W - 10, LP_Y + LP_H - 26)
    c.setDash()
    label(c, LP_X + LP_W / 2, LP_Y + LP_H - 40,
          "ulp_pir_monitor.c", font=FONT_O, size=9, color=MUTED, align="center")
    label(c, LP_X + LP_W / 2, LP_Y + LP_H - 55,
          "GPIO interrupt on PIR POSEDGE", font=FONT, size=9,
          color=DARK, align="center")
    label(c, LP_X + LP_W / 2, LP_Y + LP_H - 69,
          "500 ms debounce  ·  ~1 µA core current", font=FONT, size=8.5,
          color=MUTED, align="center")

    # PIR sensor box
    PIR_W, PIR_H = 100, 44
    PIR_X = LP_X + LP_W + 16
    PIR_Y = LP_Y + (LP_H - PIR_H) / 2
    rounded_rect(c, PIR_X, PIR_Y, PIR_W, PIR_H, r=6,
                 fill=EXT_BG, stroke=EXT_BD, lw=1.5)
    label(c, PIR_X + PIR_W / 2, PIR_Y + 26,
          "PIR sensor", font=FONT_B, size=10, color=MUTED, align="center")
    label(c, PIR_X + PIR_W / 2, PIR_Y + 11,
          "GPIO 5 (default)", font=FONT, size=8.5, color=MUTED, align="center")

    # PIR → LP Core
    horiz_arrow(c, PIR_X, PIR_Y + PIR_H / 2,
                LP_X + LP_W,
                col=LP_BD, lw=2)

    # LP Core → HP Core wakeup arrow (up to PowerTask)
    LP_CX = LP_X + LP_W / 2
    arrow(c, LP_CX, LP_Y + LP_H,
             PT_X + 30, PT_Y,
             color=LP_BD, lw=2)
    label(c, LP_CX - 60, (LP_Y + LP_H + PT_Y) / 2,
          "wakeup_main_processor()", font=FONT_B, size=8.5, color=LP_BD)

    # ─────────────────────────────────────────────────────────────────────────
    # Legend — bottom right
    # ─────────────────────────────────────────────────────────────────────────

    LG_X, LG_Y = 650, 14
    items = [
        (CORE0_BD, CORE0_BG, "Core 0 task"),
        (CORE1_BD, CORE1_BG, "Core 1 task"),
        (Q_BD,     Q_BG,     "Queue"),
        (LP_BD,    LP_BG,    "LP core"),
        (EXT_BD,   EXT_BG,   "External"),
    ]
    label(c, LG_X, LG_Y + 60, "Legend:", font=FONT_B, size=9, color=MUTED)
    for i, (bd, bg, name) in enumerate(items):
        lx = LG_X + i * 84
        rounded_rect(c, lx, LG_Y + 36, 18, 14, r=3, fill=bg, stroke=bd, lw=1.2)
        label(c, lx + 22, LG_Y + 38, name, font=FONT, size=8.5, color=MUTED)

    # Dashed line legend
    dash_lx = LG_X
    c.setStrokeColor(CORE0_BD)
    c.setLineWidth(1.2)
    c.setDash(4, 3)
    c.line(dash_lx, LG_Y + 20, dash_lx + 24, LG_Y + 20)
    c.setDash()
    label(c, dash_lx + 28, LG_Y + 16, "monitor / control signal", font=FONT, size=8.5, color=MUTED)

    c.setStrokeColor(RED)
    c.setLineWidth(1.2)
    c.setDash(4, 3)
    c.line(dash_lx + 175, LG_Y + 20, dash_lx + 199, LG_Y + 20)
    c.setDash()
    label(c, dash_lx + 203, LG_Y + 16, "shutdown signal", font=FONT, size=8.5, color=RED)

    # ─────────────────────────────────────────────────────────────────────────
    # Queue property annotations (depth / drop policy callout boxes)
    # ─────────────────────────────────────────────────────────────────────────

    # Memory note
    label(c, W - 20, 14,
          "SRAM frame buffers: 2 × 75 KB  ·  RGB888 inference buf: 150 KB  ·  Model weights: ~2 MB PSRAM",
          font=FONT_O, size=8, color=MUTED, align="right")


# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    out = "docs/esp32-p4-task-diagram.pdf"
    c = canvas.Canvas(out, pagesize=(W, H))
    c.setTitle("ai-trap ESP32-P4 — FreeRTOS Tasks & Queues")
    c.setAuthor("NatureSense")
    draw(c)
    c.save()
    print(f"Saved: {out}")


if __name__ == "__main__":
    main()
