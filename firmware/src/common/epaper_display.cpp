// ─────────────────────────────────────────────────────────────────────────────
//  epaper_display.cpp  —  Waveshare 2.13" e-Paper HAT (250×122) driver
//
//  EPD model: V3 / V4
//  Interface: Linux /dev/spidevX.Y + GPIO chardev (/dev/gpiochipN)
//  No external library dependencies.
// ─────────────────────────────────────────────────────────────────────────────

#include "epaper_display.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

// ─────────────────────────────────────────────────────────────────────────────
//  8×8 bitmap font, ASCII 32–127
//  Each byte is one row (top→bottom); bit 7 = leftmost pixel.
//  1 = foreground (black on white display).
// ─────────────────────────────────────────────────────────────────────────────

static const uint8_t kFont8x8[96][8] = {
    /* 32   */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 33 ! */ {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
    /* 34 " */ {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 35 # */ {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},
    /* 36 $ */ {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00},
    /* 37 % */ {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},
    /* 38 & */ {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00},
    /* 39 ' */ {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00},
    /* 40 ( */ {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00},
    /* 41 ) */ {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},
    /* 42 * */ {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
    /* 43 + */ {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},
    /* 44 , */ {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06},
    /* 45 - */ {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},
    /* 46 . */ {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00},
    /* 47 / */ {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},
    /* 48 0 */ {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00},
    /* 49 1 */ {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},
    /* 50 2 */ {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00},
    /* 51 3 */ {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},
    /* 52 4 */ {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00},
    /* 53 5 */ {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},
    /* 54 6 */ {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00},
    /* 55 7 */ {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},
    /* 56 8 */ {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00},
    /* 57 9 */ {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},
    /* 58 : */ {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00},
    /* 59 ; */ {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06},
    /* 60 < */ {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00},
    /* 61 = */ {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00},
    /* 62 > */ {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},
    /* 63 ? */ {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},
    /* 64 @ */ {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00},
    /* 65 A */ {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},
    /* 66 B */ {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00},
    /* 67 C */ {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},
    /* 68 D */ {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00},
    /* 69 E */ {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00},
    /* 70 F */ {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00},
    /* 71 G */ {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},
    /* 72 H */ {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00},
    /* 73 I */ {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    /* 74 J */ {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00},
    /* 75 K */ {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},
    /* 76 L */ {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00},
    /* 77 M */ {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},
    /* 78 N */ {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00},
    /* 79 O */ {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},
    /* 80 P */ {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00},
    /* 81 Q */ {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},
    /* 82 R */ {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00},
    /* 83 S */ {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},
    /* 84 T */ {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    /* 85 U */ {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},
    /* 86 V */ {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00},
    /* 87 W */ {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    /* 88 X */ {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00},
    /* 89 Y */ {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},
    /* 90 Z */ {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00},
    /* 91 [ */ {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},
    /* 92 \ */ {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},
    /* 93 ] */ {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00},
    /* 94 ^ */ {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},
    /* 95 _ */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    /* 96 ` */ {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00},
    /* 97 a */ {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00},
    /* 98 b */ {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00},
    /* 99 c */ {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00},
    /*100 d */ {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00},
    /*101 e */ {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00},
    /*102 f */ {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00},
    /*103 g */ {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F},
    /*104 h */ {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00},
    /*105 i */ {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00},
    /*106 j */ {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E},
    /*107 k */ {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00},
    /*108 l */ {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    /*109 m */ {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00},
    /*110 n */ {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00},
    /*111 o */ {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00},
    /*112 p */ {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F},
    /*113 q */ {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78},
    /*114 r */ {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00},
    /*115 s */ {0x00,0x00,0x1E,0x03,0x1E,0x30,0x1F,0x00},
    /*116 t */ {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00},
    /*117 u */ {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00},
    /*118 v */ {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00},
    /*119 w */ {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},
    /*120 x */ {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00},
    /*121 y */ {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F},
    /*122 z */ {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00},
    /*123 { */ {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},
    /*124 | */ {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
    /*125 } */ {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},
    /*126 ~ */ {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00},
    /*127 DEL*/{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

EpaperDisplay::EpaperDisplay() {
    memset(m_fb, 0xFF, sizeof(m_fb));
}

EpaperDisplay::~EpaperDisplay() {
    close();
}

// ─────────────────────────────────────────────────────────────────────────────
//  GPIO chardev helpers
//
//  Uses the Linux GPIO character device interface (/dev/gpiochipN) rather than
//  the legacy sysfs interface.  On Pi 5 (RP1), the pinctrl driver marks all
//  pins as "owned" which causes EBUSY on sysfs export; chardev always works.
// ─────────────────────────────────────────────────────────────────────────────

// Open the /dev/gpiochipN device for the main 40-pin header GPIO controller.
//
// Strategy: find the sysfs entry with the highest base that exposes >= 28 lines
// (the BCM header bank), read its label, then scan /dev/gpiochip0..31 using
// GPIO_GET_CHIPINFO_IOCTL to find the device node with the matching label.
//
// This is necessary because the sysfs entry is named after its base offset
// (e.g. "gpiochip569" on Pi 5) while the /dev/ node is numbered by chip index
// (e.g. "/dev/gpiochip0"), so we cannot build the /dev path directly from
// the sysfs name.
//
// Returns an open fd (caller must close) or -1.
static int openGpioChip() {
    // Step 1: find the sysfs chip with the highest base that has >= 28 lines.
    int  highestBase = -1;
    char bestLabel[64] = {};

    DIR* d = opendir("/sys/class/gpio");
    if (!d) return -1;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (strncmp(ent->d_name, "gpiochip", 8) != 0) continue;
        char basePath[80], ngpioPath[80], labelPath[80];
        snprintf(basePath,  sizeof(basePath),  "/sys/class/gpio/%s/base",  ent->d_name);
        snprintf(ngpioPath, sizeof(ngpioPath), "/sys/class/gpio/%s/ngpio", ent->d_name);
        snprintf(labelPath, sizeof(labelPath), "/sys/class/gpio/%s/label", ent->d_name);
        int base = -1, ngpio = 0;
        FILE* fb = fopen(basePath,  "r");
        FILE* fn = fopen(ngpioPath, "r");
        if (fb) { fscanf(fb, "%d", &base);  fclose(fb); }
        if (fn) { fscanf(fn, "%d", &ngpio); fclose(fn); }
        if (base >= 0 && ngpio >= 28 && base > highestBase) {
            highestBase = base;
            FILE* fl = fopen(labelPath, "r");
            if (fl) {
                if (fgets(bestLabel, sizeof(bestLabel), fl)) {
                    // strip trailing newline
                    size_t l = strlen(bestLabel);
                    if (l > 0 && bestLabel[l-1] == '\n') bestLabel[l-1] = '\0';
                }
                fclose(fl);
            }
        }
    }
    closedir(d);

    // Step 2: scan /dev/gpiochip0..31 and match by label via GPIO_GET_CHIPINFO_IOCTL.
    for (int i = 0; i < 32; i++) {
        char devPath[32];
        snprintf(devPath, sizeof(devPath), "/dev/gpiochip%d", i);
        int fd = ::open(devPath, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;

        struct gpiochip_info info = {};
        if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) < 0) {
            ::close(fd);
            continue;
        }

        bool match = (bestLabel[0] != '\0')
                   ? (strncmp(info.label, bestLabel, sizeof(info.label)) == 0)
                   : ((int)info.lines >= 28);  // fallback: first chip with enough lines

        if (match) {
            fprintf(stderr, "[epaper] GPIO chip: %s (label=%s lines=%u)\n",
                    devPath, info.label, info.lines);
            return fd;
        }
        ::close(fd);
    }

    fprintf(stderr, "[epaper] GPIO chip not found (label=%s)\n", bestLabel);
    return -1;
}

// Request a single output line. Returns the line fd or -1.
static int gpioRequestOutput(int chipFd, int offset, int initVal, const char* label) {
    struct gpiohandle_request req = {};
    req.lineoffsets[0]    = (uint32_t)offset;
    req.flags             = GPIOHANDLE_REQUEST_OUTPUT;
    req.default_values[0] = (uint8_t)(initVal ? 1 : 0);
    req.lines             = 1;
    strncpy(req.consumer_label, label, sizeof(req.consumer_label) - 1);
    if (ioctl(chipFd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
        fprintf(stderr, "[epaper] request output GPIO%d (%s): %s\n",
                offset, label, strerror(errno));
        return -1;
    }
    return req.fd;
}

// Request a single input line. Returns the line fd or -1.
static int gpioRequestInput(int chipFd, int offset, const char* label) {
    struct gpiohandle_request req = {};
    req.lineoffsets[0] = (uint32_t)offset;
    req.flags          = GPIOHANDLE_REQUEST_INPUT;
    req.lines          = 1;
    strncpy(req.consumer_label, label, sizeof(req.consumer_label) - 1);
    if (ioctl(chipFd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
        fprintf(stderr, "[epaper] request input GPIO%d (%s): %s\n",
                offset, label, strerror(errno));
        return -1;
    }
    return req.fd;
}

void EpaperDisplay::gpioWrite(int lineFd, int value) {
    if (lineFd < 0) return;
    struct gpiohandle_data data = {};
    data.values[0] = (uint8_t)(value ? 1 : 0);
    ioctl(lineFd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
}

int EpaperDisplay::gpioRead(int lineFd) {
    if (lineFd < 0) return 0;
    struct gpiohandle_data data = {};
    ioctl(lineFd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data);
    return data.values[0];
}

// ─────────────────────────────────────────────────────────────────────────────
//  SPI
// ─────────────────────────────────────────────────────────────────────────────

bool EpaperDisplay::spiOpen() {
    m_spiFd = ::open(m_cfg.spiDev.c_str(), O_RDWR);
    if (m_spiFd < 0) {
        fprintf(stderr, "[epaper] cannot open %s: %s\n",
                m_cfg.spiDev.c_str(), strerror(errno));
        return false;
    }

    uint8_t mode  = SPI_MODE_0;
    uint8_t bits  = 8;
    uint32_t speed = (uint32_t)m_cfg.spiSpeedHz;

    if (ioctl(m_spiFd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(m_spiFd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(m_spiFd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        fprintf(stderr, "[epaper] SPI ioctl setup failed: %s\n", strerror(errno));
        ::close(m_spiFd);
        m_spiFd = -1;
        return false;
    }
    return true;
}

void EpaperDisplay::spiClose() {
    if (m_spiFd >= 0) {
        ::close(m_spiFd);
        m_spiFd = -1;
    }
}

void EpaperDisplay::spiSend(const uint8_t* data, int len) {
    if (m_spiFd < 0 || len <= 0) return;

    const int chunkSize = 4096;
    int offset = 0;

    while (offset < len) {
        int n = len - offset;
        if (n > chunkSize) n = chunkSize;

        struct spi_ioc_transfer tr;
        memset(&tr, 0, sizeof(tr));
        tr.tx_buf        = (unsigned long)(data + offset);
        tr.rx_buf        = 0;
        tr.len           = (uint32_t)n;
        tr.speed_hz      = (uint32_t)m_cfg.spiSpeedHz;
        tr.bits_per_word = 8;
        tr.delay_usecs   = 0;

        if (ioctl(m_spiFd, SPI_IOC_MESSAGE(1), &tr) < 0)
            fprintf(stderr, "[epaper] SPI write failed: %s\n", strerror(errno));

        offset += n;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  EPD hardware
// ─────────────────────────────────────────────────────────────────────────────

void EpaperDisplay::reset() {
    gpioWrite(m_fdRst, 1);
    usleep(20000);
    gpioWrite(m_fdRst, 0);
    usleep(2000);
    gpioWrite(m_fdRst, 1);
    usleep(20000);
}

void EpaperDisplay::waitBusy(int timeoutMs) {
    auto start    = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::milliseconds(timeoutMs);
    int polls = 0;
    while (gpioRead(m_fdBusy) == 1) {   // HIGH = busy
        if (std::chrono::steady_clock::now() > deadline) {
            fprintf(stderr, "[epaper] waitBusy timeout (%d ms)\n", timeoutMs);
            return;
        }
        usleep(10000);  // 10 ms poll interval
        polls++;
    }
    if (polls > 0) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start).count();
        fprintf(stderr, "[epaper] waitBusy done in %lld ms\n", (long long)ms);
    }
}

void EpaperDisplay::sendCmd(uint8_t cmd) {
    gpioWrite(m_fdDc, 0);  // DC low = command
    spiSend(&cmd, 1);
}

void EpaperDisplay::sendData(uint8_t data) {
    gpioWrite(m_fdDc, 1);  // DC high = data
    spiSend(&data, 1);
}

void EpaperDisplay::sendDataBuf(const uint8_t* buf, int len) {
    gpioWrite(m_fdDc, 1);  // DC high = data
    spiSend(buf, len);
}

void EpaperDisplay::initPanel() {
    // Reset + SW reset
    reset();
    waitBusy();
    sendCmd(0x12);   // SW Reset
    waitBusy();

    // Driver output control: MUX = 249 (0xF9) for 250 gate lines, GD=0, SM=0, TB=0
    sendCmd(0x01);
    sendData(0xF9); sendData(0x00); sendData(0x00);

    // Data entry mode: X+, Y+ (increment X first, then Y)
    sendCmd(0x11);
    sendData(0x03);

    // RAM-X address range: 0 .. 15 (covers 128 source lines >= 122)
    sendCmd(0x44);
    sendData(0x00); sendData(0x0F);

    // RAM-Y address range: 0 .. 249
    sendCmd(0x45);
    sendData(0x00); sendData(0x00);
    sendData(0xF9); sendData(0x00);

    // Border waveform
    sendCmd(0x3C);
    sendData(0x05);

    // Display update control 1: normal source/gate output
    sendCmd(0x21);
    sendData(0x00); sendData(0x80);

    // Temperature: built-in sensor
    sendCmd(0x18);
    sendData(0x80);

    // Display update control 2 + master activation (full init update)
    sendCmd(0x22);
    sendData(0xF7);
    sendCmd(0x20);
    waitBusy();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Framebuffer
//
//  Physical display: 250 gate lines (Y-axis), 122 source outputs (X-axis).
//  RAM layout: 16 bytes × 250 gate lines = 4000 bytes.
//
//  Logical coordinate (x, y): x ∈ [0,249] left→right, y ∈ [0,121] top→bottom.
//    RAM_Y      = x          (gate line index)
//    RAM_X_byte = y / 8      (source byte index within that gate line)
//    bit        = 7 - (y%8)  (MSB = leftmost source)
//    1 = white, 0 = black
// ─────────────────────────────────────────────────────────────────────────────

void EpaperDisplay::fbClear() {
    memset(m_fb, 0xFF, sizeof(m_fb));  // 0xFF = all white
}

void EpaperDisplay::fbPixel(int x, int y, bool black) {
    if (x < 0 || x >= 250 || y < 0 || y >= 122) return;
    int byteIdx = x * 16 + (y / 8);
    int bit     = 7 - (y % 8);
    if (black)
        m_fb[byteIdx] &= ~(1 << bit);
    else
        m_fb[byteIdx] |=  (1 << bit);
}

void EpaperDisplay::fbHLine(int x0, int x1, int y) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    for (int x = x0; x <= x1; x++)
        fbPixel(x, y, true);
}

// Draw 8×8 glyph with top-left at (x, y) in logical coordinates.
// Font row byte: bit 7 = leftmost column.
void EpaperDisplay::fbText(int x, int y, const char* s) {
    for (; *s; s++, x += 8) {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c > 127) c = 32;
        const uint8_t* glyph = kFont8x8[c - 32];
        for (int row = 0; row < 8; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col))
                    fbPixel(x + col, y + row, true);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Uptime formatting
// ─────────────────────────────────────────────────────────────────────────────

std::string EpaperDisplay::fmtUptime(long long secs) {
    char buf[32];
    if (secs < 60) {
        snprintf(buf, sizeof(buf), "%llds", secs);
    } else if (secs < 3600) {
        long long m = secs / 60;
        long long s = secs % 60;
        snprintf(buf, sizeof(buf), "%lldm %llds", m, s);
    } else {
        long long h = secs / 3600;
        long long m = (secs % 3600) / 60;
        snprintf(buf, sizeof(buf), "%lldh %lldm", h, m);
    }
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────────────────────────

bool EpaperDisplay::open(Config cfg) {
    if (m_open) return true;
    m_cfg = cfg;

    fprintf(stderr, "[epaper] open: spi=%s dc=GPIO%d rst=GPIO%d busy=GPIO%d speed=%d\n",
            m_cfg.spiDev.c_str(),
            m_cfg.pinDc, m_cfg.pinRst, m_cfg.pinBusy, m_cfg.spiSpeedHz);

    // Open the GPIO chip and request lines via chardev (works on Pi 4 and Pi 5).
    // m_cfg.pinDc/pinRst/pinBusy are BCM GPIO numbers (= line offsets in the chip).
    int chipFd = openGpioChip();
    if (chipFd < 0) {
        fprintf(stderr, "[epaper] cannot open GPIO chip: %s\n", strerror(errno));
        return false;
    }
    m_fdDc   = gpioRequestOutput(chipFd, m_cfg.pinDc,  0, "epaper-dc");
    m_fdRst  = gpioRequestOutput(chipFd, m_cfg.pinRst, 1, "epaper-rst");
    m_fdBusy = gpioRequestInput (chipFd, m_cfg.pinBusy,   "epaper-busy");
    ::close(chipFd);

    if (m_fdDc < 0 || m_fdRst < 0 || m_fdBusy < 0) {
        fprintf(stderr, "[epaper] GPIO line request failed\n");
        if (m_fdDc   >= 0) { ::close(m_fdDc);   m_fdDc   = -1; }
        if (m_fdRst  >= 0) { ::close(m_fdRst);  m_fdRst  = -1; }
        if (m_fdBusy >= 0) { ::close(m_fdBusy); m_fdBusy = -1; }
        return false;
    }
    fprintf(stderr, "[epaper] GPIO ok, BUSY=%d\n", gpioRead(m_fdBusy));

    if (!spiOpen()) {
        ::close(m_fdDc);   m_fdDc   = -1;
        ::close(m_fdRst);  m_fdRst  = -1;
        ::close(m_fdBusy); m_fdBusy = -1;
        return false;
    }
    fprintf(stderr, "[epaper] SPI open ok\n");

    fprintf(stderr, "[epaper] initPanel start\n");
    initPanel();
    fprintf(stderr, "[epaper] initPanel done\n");

    fbClear();
    m_open = true;
    fprintf(stderr, "[epaper] ready\n");
    return true;
}

void EpaperDisplay::showLoading(const std::string& trapId) {
    if (!m_open) return;

    fbClear();

    char buf[64];
    snprintf(buf, sizeof(buf), "Trap: %s", trapId.c_str());
    fbText(3, 3, buf);

    fbHLine(0, 249, 16);
    fbHLine(0, 249, 17);

    fbText(3, 22, "Loading...");

    sendCmd(0x4E); sendData(0x00);
    sendCmd(0x4F); sendData(0x00); sendData(0x00);
    sendCmd(0x24);
    sendDataBuf(m_fb, sizeof(m_fb));
    sendCmd(0x22); sendData(0xF7);
    sendCmd(0x20);
    waitBusy(5000);
}

void EpaperDisplay::update(const Content& c) {
    if (!m_open) return;

    fbClear();

    // ── Line layout: 7 lines, pitch=15px, top margin y=3 ─────────────────────
    // Note: fbText(x, y, ...) draws glyph with column origin at y.
    // x = gate line (horizontal on display), y = source line (vertical).

    char buf[64];

    // Line 0  y=3:  "Trap: <trapId>"
    snprintf(buf, sizeof(buf), "Trap: %s", c.trapId.c_str());
    fbText(3, 3, buf);

    // Line 1  y=18: "Det: <N>  Trk: <N>"
    snprintf(buf, sizeof(buf), "Det: %d  Trk: %d", c.detections, c.tracks);
    fbText(3, 18, buf);

    // Horizontal rule between line 1 and line 2
    fbHLine(0, 249, 30);
    fbHLine(0, 249, 31);

    // Line 2  y=33: "IP: <ip>"  or "IP: -" if empty
    snprintf(buf, sizeof(buf), "IP: %s",
             c.ip.empty() ? "-" : c.ip.c_str());
    fbText(3, 33, buf);

    // Line 3  y=48: "WiFi: <mode>"  or "WiFi: unmanaged" if empty
    snprintf(buf, sizeof(buf), "WiFi: %s",
             c.wifiMode.empty() ? "unmanaged" : c.wifiMode.c_str());
    fbText(3, 48, buf);

    // Line 4  y=63: "Up: <Xh Ym>"
    snprintf(buf, sizeof(buf), "Up: %s", fmtUptime(c.uptimeSecs).c_str());
    fbText(3, 63, buf);

    // Line 5  y=78: "<timeStr>"
    fbText(3, 78, c.timeStr.c_str());

    // ── Send framebuffer to EPD ───────────────────────────────────────────────

    // Set RAM cursors
    sendCmd(0x4E); sendData(0x00);           // RAM-X cursor = 0
    sendCmd(0x4F); sendData(0x00); sendData(0x00);  // RAM-Y cursor = 0

    // Write RAM (black channel)
    sendCmd(0x24);
    sendDataBuf(m_fb, sizeof(m_fb));

    // Activate display update
    sendCmd(0x22); sendData(0xF7);
    sendCmd(0x20);
    waitBusy(5000);
}

void EpaperDisplay::sleep() {
    if (!m_open) return;
    sendCmd(0x10);
    sendData(0x01);  // deep sleep mode 1
}

void EpaperDisplay::close() {
    if (!m_open) return;
    sleep();
    spiClose();
    if (m_fdDc   >= 0) { ::close(m_fdDc);   m_fdDc   = -1; }
    if (m_fdRst  >= 0) { ::close(m_fdRst);  m_fdRst  = -1; }
    if (m_fdBusy >= 0) { ::close(m_fdBusy); m_fdBusy = -1; }
    m_open = false;
}
