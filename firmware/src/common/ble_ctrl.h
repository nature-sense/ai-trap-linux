#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  ble_ctrl.h  —  IPC bridge between the C++ binary and the BLE GATT daemon
//
//  The Python ble_gatt.py script (running alongside this binary) connects to
//  a Unix domain socket created here.  All BLE/GATT complexity lives in the
//  Python script (BlueZ D-Bus); this class only handles the socket and
//  dispatches WiFi commands to WifiManager.
//
//  Protocol: newline-delimited JSON in both directions.
//
//  C++ → Python (state push):
//    {"event":"state","mode":"ap","ssid":"ai-trap-001",
//     "connected":false,"ip":"192.168.4.1"}
//    {"event":"state","mode":"station","ssid":"HomeNet",
//     "connected":true,"ip":"192.168.1.42"}
//    {"event":"state","mode":"off","ssid":"","connected":false,"ip":""}
//
//  Python → C++ (commands):
//    {"cmd":"start_ap"}
//    {"cmd":"connect","ssid":"MyNet","password":"secret"}
//    {"cmd":"shutdown"}
//    {"cmd":"get_state"}
//
//  The socket path defaults to /tmp/ai-trap-ble.sock.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class WifiManager;

struct BleCtrlConfig {
    std::string socketPath = "/tmp/ai-trap-ble.sock";
};

class BleCtrl {
public:
    BleCtrl()  = default;
    ~BleCtrl() { close(); }

    BleCtrl(const BleCtrl&)            = delete;
    BleCtrl& operator=(const BleCtrl&) = delete;

    void open(const BleCtrlConfig& cfg, WifiManager* wifi);
    void close();

    // Push the current WiFi state to any connected Python client.
    // Safe to call from any thread.
    void pushState();

private:
    void acceptLoop();
    void clientLoop(int fd);
    void handleCommand(int fd, const std::string& line);

    // Build the JSON state string from WifiManager
    std::string stateJson() const;

    BleCtrlConfig m_cfg;
    WifiManager*  m_wifi    = nullptr;
    int           m_sockFd  = -1;   // listening socket
    int           m_clientFd= -1;   // currently connected Python client

    std::atomic<bool> m_running{false};
    std::thread       m_acceptThread;
    std::mutex        m_clientMu;   // guards m_clientFd
};
