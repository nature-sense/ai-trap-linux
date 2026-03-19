#include "ble_gatt_server.h"
#include "wifi_manager.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

// Bluetooth headers from libbluetooth-dev
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/l2cap.h>

// ─────────────────────────────────────────────────────────────────────────────
//  ATT protocol constants
// ─────────────────────────────────────────────────────────────────────────────

enum AttOp : uint8_t {
    ATT_OP_ERROR          = 0x01,
    ATT_OP_MTU_REQ        = 0x02,
    ATT_OP_MTU_RESP       = 0x03,
    ATT_OP_FIND_INFO_REQ  = 0x04,
    ATT_OP_FIND_INFO_RESP = 0x05,
    ATT_OP_READ_BY_TYPE_REQ  = 0x08,
    ATT_OP_READ_BY_TYPE_RESP = 0x09,
    ATT_OP_READ_REQ       = 0x0A,
    ATT_OP_READ_RESP      = 0x0B,
    ATT_OP_READ_BY_GROUP_REQ  = 0x10,
    ATT_OP_READ_BY_GROUP_RESP = 0x11,
    ATT_OP_WRITE_REQ      = 0x12,
    ATT_OP_WRITE_RESP     = 0x13,
    ATT_OP_WRITE_CMD      = 0x52,
    ATT_OP_HANDLE_NOTIFY  = 0x1B,
};

enum AttErr : uint8_t {
    ATT_ERR_INVALID_HANDLE = 0x01,
    ATT_ERR_READ_NOT_PERM  = 0x02,
    ATT_ERR_WRITE_NOT_PERM = 0x03,
    ATT_ERR_NOT_SUPPORTED  = 0x06,
    ATT_ERR_NOT_FOUND      = 0x0A,
};

// ─────────────────────────────────────────────────────────────────────────────
//  GATT attribute table
//
//  Handle  UUID    Description
//  0x0001  0x2800  Primary Service → WiFi Service (FFF0)
//  0x0002  0x2803  Characteristic Declaration → WiFi State (FFF1), Read|Notify
//  0x0003  0xFFF1  WiFi State Value  (read, notify)
//  0x0004  0x2902  Client Characteristic Config Descriptor (CCCD)
//  0x0005  0x2803  Characteristic Declaration → WiFi Command (FFF2), Write
//  0x0006  0xFFF2  WiFi Command Value  (write-without-response, write)
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint16_t HDL_SERVICE    = 0x0001;
static constexpr uint16_t HDL_STATE_DECL = 0x0002;
static constexpr uint16_t HDL_STATE_VAL  = 0x0003;
static constexpr uint16_t HDL_CCCD       = 0x0004;
static constexpr uint16_t HDL_CMD_DECL   = 0x0005;
static constexpr uint16_t HDL_CMD_VAL    = 0x0006;
static constexpr uint16_t HDL_LAST       = HDL_CMD_VAL;

// 16-bit UUIDs (little-endian in ATT PDUs)
static constexpr uint16_t UUID_PRIMARY_SVC = 0x2800;
static constexpr uint16_t UUID_CHAR_DECL   = 0x2803;
static constexpr uint16_t UUID_CCCD        = 0x2902;
static constexpr uint16_t UUID_WIFI_SVC    = 0xFFF0;
static constexpr uint16_t UUID_WIFI_STATE  = 0xFFF1;
static constexpr uint16_t UUID_WIFI_CMD    = 0xFFF2;

// Characteristic properties
static constexpr uint8_t PROP_READ   = 0x02;
static constexpr uint8_t PROP_WRITE_NR = 0x04;
static constexpr uint8_t PROP_WRITE  = 0x08;
static constexpr uint8_t PROP_NOTIFY = 0x10;

// L2CAP fixed channel for ATT
static constexpr uint16_t ATT_CID = 4;

// ─────────────────────────────────────────────────────────────────────────────
//  Utility: put/get little-endian 16-bit
// ─────────────────────────────────────────────────────────────────────────────

static void put16(std::vector<uint8_t>& v, uint16_t val) {
    v.push_back(val & 0xFF);
    v.push_back((val >> 8) & 0xFF);
}
static uint16_t get16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

// ─────────────────────────────────────────────────────────────────────────────
//  open / close
// ─────────────────────────────────────────────────────────────────────────────

