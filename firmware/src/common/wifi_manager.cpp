#include "wifi_manager.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor
// ─────────────────────────────────────────────────────────────────────────────

WifiManager::WifiManager(const std::string& trapId, const WifiConfig& cfg)
    : m_trapId(trapId), m_cfg(cfg)
{
    std::string candidate = "ai-trap-" + trapId;
    m_apSsid = candidate.substr(0, 32);

    if (m_cfg.inactivitySeconds > 0) {
        markActivity();   // seed timer so it doesn't fire immediately
        m_timerRunning = true;
        m_timerThread  = std::thread(&WifiManager::timerLoop, this);
    }
}

WifiManager::~WifiManager() {
    m_timerRunning = false;
    if (m_timerThread.joinable()) m_timerThread.join();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Inactivity timer
// ─────────────────────────────────────────────────────────────────────────────

void WifiManager::markActivity() {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    m_lastActivity.store(now, std::memory_order_relaxed);
}

void WifiManager::timerLoop() {
    while (m_timerRunning.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        if (!m_timerRunning.load()) break;

        auto lastRaw = m_lastActivity.load(std::memory_order_relaxed);
        auto last    = std::chrono::steady_clock::time_point(
                           std::chrono::steady_clock::duration(lastRaw));
        auto elapsed = std::chrono::steady_clock::now() - last;
        auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

        if (elapsedSec >= m_cfg.inactivitySeconds) {
            WifiStatus st = getStatus();
            if (st.mode != "off" && st.mode != "unknown") {
                printf("[wifi] inactivity timeout (%llds) — shutting down WiFi\n",
                       (long long)elapsedSec);
                shutdownImpl();
                markActivity();   // reset so we don't fire again immediately
                if (m_inactiveCb) m_inactiveCb();
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Shell helpers
// ─────────────────────────────────────────────────────────────────────────────

bool WifiManager::run(const std::string& cmd) const {
    int rc = system(cmd.c_str());
    return (rc == 0);
}

std::string WifiManager::readCmd(const std::string& cmd) const {
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return {};
    std::string out;
    char buf[256];
    while (fgets(buf, sizeof(buf), fp))
        out += buf;
    pclose(fp);
    // Trim trailing whitespace
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

std::string WifiManager::currentIp() const {
    std::string cmd = "ip -4 addr show " + m_cfg.iface +
                      " 2>/dev/null | grep -oP '(?<=inet )\\S+' | cut -d/ -f1";
    return readCmd(cmd);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Credentials persistence
// ─────────────────────────────────────────────────────────────────────────────

bool WifiManager::loadCreds(std::string& ssid, std::string& pass) const {
    std::ifstream f(m_cfg.credsPath);
    if (!f.is_open()) return false;
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "ssid")     ssid = val;
        if (key == "password") pass = val;
    }
    return !ssid.empty();
}

void WifiManager::saveCreds(const std::string& ssid, const std::string& pass) const {
    std::ofstream f(m_cfg.credsPath, std::ios::trunc);
    if (!f.is_open()) {
        fprintf(stderr, "[wifi] cannot write creds to %s\n", m_cfg.credsPath.c_str());
        return;
    }
    f << "ssid=" << ssid << "\n";
    f << "password=" << pass << "\n";
}

void WifiManager::clearCreds() const {
    ::remove(m_cfg.credsPath.c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────────────────────────

WifiStatus WifiManager::getStatus() const {
    return getStatusImpl();
}

std::string WifiManager::setStation(const std::string& ssid, const std::string& pass) {
    if (ssid.empty()) return "ssid is required";
    saveCreds(ssid, pass);
    std::string err = switchToStation(ssid, pass);
    if (!err.empty()) {
        clearCreds();
        return err;
    }
    printf("[wifi] switched to station mode, SSID=%s\n", ssid.c_str());
    return {};
}

std::string WifiManager::resetToAP() {
    clearCreds();
    std::string err = switchToAP();
    if (!err.empty()) return err;
    printf("[wifi] switched to AP mode, SSID=%s\n", m_apSsid.c_str());
    return {};
}

std::string WifiManager::shutdown() {
    std::string err = shutdownImpl();
    if (err.empty()) printf("[wifi] WiFi shut down\n");
    return err;
}

void WifiManager::applyStartupMode() {
    std::string ssid, pass;
    if (loadCreds(ssid, pass)) {
        printf("[wifi] startup: credentials found, connecting to %s\n", ssid.c_str());
        std::string err = switchToStation(ssid, pass);
        if (!err.empty()) {
            fprintf(stderr, "[wifi] station connect failed: %s — falling back to AP\n",
                    err.c_str());
            clearCreds();
            switchToAP();
        }
    } else {
        printf("[wifi] startup: no credentials, starting AP mode (%s)\n", m_apSsid.c_str());
        switchToAP();
    }
}

// =============================================================================
//  BACKEND: NetworkManager  (Raspberry Pi 5)
// =============================================================================
#if defined(WIFI_BACKEND_NM)

static constexpr const char* NM_AP_CON = "ai-trap-ap";
static constexpr const char* AP_IP     = "192.168.4.1/24";

WifiStatus WifiManager::getStatusImpl() const {
    WifiStatus st;
    st.ip = currentIp();

    // Check if our AP connection is active
    std::string apState = readCmd(
        std::string("nmcli -t -f STATE connection show --active 2>/dev/null | grep -c ") +
        NM_AP_CON);
    // Actually check by connection name:
    std::string activeNames = readCmd(
        "nmcli -t -f NAME,TYPE,STATE connection show --active 2>/dev/null");

    if (activeNames.find(NM_AP_CON) != std::string::npos) {
        st.mode      = "ap";
        st.ssid      = m_apSsid;
        st.connected = !st.ip.empty();
        return st;
    }

    // Check if any wifi connection is active
    // nmcli -t -f DEVICE,TYPE,STATE,CONNECTION dev
    std::string devOut = readCmd(
        "nmcli -t -f DEVICE,TYPE,STATE,CONNECTION dev 2>/dev/null");
    // Look for wlan0 connected line
    std::istringstream iss(devOut);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find(m_cfg.iface) == std::string::npos) continue;
        if (line.find(":connected:") != std::string::npos) {
            st.mode      = "station";
            st.connected = true;
            // Extract connection name (last field)
            size_t last = line.rfind(':');
            if (last != std::string::npos)
                st.ssid = line.substr(last + 1);
            return st;
        }
    }

    st.mode      = "unknown";
    st.connected = false;
    return st;
}

std::string WifiManager::switchToAP() const {
    // Delete any existing AP connection first
    run(std::string("nmcli connection delete ") + NM_AP_CON + " 2>/dev/null");

    // Escape SSID and password for shell
    auto esc = [](const std::string& s) {
        std::string out = "'";
        for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
        out += "'";
        return out;
    };

    // Create AP connection
    std::string addCmd =
        std::string("nmcli connection add type wifi ifname ") + m_cfg.iface +
        " con-name " + NM_AP_CON +
        " ssid " + esc(m_apSsid) +
        " mode ap"
        " ipv4.method shared"
        " ipv4.addresses " + AP_IP +
        " wifi-sec.key-mgmt wpa-psk"
        " wifi-sec.psk " + esc(m_cfg.apPassword) +
        " 2>&1";

    std::string addOut = readCmd(addCmd);
    if (!run(std::string("nmcli connection up ") + NM_AP_CON + " 2>/dev/null")) {
        return "nmcli: failed to bring up AP connection";
    }
    return {};
}

std::string WifiManager::switchToStation(const std::string& ssid,
                                          const std::string& pass) const {
    // Tear down AP if running
    run(std::string("nmcli connection down ") + NM_AP_CON + " 2>/dev/null");
    run(std::string("nmcli connection delete ") + NM_AP_CON + " 2>/dev/null");

    auto esc = [](const std::string& s) {
        std::string out = "'";
        for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
        out += "'";
        return out;
    };

    std::string cmd = "nmcli device wifi connect " + esc(ssid) +
                      " password " + esc(pass) +
                      " ifname " + m_cfg.iface + " 2>&1";
    std::string out = readCmd(cmd);
    if (out.find("successfully activated") == std::string::npos &&
        out.find("successfully") == std::string::npos) {
        return "nmcli connect failed: " + out;
    }
    return {};
}

std::string WifiManager::shutdownImpl() const {
    run(std::string("nmcli connection down ") + NM_AP_CON + " 2>/dev/null");
    run(std::string("nmcli connection delete ") + NM_AP_CON + " 2>/dev/null");
    run("nmcli device disconnect " + m_cfg.iface + " 2>/dev/null");
    run("nmcli radio wifi off 2>/dev/null");
    return {};
}

// =============================================================================
//  BACKEND: hostapd + dnsmasq + wpa_supplicant  (Luckfox / Buildroot)
// =============================================================================
#elif defined(WIFI_BACKEND_HOSTAPD)

static constexpr const char* HOSTAPD_CONF  = "/tmp/ai-trap-hostapd.conf";
static constexpr const char* DNSMASQ_CONF  = "/tmp/ai-trap-dnsmasq.conf";
static constexpr const char* WPA_CONF      = "/tmp/ai-trap-wpa.conf";
static constexpr const char* AP_IP_ADDR    = "192.168.4.1";
static constexpr const char* AP_NETMASK    = "255.255.255.0";
static constexpr int         AP_CHANNEL    = 6;

WifiStatus WifiManager::getStatusImpl() const {
    WifiStatus st;
    st.ip = currentIp();

    bool hostapdRunning = (system("pgrep -x hostapd > /dev/null 2>&1") == 0);
    bool wpasRunning    = (system("pgrep -x wpa_supplicant > /dev/null 2>&1") == 0);

    if (hostapdRunning) {
        st.mode      = "ap";
        st.ssid      = m_apSsid;
        st.connected = !st.ip.empty();
        return st;
    }

    if (wpasRunning) {
        st.mode      = "station";
        st.connected = !st.ip.empty();
        // Read current SSID from wpa_cli
        std::string wpaSsid = readCmd("wpa_cli -i " + m_cfg.iface +
                                      " status 2>/dev/null | grep '^ssid=' | cut -d= -f2");
        st.ssid = wpaSsid;
        return st;
    }

    st.mode      = "unknown";
    st.connected = false;
    return st;
}

std::string WifiManager::switchToAP() const {
    // Stop station-mode processes
    run("killall wpa_supplicant 2>/dev/null");
    run("killall udhcpc 2>/dev/null");
    // Brief pause to allow processes to exit
    run("sleep 1");

    // Set static IP
    run("ip addr flush dev " + m_cfg.iface);
    run("ip link set " + m_cfg.iface + " up");
    run(std::string("ip addr add ") + AP_IP_ADDR + "/" + AP_NETMASK +
        " dev " + m_cfg.iface);

    // Write hostapd config
    // hw_mode=g required for AIC8800DC — 'n' mode fails with -22
    {
        FILE* f = fopen(HOSTAPD_CONF, "w");
        if (!f) return std::string("cannot write ") + HOSTAPD_CONF;
        fprintf(f,
            "interface=%s\n"
            "driver=nl80211\n"
            "ssid=%s\n"
            "hw_mode=g\n"
            "channel=%d\n"
            "wpa=2\n"
            "wpa_passphrase=%s\n"
            "wpa_key_mgmt=WPA-PSK\n"
            "rsn_pairwise=CCMP\n"
            "ignore_broadcast_ssid=0\n",
            m_cfg.iface.c_str(),
            m_apSsid.c_str(),
            AP_CHANNEL,
            m_cfg.apPassword.c_str());
        fclose(f);
    }

    // Write dnsmasq config (DHCP only, no upstream DNS)
    {
        FILE* f = fopen(DNSMASQ_CONF, "w");
        if (!f) return std::string("cannot write ") + DNSMASQ_CONF;
        fprintf(f,
            "interface=%s\n"
            "dhcp-range=192.168.4.2,192.168.4.20,255.255.255.0,1h\n"
            "no-resolv\n"
            "no-hosts\n"
            "address=/#/%s\n",
            m_cfg.iface.c_str(),
            AP_IP_ADDR);
        fclose(f);
    }

    // Kill any previous instances
    run("killall hostapd dnsmasq 2>/dev/null");
    run("sleep 1");

    if (!run(std::string("hostapd -B ") + HOSTAPD_CONF)) {
        return "hostapd failed to start";
    }
    // dnsmasq: -k would run in foreground; -x writes pidfile
    if (!run(std::string("dnsmasq --conf-file=") + DNSMASQ_CONF + " --pid-file=/tmp/dnsmasq.pid")) {
        return "dnsmasq failed to start";
    }
    return {};
}

std::string WifiManager::switchToStation(const std::string& ssid,
                                          const std::string& pass) const {
    // Stop AP processes
    run("killall hostapd 2>/dev/null");
    run("killall dnsmasq 2>/dev/null");
    run("sleep 1");

    // Flush existing addresses
    run("ip addr flush dev " + m_cfg.iface);
    run("ip link set " + m_cfg.iface + " up");

    // Write wpa_supplicant config
    {
        FILE* f = fopen(WPA_CONF, "w");
        if (!f) return std::string("cannot write ") + WPA_CONF;
        fprintf(f,
            "ctrl_interface=/var/run/wpa_supplicant\n"
            "ctrl_interface_group=0\n"
            "update_config=1\n"
            "\n"
            "network={\n"
            "    ssid=\"%s\"\n"
            "    psk=\"%s\"\n"
            "    key_mgmt=WPA-PSK\n"
            "}\n",
            ssid.c_str(), pass.c_str());
        fclose(f);
    }

    run("mkdir -p /var/run/wpa_supplicant");

    if (!run(std::string("wpa_supplicant -B -i ") + m_cfg.iface +
             " -c " + WPA_CONF)) {
        return "wpa_supplicant failed to start";
    }

    // Request DHCP — udhcpc is standard on Buildroot; -q = quit after lease
    run(std::string("udhcpc -i ") + m_cfg.iface + " -b -q -t 10 2>/dev/null");

    // Verify we got an address
    std::string ip = currentIp();
    if (ip.empty()) {
        // Not fatal — DHCP may be slow; the caller can poll getStatus()
        fprintf(stderr, "[wifi] warning: no IP on %s after udhcpc\n", m_cfg.iface.c_str());
    }
    return {};
}

std::string WifiManager::shutdownImpl() const {
    run("killall hostapd dnsmasq wpa_supplicant udhcpc 2>/dev/null");
    run("ip link set " + m_cfg.iface + " down");
    return {};
}

// =============================================================================
//  BACKEND: stub (no WiFi management compiled in)
// =============================================================================
#else

WifiStatus WifiManager::getStatusImpl() const {
    WifiStatus st;
    st.mode = "unknown";
    st.ip   = currentIp();
    return st;
}

std::string WifiManager::switchToAP() const {
    return "no wifi backend compiled";
}

std::string WifiManager::switchToStation(const std::string&, const std::string&) const {
    return "no wifi backend compiled";
}

std::string WifiManager::shutdownImpl() const { return {}; }

#endif  // WIFI_BACKEND_*
