#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  wifi_manager.h  —  WiFi provisioning (AP ↔ station mode switching)
//
//  Two backends selected at compile time:
//    WIFI_BACKEND_NM       — NetworkManager (Raspberry Pi 5)
//    WIFI_BACKEND_HOSTAPD  — hostapd + dnsmasq + wpa_supplicant (Luckfox)
//
//  Flow:
//    1. At startup, applyStartupMode() is called.
//       - If wifi_creds.conf exists  → switch to station mode.
//       - Otherwise                  → start/confirm AP mode.
//    2. Phone connects to the trap's AP (SSID: "ai-trap-<trapId>").
//    3. App calls POST /api/wifi  {"ssid":"...","password":"..."}.
//    4. setStation() saves creds and switches to station mode.
//    5. App calls POST /api/wifi/reset to return to AP mode.
//
//  AP defaults:
//    SSID     : "ai-trap-<trapId>" (truncated to 32 chars)
//    Password : "aiwildlife"  (override via [wifi] ap_password in config)
//    IP       : 192.168.4.1
// ─────────────────────────────────────────────────────────────────────────────

#include <string>

struct WifiStatus {
    std::string mode;       // "ap", "station", "unknown"
    std::string ssid;       // AP SSID or currently associated SSID
    bool        connected = false;   // false in AP mode; true once DHCP acquired
    std::string ip;         // current IPv4 on wlan0 (empty if none)
};

struct WifiConfig {
    std::string apPassword = "aiwildlife";
    std::string iface      = "wlan0";
    // Path to persisted station credentials file
    std::string credsPath  = "/opt/ai-trap/wifi_creds.conf";
};

class WifiManager {
public:
    WifiManager(const std::string& trapId, const WifiConfig& cfg);

    // Read the current WiFi state from the system (non-blocking)
    WifiStatus  getStatus()  const;

    // Switch to station mode with the given credentials.
    // Saves credentials to credsPath. Returns "" on success, error string on failure.
    std::string setStation(const std::string& ssid, const std::string& pass);

    // Clear saved credentials and switch back to AP mode.
    std::string resetToAP();

    // Called once at program startup to restore the appropriate mode.
    void applyStartupMode();

    const std::string& apSsid() const { return m_apSsid; }

private:
    std::string m_trapId;
    WifiConfig  m_cfg;
    std::string m_apSsid;  // computed from trapId in constructor

    bool        loadCreds(std::string& ssid, std::string& pass) const;
    void        saveCreds(const std::string& ssid, const std::string& pass) const;
    void        clearCreds() const;

    // Run a shell command; returns true if exit code is 0
    bool        run(const std::string& cmd) const;
    // Run a shell command; returns stdout output (trimmed)
    std::string readCmd(const std::string& cmd) const;

    // Current IPv4 address on m_cfg.iface ("" if none)
    std::string currentIp() const;

    // Platform-specific implementations
    WifiStatus  getStatusImpl()  const;
    std::string switchToAP()     const;
    std::string switchToStation(const std::string& ssid, const std::string& pass) const;
};