void BleGattServer::open(const BleGattConfig& cfg, WifiManager* wifi) {
    m_cfg  = cfg;
    m_wifi = wifi;

    // ── HCI socket for advertising ───────────────────────────────────────────
    m_hciFd = ::socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (m_hciFd < 0) {
        fprintf(stderr, "[ble] HCI socket: %s\n", strerror(errno));
        return;
    }

    struct sockaddr_hci hciAddr{};
    hciAddr.hci_family  = AF_BLUETOOTH;
    hciAddr.hci_dev     = static_cast<uint16_t>(m_cfg.hciDev);
    hciAddr.hci_channel = HCI_CHANNEL_RAW;

    if (::bind(m_hciFd, reinterpret_cast<sockaddr*>(&hciAddr), sizeof(hciAddr)) < 0) {
        fprintf(stderr, "[ble] HCI bind: %s\n", strerror(errno));
        ::close(m_hciFd); m_hciFd = -1;
        return;
    }

    // Bring adapter up
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "hciconfig hci%d up 2>/dev/null", m_cfg.hciDev);
    system(cmd);

    // ── L2CAP ATT listening socket ───────────────────────────────────────────
    m_l2Fd = ::socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_L2CAP);
    if (m_l2Fd < 0) {
        fprintf(stderr, "[ble] L2CAP socket: %s\n", strerror(errno));
        ::close(m_hciFd); m_hciFd = -1;
        return;
    }

    struct sockaddr_l2 l2Addr{};
    l2Addr.l2_family      = AF_BLUETOOTH;
    l2Addr.l2_cid         = htobs(ATT_CID);
    l2Addr.l2_bdaddr_type = BDADDR_LE_PUBLIC;
    bacpy(&l2Addr.l2_bdaddr, BDADDR_ANY);

    // Allow re-bind after crash
    int one = 1;
    ::setsockopt(m_l2Fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    if (::bind(m_l2Fd, reinterpret_cast<sockaddr*>(&l2Addr), sizeof(l2Addr)) < 0) {
        fprintf(stderr, "[ble] L2CAP bind: %s\n", strerror(errno));
        ::close(m_hciFd); m_hciFd = -1;
        ::close(m_l2Fd);  m_l2Fd  = -1;
        return;
    }
    if (::listen(m_l2Fd, 2) < 0) {
        fprintf(stderr, "[ble] L2CAP listen: %s\n", strerror(errno));
        ::close(m_hciFd); m_hciFd = -1;
        ::close(m_l2Fd);  m_l2Fd  = -1;
        return;
    }

    if (!setupAdvertising()) {
        fprintf(stderr, "[ble] advertising setup failed\n");
    }

    m_running = true;
    m_serverThread = std::thread(&BleGattServer::serverLoop, this);
    printf("[ble] GATT server started (hci%d), advertising as '%s'\n",
           m_cfg.hciDev, m_cfg.name.c_str());
}

void BleGattServer::close() {
    if (!m_running.exchange(false)) return;
    disableAdvertising();
    if (m_l2Fd  >= 0) { ::close(m_l2Fd);  m_l2Fd  = -1; }
    if (m_hciFd >= 0) { ::close(m_hciFd); m_hciFd = -1; }
    {
        std::lock_guard<std::mutex> lk(m_clientMu);
        if (m_clientFd >= 0) { ::close(m_clientFd); m_clientFd = -1; }
    }
    if (m_serverThread.joinable()) m_serverThread.join();
}

// ─────────────────────────────────────────────────────────────────────────────
//  HCI advertising
// ─────────────────────────────────────────────────────────────────────────────

bool BleGattServer::hciCmd(uint16_t ogf, uint16_t ocf,
                            const uint8_t* param, uint8_t plen)
{
    if (m_hciFd < 0) return false;

    uint8_t  type  = HCI_COMMAND_PKT;
    uint16_t op    = htobs(cmd_opcode_pack(ogf, ocf));
    hci_command_hdr hdr;
    hdr.opcode = op;
    hdr.plen   = plen;

    struct iovec iv[3];
    iv[0].iov_base = &type;  iv[0].iov_len = 1;
    iv[1].iov_base = &hdr;   iv[1].iov_len = HCI_COMMAND_HDR_SIZE;
    int ivn = 2;
    if (plen && param) {
        iv[2].iov_base = const_cast<uint8_t*>(param);
        iv[2].iov_len  = plen;
        ivn = 3;
    }
    ssize_t r = ::writev(m_hciFd, iv, ivn);
    return (r > 0);
}

