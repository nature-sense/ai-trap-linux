#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  epaper_display.h  —  Waveshare 2.13" e-Paper HAT driver
//
//  Physical: 250 wide × 122 tall (landscape), monochrome, SPI.
//  Driver uses Linux /dev/spidevX.Y + sysfs GPIO.
//  No external library dependencies.
//
//  Usage:
//    EpaperDisplay disp;
//    EpaperDisplay::Config cfg;   // defaults: disabled
//    cfg.enabled = true;
//    if (disp.open(cfg)) {
//        EpaperDisplay::Content c;
//        c.trapId = "trap_001";
//        disp.update(c);
//        disp.sleep();
//    }
//    disp.close();
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <string>

class EpaperDisplay {
public:
    // ── Configuration ─────────────────────────────────────────────────────────
    struct Config {
        bool        enabled    = false;
        std::string spiDev     = "/dev/spidev0.0";
        int         pinDc      = 25;   // BCM GPIO: Data/Command
        int         pinRst     = 17;   // BCM GPIO: Reset (active-low)
        int         pinBusy    = 24;   // BCM GPIO: BUSY (HIGH = busy)
        int         spiSpeedHz = 2000000;
    };

    // ── Display content ───────────────────────────────────────────────────────
    struct Content {
        std::string trapId;
        int         detections  = 0;
        int         tracks      = 0;
        std::string ip;          // empty → show "-"
        std::string wifiMode;    // empty → show "unmanaged"
        long long   uptimeSecs  = 0;
        std::string timeStr;     // e.g. "2024-06-01 14:32"
    };

    // ── Public interface ──────────────────────────────────────────────────────
    EpaperDisplay();
    ~EpaperDisplay();

    bool open(Config cfg);
    void showLoading(const std::string& trapId);
    void update(const Content& c);
    void sleep();
    void close();
    bool isOpen() const { return m_open; }

private:
    // ── GPIO chardev helpers ──────────────────────────────────────────────────
    static void gpioWrite(int lineFd, int value);
    static int  gpioRead(int lineFd);

    // ── SPI ───────────────────────────────────────────────────────────────────
    bool     spiOpen();
    void     spiClose();
    void     spiSend(const uint8_t* data, int len);

    // ── EPD hardware ──────────────────────────────────────────────────────────
    void     reset();
    void     waitBusy(int timeoutMs = 5000);
    void     sendCmd(uint8_t cmd);
    void     sendData(uint8_t data);
    void     sendDataBuf(const uint8_t* buf, int len);
    void     initPanel();

    // ── Framebuffer helpers ───────────────────────────────────────────────────
    //  Physical: 250 gate lines (Y), 16 source bytes (X) — 4000 bytes total.
    //  Logical (x,y): x∈[0,249], y∈[0,121].
    //    RAM_Y = x,  RAM_X_byte = y/8,  bit = 7-(y%8).  1=white, 0=black.
    void     fbClear();
    void     fbPixel(int x, int y, bool black);
    void     fbHLine(int x0, int x1, int y);
    void     fbText(int x, int y, const char* s);

    // ── Formatting ────────────────────────────────────────────────────────────
    static std::string fmtUptime(long long secs);

    // ── State ─────────────────────────────────────────────────────────────────
    Config   m_cfg;
    bool     m_open    = false;
    int      m_spiFd   = -1;
    int      m_fdDc    = -1;   // GPIO chardev line fd: Data/Command
    int      m_fdRst   = -1;   // GPIO chardev line fd: Reset
    int      m_fdBusy  = -1;   // GPIO chardev line fd: BUSY (input)
    uint8_t  m_fb[16 * 250];  // 4000-byte framebuffer
};
