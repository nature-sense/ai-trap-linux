#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  ble_gatt_server.h  —  Pure C++ BLE GATT server (no Python, no D-Bus)
//
//  Uses two raw Linux Bluetooth sockets:
//    HCI socket (HCI_CHANNEL_RAW)  — sends LE advertising commands
//    L2CAP socket (CID 4, ATT)     — handles GATT ATT protocol
//
//  GATT service  UUID: FFF0
//    FFF1  WiFi State  (read + notify)  — JSON UTF-8 string, ≤ MTU bytes
//    FFF2  WiFi Command (write)         — JSON UTF-8 string
//
//  WiFi State JSON example:
//    {"mode":"ap","ssid":"ai-trap-001","connected":false,"ip":"192.168.4.1"}
//
//  WiFi Command JSON examples:
//    {"cmd":"start_ap"}
//    {"cmd":"connect","ssid":"MyNet","password":"secret"}
//    {"cmd":"shutdown"}
//
//  Prerequisites:
//    - libbluetooth-dev (package) on Pi OS / Buildroot bluez package
//    - bluetoothd stopped/disabled (conflicts with raw L2CAP ATT socket):
//        sudo systemctl disable --now bluetooth
//    - hci0 powered up:
//        hciconfig hci0 up   (done automatically by open())
//
//  Usage:
//    BleGattServer ble;
//    BleGattConfig cfg;
//    cfg.name = "AI-Trap-001";
//    ble.open(cfg, &wifiManager);
//    // ... (call notifyStateChanged() whenever WiFi state changes)
//    ble.close();
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class WifiManager;

struct BleGattConfig {
    std::string name   = "AI-Trap";   // advertised BLE device name (max 20 chars)
    int         hciDev = 0;           // 0 → hci0
};

class BleGattServer {
public:
    BleGattServer()  = default;
    ~BleGattServer() { close(); }

    BleGattServer(const BleGattServer&)            = delete;
    BleGattServer& operator=(const BleGattServer&) = delete;

    // Start the BLE GATT server. Non-blocking — runs in a background thread.
    void open(const BleGattConfig& cfg, WifiManager* wifi);
    void close();

    // Call whenever WiFi state changes to send a GATT notification to any
    // connected BLE client that has enabled notifications on FFF1.
    void notifyStateChanged();

private:
    // ── Threads ──────────────────────────────────────────────────────────────
    void serverLoop();
    void clientLoop(int fd);

    // ── ATT dispatch ─────────────────────────────────────────────────────────
    void dispatchAtt(int fd, const uint8_t* pdu, size_t len);

    void attMtuReq     (int fd, const uint8_t* p, size_t n);
    void attFindInfo   (int fd, const uint8_t* p, size_t n);
    void attReadByType (int fd, const uint8_t* p, size_t n);
    void attReadByGroup(int fd, const uint8_t* p, size_t n);
    void attRead       (int fd, const uint8_t* p, size_t n);
    void attWrite      (int fd, const uint8_t* p, size_t n, bool withResp);

    // ── ATT response helpers ─────────────────────────────────────────────────
    void sendPdu   (int fd, const std::vector<uint8_t>& pdu);
    void sendError (int fd, uint8_t reqOp, uint16_t handle, uint8_t ecode);
    void sendNotify(int fd, uint16_t handle, const std::vector<uint8_t>& val);

    // ── Attribute value builders ─────────────────────────────────────────────
    std::vector<uint8_t> wifiStateBytes() const;

    // ── HCI advertising ──────────────────────────────────────────────────────
    bool setupAdvertising();
    void disableAdvertising();
    bool hciCmd(uint16_t ogf, uint16_t ocf,
                const uint8_t* param, uint8_t plen);

    // ── State ─────────────────────────────────────────────────────────────────
    BleGattConfig     m_cfg;
    WifiManager*      m_wifi     = nullptr;
    int               m_hciFd   = -1;   // HCI raw socket
    int               m_l2Fd    = -1;   // L2CAP listening socket
    int               m_clientFd= -1;   // connected ATT client (-1 = none)

    uint16_t          m_mtu     = 23;   // negotiated ATT MTU
    uint16_t          m_cccd    = 0;    // client characteristic config descriptor

    std::atomic<bool> m_running{false};
    std::thread       m_serverThread;
    std::mutex        m_clientMu;       // guards m_clientFd and m_cccd
};