bool BleGattServer::setupAdvertising() {
    // ── LE advertising parameters ────────────────────────────────────────────
    // ADV_IND (connectable undirected), interval 100ms, own addr public
    struct {
        uint16_t min_interval;
        uint16_t max_interval;
        uint8_t  type;
        uint8_t  own_bdaddr_type;
        uint8_t  direct_bdaddr_type;
        bdaddr_t direct_bdaddr;
        uint8_t  chan_map;
        uint8_t  filter;
    } __attribute__((packed)) advParams{};

    advParams.min_interval     = htobs(0x00A0);  // 100 ms
    advParams.max_interval     = htobs(0x00A0);
    advParams.type             = 0x00;           // ADV_IND
    advParams.own_bdaddr_type  = 0x00;           // public
    advParams.direct_bdaddr_type = 0x00;
    memset(&advParams.direct_bdaddr, 0, sizeof(bdaddr_t));
    advParams.chan_map         = 0x07;           // channels 37, 38, 39
    advParams.filter           = 0x00;           // allow all

    if (!hciCmd(OGF_LE_CTL, OCF_LE_SET_ADV_PARAMETERS,
                reinterpret_cast<const uint8_t*>(&advParams), sizeof(advParams)))
        return false;

    // ── LE advertising data ───────────────────────────────────────────────────
    // Format: [length][type][data] ...
    // AD types: 0x01=Flags, 0x09=Complete Local Name, 0x03=16-bit UUID list

    struct {
        uint8_t length;
        uint8_t data[31];
    } __attribute__((packed)) advData{};

    uint8_t* d   = advData.data;
    uint8_t  len = 0;

    // Flags: LE General Discoverable, BR/EDR Not Supported
    d[len++] = 2; d[len++] = 0x01; d[len++] = 0x06;

    // 16-bit Service UUID list (FFF0)
    d[len++] = 3; d[len++] = 0x03;
    d[len++] = UUID_WIFI_SVC & 0xFF;
    d[len++] = (UUID_WIFI_SVC >> 8) & 0xFF;

    // Complete local name (truncated to fit remaining space)
    std::string name = m_cfg.name.substr(0, 20);
    uint8_t namelen  = static_cast<uint8_t>(name.size());
    if (len + 2 + namelen <= 31) {
        d[len++] = namelen + 1; d[len++] = 0x09;
        memcpy(d + len, name.data(), namelen);
        len += namelen;
    }

    advData.length = len;

    if (!hciCmd(OGF_LE_CTL, OCF_LE_SET_ADV_DATA,
                reinterpret_cast<const uint8_t*>(&advData), sizeof(advData)))
        return false;

    // ── Enable advertising ────────────────────────────────────────────────────
    uint8_t enable = 1;
    return hciCmd(OGF_LE_CTL, OCF_LE_SET_ADVERTISE_ENABLE, &enable, 1);
}

