#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  wifi_manager.h  —  WiFi provisioning (AP ↔ station mode switching)
//
//  Two backends selected at compile time:
//    WIFI_BACKEND_NM       — NetworkManager (Raspberry Pi 5)
//    WIFI_BACKEND_HOSTAPD  — hostapd + dnsmasq + wpa_supplicant (Luckfox)
//
//  Lifecycle:
//    1. applyStartupMode() — restore AP or station mode from saved credentials.
//    2. Phone pairs via BLE → requests AP mode → connects to trap WiFi.
//    3. App POSTs credentials → setStation() saves and switches.
//    4. resetToAP() / shutdown() called when done.
//
//  Inactivity timer:
//    Call markActivity() on each HTTP request.
//    After inactivitySeconds with no activity, shutdown() is called
//    automatically and the optional onInactive callback fires.
//
//  AP defaults:
//    SSID     : "ai-trap-<trapId>" (truncated to 32 chars)
//    Password : "aiwildlife"
//    IP       : 192.168.4.1
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

struct WifiStatus {
    std::string mode;          // "ap", "station", "off", "unknown"
    std::string ssid;          // AP SSID or currently associated SSID
    bool        connected = false;   // true once DHCP acquired in station mode
    std::string ip;            // current IPv4 on wlan0 (empty if none)
};

struct WifiConfig {
    bool        managed           = true;  // false = skip AP/BLE management; use OS WiFi as-is
    std::string apPassword        = "aiwildlife";
    std::string iface             = "wlan0";
    std::string credsPath         = "/opt/ai-trap/wifi_creds.conf";
    int         inactivitySeconds = 600;   // 10 min; 0 = disabled
};

class WifiManager {
public:
    WifiManager(const std::string& trapId, const WifiConfig& cfg);
    ~WifiManager();

    // Read the current WiFi state from the system (non-blocking)
    WifiStatus  getStatus()  const;

    // Switch to station mode — saves credentials, switches networking.
    // Returns "" on success, error string on failure.
    std::string setStation(const std::string& ssid, const std::string& pass);

    // Clear saved credentials and switch back to AP mode.
    std::string resetToAP();

    // Shut WiFi down entirely (for power saving). BLE command wakes it again.
    std::string shutdown();

    // Called once at startup to restore the appropriate mode.
    void applyStartupMode();

    // Call on every HTTP request to reset the inactivity timer.
    void markActivity();

    // Optional: called when inactivity timer fires (e.g. to notify BLE stack)
    using InactiveCb = std::function<void()>;
    void setInactiveCallback(InactiveCb cb) { m_inactiveCb = std::move(cb); }

    const std::string& apSsid() const { return m_apSsid; }

private:
    std::string  m_trapId;
    WifiConfig   m_cfg;
    std::string  m_apSsid;
    InactiveCb   m_inactiveCb;

    // Inactivity timer
    std::atomic<std::chrono::steady_clock::time_point::rep> m_lastActivity{0};
    std::atomic<bool>   m_timerRunning{false};
    std::thread         m_timerThread;
    void                timerLoop();

    bool        loadCreds(std::string& ssid, std::string& pass) const;
    void        saveCreds(const std::string& ssid, const std::string& pass) const;
    void        clearCreds() const;

    bool        run(const std::string& cmd) const;
    std::string readCmd(const std::string& cmd) const;
    std::string currentIp() const;

    // Platform-specific
    WifiStatus  getStatusImpl()  const;
    std::string switchToAP()     const;
    std::string switchToStation(const std::string& ssid, const std::string& pass) const;
    std::string shutdownImpl()   const;
};
