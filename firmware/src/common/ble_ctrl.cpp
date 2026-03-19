#include "ble_ctrl.h"
#include "wifi_manager.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// ─────────────────────────────────────────────────────────────────────────────
//  open / close
// ─────────────────────────────────────────────────────────────────────────────

void BleCtrl::open(const BleCtrlConfig& cfg, WifiManager* wifi) {
    m_cfg  = cfg;
    m_wifi = wifi;

    // Remove stale socket file
    ::unlink(m_cfg.socketPath.c_str());

    m_sockFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_sockFd < 0) {
        fprintf(stderr, "[ble_ctrl] socket: %s\n", strerror(errno));
        return;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, m_cfg.socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(m_sockFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        fprintf(stderr, "[ble_ctrl] bind: %s\n", strerror(errno));
        ::close(m_sockFd); m_sockFd = -1;
        return;
    }
    if (::listen(m_sockFd, 2) < 0) {
        fprintf(stderr, "[ble_ctrl] listen: %s\n", strerror(errno));
        ::close(m_sockFd); m_sockFd = -1;
        return;
    }

    m_running = true;
    m_acceptThread = std::thread(&BleCtrl::acceptLoop, this);
    printf("[ble_ctrl] listening on %s\n", m_cfg.socketPath.c_str());
}

void BleCtrl::close() {
    if (!m_running.exchange(false)) return;
    if (m_sockFd >= 0)  { ::close(m_sockFd);   m_sockFd   = -1; }
    {
        std::lock_guard<std::mutex> lk(m_clientMu);
        if (m_clientFd >= 0) { ::close(m_clientFd); m_clientFd = -1; }
    }
    if (m_acceptThread.joinable()) m_acceptThread.join();
    ::unlink(m_cfg.socketPath.c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
//  acceptLoop  — one client at a time (the ble_gatt.py script)
// ─────────────────────────────────────────────────────────────────────────────

void BleCtrl::acceptLoop() {
    while (m_running.load()) {
        int fd = ::accept(m_sockFd, nullptr, nullptr);
        if (fd < 0) {
            if (m_running.load())
                fprintf(stderr, "[ble_ctrl] accept: %s\n", strerror(errno));
            break;
        }
        printf("[ble_ctrl] BLE daemon connected\n");
        {
            std::lock_guard<std::mutex> lk(m_clientMu);
            m_clientFd = fd;
        }
        // Send current state immediately on connect
        pushState();
        // Block here until client disconnects
        clientLoop(fd);
        {
            std::lock_guard<std::mutex> lk(m_clientMu);
            ::close(m_clientFd);
            m_clientFd = -1;
        }
        printf("[ble_ctrl] BLE daemon disconnected\n");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  clientLoop  — read newline-delimited JSON commands from ble_gatt.py
// ─────────────────────────────────────────────────────────────────────────────

void BleCtrl::clientLoop(int fd) {
    std::string buf;
    char tmp[256];
    while (m_running.load()) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp) - 1, 0);
        if (n <= 0) break;
        tmp[n] = '\0';
        buf += tmp;

        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty())
                handleCommand(fd, line);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  handleCommand  — parse JSON command and act on it
// ─────────────────────────────────────────────────────────────────────────────

// Minimal JSON string extractor: finds "key":"value" and returns value
static std::string jsonStr(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos + search.size() + 1);
    if (pos == std::string::npos) return {};
    size_t end = json.find('"', pos + 1);
    if (end == std::string::npos) return {};
    return json.substr(pos + 1, end - pos - 1);
}

void BleCtrl::handleCommand(int fd, const std::string& line) {
    std::string cmd = jsonStr(line, "cmd");
    printf("[ble_ctrl] cmd: %s\n", line.c_str());

    if (!m_wifi) return;

    std::string err;
    if (cmd == "get_state") {
        // just pushState() below
    } else if (cmd == "start_ap") {
        err = m_wifi->resetToAP();
    } else if (cmd == "connect") {
        std::string ssid = jsonStr(line, "ssid");
        std::string pass = jsonStr(line, "password");
        err = m_wifi->setStation(ssid, pass);
    } else if (cmd == "shutdown") {
        err = m_wifi->shutdown();
    } else {
        fprintf(stderr, "[ble_ctrl] unknown cmd: %s\n", cmd.c_str());
    }

    if (!err.empty())
        fprintf(stderr, "[ble_ctrl] error: %s\n", err.c_str());

    // Always reply with current state after any command
    pushState();
}

// ─────────────────────────────────────────────────────────────────────────────
//  pushState  — send current WiFi state JSON to connected client
// ─────────────────────────────────────────────────────────────────────────────

std::string BleCtrl::stateJson() const {
    if (!m_wifi) return "{\"event\":\"state\",\"mode\":\"unknown\",\"ssid\":\"\",\"connected\":false,\"ip\":\"\"}\n";
    WifiStatus ws = m_wifi->getStatus();

    auto esc = [](const std::string& s) {
        std::string out; out.reserve(s.size() + 2);
        for (char c : s) {
            if (c == '"') out += "\\\"";
            else out += c;
        }
        return out;
    };

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"event\":\"state\","
        "\"mode\":\"%s\","
        "\"ssid\":\"%s\","
        "\"connected\":%s,"
        "\"ip\":\"%s\"}\n",
        esc(ws.mode).c_str(),
        esc(ws.ssid).c_str(),
        ws.connected ? "true" : "false",
        esc(ws.ip).c_str());
    return buf;
}

void BleCtrl::pushState() {
    std::string msg = stateJson();
    std::lock_guard<std::mutex> lk(m_clientMu);
    if (m_clientFd >= 0) {
        if (::send(m_clientFd, msg.c_str(), msg.size(), MSG_NOSIGNAL) < 0) {
            fprintf(stderr, "[ble_ctrl] send: %s\n", strerror(errno));
        }
    }
}