void BleGattServer::disableAdvertising() {
    if (m_hciFd < 0) return;
    uint8_t disable = 0;
    hciCmd(OGF_LE_CTL, OCF_LE_SET_ADVERTISE_ENABLE, &disable, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
//  serverLoop  — accept one client at a time
// ─────────────────────────────────────────────────────────────────────────────

void BleGattServer::serverLoop() {
    while (m_running.load()) {
        struct sockaddr_l2 peer{};
        socklen_t peerLen = sizeof(peer);
        int fd = ::accept(m_l2Fd,
                          reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (fd < 0) {
            if (m_running.load())
                fprintf(stderr, "[ble] accept: %s\n", strerror(errno));
            break;
        }

        char addr[18];
        ba2str(&peer.l2_bdaddr, addr);
        printf("[ble] BLE client connected: %s\n", addr);

        {
            std::lock_guard<std::mutex> lk(m_clientMu);
            m_clientFd = fd;
            m_mtu  = 23;
            m_cccd = 0;
        }

        clientLoop(fd);

        {
            std::lock_guard<std::mutex> lk(m_clientMu);
            ::close(m_clientFd);
            m_clientFd = -1;
            m_cccd = 0;
        }
        printf("[ble] BLE client disconnected\n");

        // Re-enable advertising so the next client can find us
        setupAdvertising();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  clientLoop  — read and dispatch ATT PDUs
// ─────────────────────────────────────────────────────────────────────────────

void BleGattServer::clientLoop(int fd) {
    uint8_t buf[512];
    while (m_running.load()) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        dispatchAtt(fd, buf, static_cast<size_t>(n));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  ATT dispatch
// ─────────────────────────────────────────────────────────────────────────────

void BleGattServer::dispatchAtt(int fd, const uint8_t* p, size_t n) {
    if (n < 1) return;
    switch (p[0]) {
    case ATT_OP_MTU_REQ:           attMtuReq     (fd, p, n); break;
    case ATT_OP_FIND_INFO_REQ:     attFindInfo   (fd, p, n); break;
    case ATT_OP_READ_BY_TYPE_REQ:  attReadByType (fd, p, n); break;
    case ATT_OP_READ_BY_GROUP_REQ: attReadByGroup(fd, p, n); break;
    case ATT_OP_READ_REQ:          attRead       (fd, p, n); break;
    case ATT_OP_WRITE_REQ:         attWrite      (fd, p, n, true);  break;
    case ATT_OP_WRITE_CMD:         attWrite      (fd, p, n, false); break;
    case ATT_OP_HANDLE_NOTIFY:     break; // client notification (ignored)
    default:
        sendError(fd, p[0], 0x0000, ATT_ERR_NOT_SUPPORTED);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  ATT handlers
// ─────────────────────────────────────────────────────────────────────────────

void BleGattServer::attMtuReq(int fd, const uint8_t* p, size_t n) {
    if (n < 3) { sendError(fd, ATT_OP_MTU_REQ, 0, ATT_ERR_INVALID_HANDLE); return; }
    uint16_t clientMtu = get16(p + 1);
    uint16_t serverMtu = 512;
    m_mtu = std::min(clientMtu, serverMtu);
    if (m_mtu < 23) m_mtu = 23;

    std::vector<uint8_t> resp = { ATT_OP_MTU_RESP };
    put16(resp, m_mtu);
    sendPdu(fd, resp);
}

void BleGattServer::attFindInfo(int fd, const uint8_t* p, size_t n) {
    if (n < 5) { sendError(fd, ATT_OP_FIND_INFO_REQ, 0, ATT_ERR_INVALID_HANDLE); return; }
    uint16_t startHdl = get16(p + 1);
    uint16_t endHdl   = get16(p + 3);

    // All our attributes use 16-bit UUIDs
    struct { uint16_t handle; uint16_t uuid; }
    table[] = {
        { HDL_SERVICE,    UUID_PRIMARY_SVC },
        { HDL_STATE_DECL, UUID_CHAR_DECL   },
        { HDL_STATE_VAL,  UUID_WIFI_STATE  },
        { HDL_CCCD,       UUID_CCCD        },
        { HDL_CMD_DECL,   UUID_CHAR_DECL   },
        { HDL_CMD_VAL,    UUID_WIFI_CMD    },
    };

    std::vector<uint8_t> resp = { ATT_OP_FIND_INFO_RESP, 0x01 }; // format=1 (16-bit UUIDs)
    bool found = false;
    for (auto& e : table) {
        if (e.handle >= startHdl && e.handle <= endHdl) {
            put16(resp, e.handle);
            put16(resp, e.uuid);
            found = true;
        }
    }

    if (!found)
        sendError(fd, ATT_OP_FIND_INFO_REQ, startHdl, ATT_ERR_NOT_FOUND);
    else
        sendPdu(fd, resp);
}

void BleGattServer::attReadByType(int fd, const uint8_t* p, size_t n) {
    if (n < 7) { sendError(fd, ATT_OP_READ_BY_TYPE_REQ, 0, ATT_ERR_INVALID_HANDLE); return; }
    uint16_t startHdl = get16(p + 1);
    uint16_t endHdl   = get16(p + 3);
    uint16_t uuid     = get16(p + 5);

    if (uuid == UUID_CHAR_DECL) {
        // Characteristic declarations
        // Each entry: handle(2) + props(1) + value_handle(2) + uuid(2) = 7 bytes
        std::vector<uint8_t> resp = { ATT_OP_READ_BY_TYPE_RESP, 7 };
        bool found = false;

        if (HDL_STATE_DECL >= startHdl && HDL_STATE_DECL <= endHdl) {
            put16(resp, HDL_STATE_DECL);
            resp.push_back(PROP_READ | PROP_NOTIFY);
            put16(resp, HDL_STATE_VAL);
            put16(resp, UUID_WIFI_STATE);
            found = true;
        }
        if (HDL_CMD_DECL >= startHdl && HDL_CMD_DECL <= endHdl) {
            put16(resp, HDL_CMD_DECL);
            resp.push_back(PROP_WRITE_NR | PROP_WRITE);
            put16(resp, HDL_CMD_VAL);
            put16(resp, UUID_WIFI_CMD);
            found = true;
        }

        if (!found)
            sendError(fd, ATT_OP_READ_BY_TYPE_REQ, startHdl, ATT_ERR_NOT_FOUND);
        else
            sendPdu(fd, resp);
    } else {
        sendError(fd, ATT_OP_READ_BY_TYPE_REQ, startHdl, ATT_ERR_NOT_FOUND);
    }
}

void BleGattServer::attReadByGroup(int fd, const uint8_t* p, size_t n) {
    if (n < 7) { sendError(fd, ATT_OP_READ_BY_GROUP_REQ, 0, ATT_ERR_INVALID_HANDLE); return; }
    uint16_t startHdl = get16(p + 1);
    uint16_t endHdl   = get16(p + 3);
    uint16_t uuid     = get16(p + 5);

    if (uuid == UUID_PRIMARY_SVC && startHdl <= HDL_SERVICE && endHdl >= HDL_SERVICE) {
        // length = 2 (start) + 2 (end) + 2 (uuid) = 6
        std::vector<uint8_t> resp = { ATT_OP_READ_BY_GROUP_RESP, 6 };
        put16(resp, HDL_SERVICE);
        put16(resp, HDL_LAST);
        put16(resp, UUID_WIFI_SVC);
        sendPdu(fd, resp);
    } else {
        sendError(fd, ATT_OP_READ_BY_GROUP_REQ, startHdl, ATT_ERR_NOT_FOUND);
    }
}

void BleGattServer::attRead(int fd, const uint8_t* p, size_t n) {
    if (n < 3) { sendError(fd, ATT_OP_READ_REQ, 0, ATT_ERR_INVALID_HANDLE); return; }
    uint16_t handle = get16(p + 1);

    std::vector<uint8_t> resp = { ATT_OP_READ_RESP };

    if (handle == HDL_STATE_VAL) {
        auto val = wifiStateBytes();
        // Truncate to negotiated MTU - 1
        if (val.size() > static_cast<size_t>(m_mtu - 1))
            val.resize(m_mtu - 1);
        resp.insert(resp.end(), val.begin(), val.end());
    } else if (handle == HDL_CCCD) {
        put16(resp, m_cccd);
    } else if (handle == HDL_SERVICE) {
        put16(resp, UUID_WIFI_SVC);
    } else if (handle == HDL_CMD_VAL) {
        sendError(fd, ATT_OP_READ_REQ, handle, ATT_ERR_READ_NOT_PERM);
        return;
    } else if (handle == 0 || handle > HDL_LAST) {
        sendError(fd, ATT_OP_READ_REQ, handle, ATT_ERR_INVALID_HANDLE);
        return;
    } else {
        sendError(fd, ATT_OP_READ_REQ, handle, ATT_ERR_NOT_FOUND);
        return;
    }
    sendPdu(fd, resp);
}

void BleGattServer::attWrite(int fd, const uint8_t* p, size_t n, bool withResp) {
    if (n < 3) {
        if (withResp) sendError(fd, ATT_OP_WRITE_REQ, 0, ATT_ERR_INVALID_HANDLE);
        return;
    }
    uint16_t handle = get16(p + 1);

    if (handle == HDL_CCCD) {
        // Client enabling/disabling notifications
        if (n >= 5) {
            std::lock_guard<std::mutex> lk(m_clientMu);
            m_cccd = get16(p + 3);
            printf("[ble] CCCD set to 0x%04X (%s)\n", m_cccd,
                   (m_cccd & 0x0001) ? "notify ON" : "notify OFF");
        }
        if (withResp) sendPdu(fd, { ATT_OP_WRITE_RESP });
        return;
    }

    if (handle == HDL_CMD_VAL) {
        // WiFi command — parse JSON and dispatch to WifiManager
        if (n > 3 && m_wifi) {
            std::string json(reinterpret_cast<const char*>(p + 3), n - 3);
            printf("[ble] WiFi cmd: %s\n", json.c_str());

            // Minimal JSON field extractor
            auto jstr = [&](const std::string& key) -> std::string {
                auto k = "\"" + key + "\"";
                auto pos = json.find(k);
                if (pos == std::string::npos) return {};
                pos = json.find('"', pos + k.size() + 1);
                if (pos == std::string::npos) return {};
                auto end = json.find('"', pos + 1);
                if (end == std::string::npos) return {};
                return json.substr(pos + 1, end - pos - 1);
            };

            std::string cmd = jstr("cmd");
            std::string err;
            if (cmd == "start_ap") {
                err = m_wifi->resetToAP();
            } else if (cmd == "connect") {
                err = m_wifi->setStation(jstr("ssid"), jstr("password"));
            } else if (cmd == "shutdown") {
                err = m_wifi->shutdown();
            }

            if (!err.empty())
                fprintf(stderr, "[ble] WiFi error: %s\n", err.c_str());

            // Notify the client of the new state
            notifyStateChanged();
        }
        if (withResp) sendPdu(fd, { ATT_OP_WRITE_RESP });
        return;
    }

    if (withResp) {
        uint8_t ecode = (handle == 0 || handle > HDL_LAST)
                        ? ATT_ERR_INVALID_HANDLE
                        : ATT_ERR_WRITE_NOT_PERM;
        sendError(fd, ATT_OP_WRITE_REQ, handle, ecode);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Response helpers
// ─────────────────────────────────────────────────────────────────────────────

void BleGattServer::sendPdu(int fd, const std::vector<uint8_t>& pdu) {
    if (fd < 0 || pdu.empty()) return;
    ::send(fd, pdu.data(), pdu.size(), MSG_NOSIGNAL);
}

void BleGattServer::sendError(int fd, uint8_t reqOp, uint16_t handle, uint8_t ecode) {
    std::vector<uint8_t> err = { ATT_OP_ERROR, reqOp };
    put16(err, handle);
    err.push_back(ecode);
    sendPdu(fd, err);
}

void BleGattServer::sendNotify(int fd, uint16_t handle,
                               const std::vector<uint8_t>& val) {
    std::vector<uint8_t> pdu = { ATT_OP_HANDLE_NOTIFY };
    put16(pdu, handle);
    pdu.insert(pdu.end(), val.begin(), val.end());
    // Truncate to MTU
    if (pdu.size() > static_cast<size_t>(m_mtu))
        pdu.resize(m_mtu);
    sendPdu(fd, pdu);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Attribute value builders
// ─────────────────────────────────────────────────────────────────────────────

std::vector<uint8_t> BleGattServer::wifiStateBytes() const {
    std::string json;
    if (m_wifi) {
        WifiStatus ws = m_wifi->getStatus();
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"mode\":\"%s\",\"ssid\":\"%s\",\"connected\":%s,\"ip\":\"%s\"}",
            ws.mode.c_str(), ws.ssid.c_str(),
            ws.connected ? "true" : "false",
            ws.ip.c_str());
        json = buf;
    } else {
        json = "{\"mode\":\"unknown\"}";
    }
    return std::vector<uint8_t>(json.begin(), json.end());
}

// ─────────────────────────────────────────────────────────────────────────────
//  notifyStateChanged  — send GATT notification to connected client
// ─────────────────────────────────────────────────────────────────────────────

void BleGattServer::notifyStateChanged() {
    std::lock_guard<std::mutex> lk(m_clientMu);
    if (m_clientFd < 0) return;
    if (!(m_cccd & 0x0001)) return;  // notifications not enabled by client
    auto val = wifiStateBytes();
    sendNotify(m_clientFd, HDL_STATE_VAL, val);
}
