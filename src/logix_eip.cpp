#include "logix_eip.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <sstream>
#include <thread>

#ifdef _WIN32
namespace {
struct WinsockInit {
    WinsockInit() { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); }
    ~WinsockInit() { WSACleanup(); }
};
static WinsockInit s_winsock_init;
}
#endif

namespace logix {

// ─── Utility helpers ────────────────────────────────────────────────────────

static void appendU8(std::vector<uint8_t>& v, uint8_t val) { v.push_back(val); }

static void appendU16(std::vector<uint8_t>& v, uint16_t val) {
    v.push_back(val & 0xFF);
    v.push_back((val >> 8) & 0xFF);
}

static void appendU32(std::vector<uint8_t>& v, uint32_t val) {
    v.push_back(val & 0xFF);
    v.push_back((val >> 8) & 0xFF);
    v.push_back((val >> 16) & 0xFF);
    v.push_back((val >> 24) & 0xFF);
}

static uint16_t readU16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
static uint32_t readU32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

static uint16_t randomU16() {
    static std::mt19937 rng(std::random_device{}());
    return std::uniform_int_distribution<uint16_t>(1, 0xFFFF)(rng);
}

// ─── Free functions ─────────────────────────────────────────────────────────

std::vector<uint8_t> buildCipPath(uint16_t cip_class, int instance) {
    std::vector<uint8_t> path;
    if (cip_class <= 0xFF) {
        appendU8(path, 0x20);
        appendU8(path, static_cast<uint8_t>(cip_class));
    } else {
        appendU8(path, 0x21);
        appendU8(path, 0x00);
        appendU16(path, cip_class);
    }
    if (instance >= 0) {
        if (instance <= 0xFF) {
            appendU8(path, 0x24);
            appendU8(path, static_cast<uint8_t>(instance));
        } else {
            appendU8(path, 0x25);
            appendU8(path, 0x00);
            appendU16(path, static_cast<uint16_t>(instance));
        }
    }
    return path;
}

std::vector<uint8_t> buildSymbolicSegment(const std::string& tag_name) {
    std::vector<uint8_t> seg;
    std::istringstream ss(tag_name);
    std::string part;
    while (std::getline(ss, part, '.')) {
        // Check for array index: "TagName[index]"
        std::string sym_name = part;
        int array_index = -1;
        auto bracket_pos = part.find('[');
        if (bracket_pos != std::string::npos) {
            sym_name = part.substr(0, bracket_pos);
            auto end_bracket = part.find(']', bracket_pos);
            if (end_bracket != std::string::npos) {
                array_index = std::stoi(part.substr(bracket_pos + 1, end_bracket - bracket_pos - 1));
            }
        }

        // Symbolic segment for the name
        appendU8(seg, 0x91);
        appendU8(seg, static_cast<uint8_t>(sym_name.size()));
        for (char c : sym_name) seg.push_back(static_cast<uint8_t>(c));
        if (sym_name.size() % 2) seg.push_back(0x00);

        // Element segment for array index
        if (array_index >= 0) {
            if (array_index <= 0xFF) {
                appendU8(seg, 0x28);
                appendU8(seg, static_cast<uint8_t>(array_index));
            } else {
                appendU8(seg, 0x29);
                appendU8(seg, 0x00);
                appendU16(seg, static_cast<uint16_t>(array_index));
            }
        }
    }
    return seg;
}

CipResponse parseCipStatus(const std::vector<uint8_t>& resp) {
    CipResponse r;
    if (resp.size() < 4) return r;
    r.status = resp[2];
    uint8_t ext_size = resp[3];
    size_t data_offset = 4;
    if (ext_size > 0 && 4 + ext_size * 2 <= resp.size()) {
        r.ext_status = readU16(&resp[4]);
        data_offset = 4 + ext_size * 2;
    }
    if (data_offset < resp.size()) {
        r.data.assign(resp.begin() + data_offset, resp.end());
    }
    return r;
}

std::string cipTypeName(uint16_t type_code) {
    switch (type_code) {
        case kTypeBOOL:  return "BOOL";
        case kTypeSINT:  return "SINT";
        case kTypeINT:   return "INT";
        case kTypeDINT:  return "DINT";
        case kTypeLINT:  return "LINT";
        case kTypeUSINT: return "USINT";
        case kTypeUINT:  return "UINT";
        case kTypeUDINT: return "UDINT";
        case kTypeLWORD: return "LWORD";
        case kTypeREAL:  return "REAL";
        case kTypeLREAL: return "LREAL";
        case 0xA0: return "STRUCT";
        case 0xDA: return "STRING";
        default: return "UNKNOWN";
    }
}

bool isNumericType(uint16_t type_code) {
    switch (type_code) {
        case kTypeBOOL: case kTypeSINT: case kTypeINT: case kTypeDINT:
        case kTypeLINT: case kTypeUSINT: case kTypeUINT: case kTypeUDINT:
        case kTypeREAL: case kTypeLREAL:
            return true;
        default:
            return false;
    }
}

int cipTypeSize(uint16_t type_code) {
    switch (type_code) {
        case kTypeBOOL: case kTypeSINT: case kTypeUSINT: return 1;
        case kTypeINT: case kTypeUINT: return 2;
        case kTypeDINT: case kTypeUDINT: case kTypeREAL: return 4;
        case kTypeLINT: case kTypeLREAL: case kTypeLWORD: return 8;
        default: return 0;
    }
}

// ─── EipConnection ──────────────────────────────────────────────────────────

std::vector<std::pair<int, std::string>> EipConnection::parseRouteString(const std::string& route_str) {
    std::vector<std::pair<int, std::string>> route;
    if (route_str.empty()) return route;

    std::istringstream ss(route_str);
    std::string token;
    std::vector<std::string> tokens;
    while (std::getline(ss, token, ',')) {
        // Trim whitespace
        size_t start = token.find_first_not_of(" \t");
        size_t end = token.find_last_not_of(" \t");
        if (start != std::string::npos) {
            tokens.push_back(token.substr(start, end - start + 1));
        }
    }

    // Tokens come in pairs: port, link, port, link, ...
    if (tokens.size() % 2 != 0) {
        throw std::runtime_error("Invalid route: must have even number of comma-separated values (port,link pairs)");
    }

    for (size_t i = 0; i < tokens.size(); i += 2) {
        int port = std::stoi(tokens[i]);
        route.push_back({port, tokens[i + 1]});
    }

    return route;
}

std::vector<uint8_t> EipConnection::buildRoutePath() const {
    std::vector<uint8_t> path;
    for (const auto& [port, link] : route_) {
        // Check if link is a numeric slot or an IP/string address
        bool is_numeric = true;
        for (char c : link) {
            if (!std::isdigit(c)) { is_numeric = false; break; }
        }

        if (is_numeric) {
            // Port segment with numeric link (slot)
            appendU8(path, static_cast<uint8_t>(port));
            appendU8(path, static_cast<uint8_t>(std::stoi(link)));
        } else {
            // Port segment with extended link (IP address)
            appendU8(path, static_cast<uint8_t>(port) | 0x10);
            appendU8(path, static_cast<uint8_t>(link.size()));
            for (char c : link) {
                path.push_back(static_cast<uint8_t>(c));
            }
            // Pad to even length
            if (path.size() % 2) {
                path.push_back(0x00);
            }
        }
    }
    return path;
}

EipConnection::~EipConnection() {
    try { close(); } catch (...) {}
}

void EipConnection::connect(const std::string& ip,
                             const std::vector<std::pair<int, std::string>>& route,
                             double timeout_s) {
    close();
    route_ = route;

    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ == kInvalidSocket) {
        throw std::runtime_error("Failed to create socket");
    }

    // Set timeout
    struct timeval tv;
    tv.tv_sec = static_cast<long>(timeout_s);
    tv.tv_usec = static_cast<long>((timeout_s - tv.tv_sec) * 1e6);
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    // Disable Nagle
    int flag = 1;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kEnipPort);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("Invalid IP address: " + ip);
    }

    if (::connect(sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        throw std::runtime_error("Failed to connect to " + ip + ":" + std::to_string(kEnipPort));
    }

    registerSession();
    forwardOpen();
    connected_ = true;
}

void EipConnection::registerSession() {
    std::vector<uint8_t> data;
    appendU16(data, 1); // protocol version
    appendU16(data, 0); // options
    auto pkt = buildEnipHeader(kRegisterSession, 0, data);

    send(sock_, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0);
    auto resp = recvEnip();
    if (resp.size() < 24) throw std::runtime_error("RegisterSession response too short");
    uint32_t status = readU32(&resp[8]);
    if (status != 0) throw std::runtime_error("RegisterSession failed");
    session_ = readU32(&resp[4]);
}

void EipConnection::forwardOpen() {
    uint32_t ot_id = 0x80000000 | randomU16();
    uint32_t to_id = 0x803F0000 | randomU16();
    conn_serial_ = randomU16();

    // Connection path: route segments + message router (class 2, instance 1)
    auto route_path = buildRoutePath();
    std::vector<uint8_t> conn_path;
    conn_path.insert(conn_path.end(), route_path.begin(), route_path.end());
    conn_path.insert(conn_path.end(), {0x20, 0x02, 0x24, 0x01});
    uint8_t conn_path_words = static_cast<uint8_t>(conn_path.size() / 2);

    // Forward Open service
    std::vector<uint8_t> fo;
    appendU8(fo, 0x54); // Forward Open service
    appendU8(fo, 0x02); // path size (words)
    appendU8(fo, 0x20); appendU8(fo, 0x06); // class 6
    appendU8(fo, 0x24); appendU8(fo, 0x01); // instance 1
    appendU8(fo, 0x0A); appendU8(fo, 0x0E); // priority/tick, timeout ticks
    appendU32(fo, ot_id);
    appendU32(fo, to_id);
    appendU16(fo, conn_serial_);
    appendU16(fo, 0x1234); // vendor ID
    appendU32(fo, 0x00000001); // originator serial
    appendU8(fo, 0x03); // timeout multiplier
    fo.push_back(0); fo.push_back(0); fo.push_back(0); // reserved
    appendU32(fo, 0x00201340); // O->T RPI
    appendU16(fo, 0x43F4);     // O->T params (500 bytes, class 3)
    appendU32(fo, 0x00201340); // T->O RPI
    appendU16(fo, 0x43F4);     // T->O params
    appendU8(fo, 0xA3);        // transport trigger
    appendU8(fo, conn_path_words);
    fo.insert(fo.end(), conn_path.begin(), conn_path.end());

    // Wrap in SendRRData
    std::vector<uint8_t> payload;
    appendU32(payload, 0); // interface
    appendU16(payload, 0); // timeout
    appendU16(payload, 2); // item count
    appendU16(payload, 0x00); appendU16(payload, 0); // Null Address
    appendU16(payload, 0xB2); appendU16(payload, static_cast<uint16_t>(fo.size()));
    payload.insert(payload.end(), fo.begin(), fo.end());

    auto pkt = buildEnipHeader(kSendRRData, session_, payload);
    send(sock_, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0);

    auto resp = recvEnip();
    if (resp.size() < 24) throw std::runtime_error("Forward Open response too short");
    uint32_t status = readU32(&resp[8]);
    if (status != 0) throw std::runtime_error("Forward Open ENIP failed");

    auto enip_payload = std::vector<uint8_t>(resp.begin() + 24, resp.end());
    auto cip_resp = parseSendRRResponse(enip_payload);

    if (cip_resp.size() < 4 || cip_resp[2] != 0) {
        uint8_t cip_status = cip_resp.size() > 2 ? cip_resp[2] : 0xFF;
        throw std::runtime_error("Forward Open CIP failed: 0x" +
            std::to_string(cip_status));
    }

    ot_connection_id_ = readU32(&cip_resp[4]);
    to_connection_id_ = readU32(&cip_resp[8]);

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
}

std::vector<uint8_t> EipConnection::sendCip(const std::vector<uint8_t>& cip_message) {
    if (!connected_) throw std::runtime_error("Not connected");

    sequence_++;
    std::vector<uint8_t> cip_with_seq;
    appendU16(cip_with_seq, sequence_);
    cip_with_seq.insert(cip_with_seq.end(), cip_message.begin(), cip_message.end());

    std::vector<uint8_t> payload;
    appendU32(payload, 0); // interface
    appendU16(payload, 0); // timeout
    appendU16(payload, 2); // item count
    appendU16(payload, 0xA1); appendU16(payload, 4);
    appendU32(payload, ot_connection_id_);
    appendU16(payload, 0xB1);
    appendU16(payload, static_cast<uint16_t>(cip_with_seq.size()));
    payload.insert(payload.end(), cip_with_seq.begin(), cip_with_seq.end());

    auto pkt = buildEnipHeader(kSendUnitData, session_, payload);
    send(sock_, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0);

    auto resp = recvEnip();
    if (resp.size() < 24) throw std::runtime_error("SendUnitData response too short");
    uint32_t status = readU32(&resp[8]);
    if (status != 0) throw std::runtime_error("SendUnitData ENIP failed");

    auto enip_payload = std::vector<uint8_t>(resp.begin() + 24, resp.end());
    return parseSendUnitResponse(enip_payload);
}

std::vector<uint8_t> EipConnection::sendUnconnected(const std::vector<uint8_t>& cip_message) {
    if (sock_ == kInvalidSocket) throw std::runtime_error("Not connected");

    std::vector<uint8_t> frame;

    if (route_.empty()) {
        // Direct connection: no unconnected send wrapper needed, just raw CIP
        frame = cip_message;
    } else {
        // Build unconnected send wrapper with route
        auto route_path = buildRoutePath();

        appendU8(frame, 0x52); // Unconnected Send service
        appendU8(frame, 0x02); // path size (words) for CM
        appendU8(frame, 0x20); appendU8(frame, 0x06); // class 6
        appendU8(frame, 0x24); appendU8(frame, 0x01); // instance 1
        appendU8(frame, 0x0A); appendU8(frame, 0x0E); // priority/tick, timeout
        appendU16(frame, static_cast<uint16_t>(cip_message.size()));
        frame.insert(frame.end(), cip_message.begin(), cip_message.end());
        if (cip_message.size() % 2) frame.push_back(0x00); // pad
        appendU8(frame, static_cast<uint8_t>(route_path.size() / 2));
        appendU8(frame, 0x00); // reserved
        frame.insert(frame.end(), route_path.begin(), route_path.end());
    }

    // Wrap in SendRRData
    std::vector<uint8_t> payload;
    appendU32(payload, 0); // interface
    appendU16(payload, 0); // timeout
    appendU16(payload, 2); // item count
    appendU16(payload, 0x00); appendU16(payload, 0); // Null Address
    appendU16(payload, 0xB2); appendU16(payload, static_cast<uint16_t>(frame.size()));
    payload.insert(payload.end(), frame.begin(), frame.end());

    auto pkt = buildEnipHeader(kSendRRData, session_, payload);
    send(sock_, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0);

    auto resp = recvEnip();
    if (resp.size() < 24) throw std::runtime_error("SendRRData response too short");
    uint32_t status = readU32(&resp[8]);
    if (status != 0) throw std::runtime_error("SendRRData ENIP failed");

    auto enip_payload = std::vector<uint8_t>(resp.begin() + 24, resp.end());
    return parseSendRRResponse(enip_payload);
}

void EipConnection::forwardClose() {
    if (ot_connection_id_ == 0) return;

    auto route_path = buildRoutePath();
    std::vector<uint8_t> conn_path;
    conn_path.insert(conn_path.end(), route_path.begin(), route_path.end());
    conn_path.insert(conn_path.end(), {0x20, 0x02, 0x24, 0x01});

    std::vector<uint8_t> fc;
    appendU8(fc, 0x4E); // Forward Close service
    appendU8(fc, 0x02); // path size
    appendU8(fc, 0x20); appendU8(fc, 0x06);
    appendU8(fc, 0x24); appendU8(fc, 0x01);
    appendU8(fc, 0x0A); appendU8(fc, 0x0E);
    appendU16(fc, conn_serial_);
    appendU16(fc, 0x1234);
    appendU32(fc, 0x00000001);
    appendU8(fc, static_cast<uint8_t>(conn_path.size() / 2));
    appendU8(fc, 0x00);
    fc.insert(fc.end(), conn_path.begin(), conn_path.end());

    std::vector<uint8_t> payload;
    appendU32(payload, 0);
    appendU16(payload, 0);
    appendU16(payload, 2);
    appendU16(payload, 0x00); appendU16(payload, 0);
    appendU16(payload, 0xB2); appendU16(payload, static_cast<uint16_t>(fc.size()));
    payload.insert(payload.end(), fc.begin(), fc.end());

    auto pkt = buildEnipHeader(kSendRRData, session_, payload);
    send(sock_, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0);

    try { recvEnip(); } catch (...) {}
    ot_connection_id_ = 0;
}

void EipConnection::close() {
    if (sock_ == kInvalidSocket) return;

    connected_ = false;

    try { forwardClose(); } catch (...) {}

    // Unregister session
    auto pkt = buildEnipHeader(kUnregisterSession, session_, {});
    try { send(sock_, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0); } catch (...) {}

#ifdef _WIN32
    closesocket(sock_);
#else
    ::close(sock_);
#endif
    sock_ = kInvalidSocket;
    session_ = 0;
    sequence_ = 0;
}

std::vector<uint8_t> EipConnection::buildEnipHeader(uint16_t command, uint32_t session,
                                                      const std::vector<uint8_t>& data) {
    std::vector<uint8_t> hdr;
    appendU16(hdr, command);
    appendU16(hdr, static_cast<uint16_t>(data.size()));
    appendU32(hdr, session);
    appendU32(hdr, 0); // status
    // sender context (8 bytes)
    for (int i = 0; i < 8; i++) hdr.push_back(0);
    appendU32(hdr, 0); // options
    hdr.insert(hdr.end(), data.begin(), data.end());
    return hdr;
}

std::vector<uint8_t> EipConnection::recvExact(size_t count) {
    std::vector<uint8_t> buf(count);
    size_t received = 0;
    while (received < count) {
        ssize_t n = recv(sock_, reinterpret_cast<char*>(buf.data() + received),
                         static_cast<int>(count - received), 0);
        if (n <= 0) throw std::runtime_error("Connection closed or recv error");
        received += n;
    }
    return buf;
}

std::vector<uint8_t> EipConnection::recvEnip() {
    auto header = recvExact(24);
    uint16_t length = readU16(&header[2]);
    if (length > 0) {
        auto payload = recvExact(length);
        header.insert(header.end(), payload.begin(), payload.end());
    }
    return header;
}

std::vector<uint8_t> EipConnection::parseSendRRResponse(const std::vector<uint8_t>& payload) {
    if (payload.size() < 8) throw std::runtime_error("SendRRData payload too short");
    uint16_t count = readU16(&payload[6]);
    size_t offset = 8;
    for (uint16_t i = 0; i < count; i++) {
        if (offset + 4 > payload.size()) break;
        uint16_t type_id = readU16(&payload[offset]);
        uint16_t length = readU16(&payload[offset + 2]);
        offset += 4;
        if (type_id == 0xB2) {
            return std::vector<uint8_t>(payload.begin() + offset,
                                         payload.begin() + offset + length);
        }
        offset += length;
    }
    throw std::runtime_error("No Unconnected Data Item in response");
}

std::vector<uint8_t> EipConnection::parseSendUnitResponse(const std::vector<uint8_t>& payload) {
    if (payload.size() < 8) throw std::runtime_error("SendUnitData payload too short");
    uint16_t count = readU16(&payload[6]);
    size_t offset = 8;
    for (uint16_t i = 0; i < count; i++) {
        if (offset + 4 > payload.size()) break;
        uint16_t type_id = readU16(&payload[offset]);
        uint16_t length = readU16(&payload[offset + 2]);
        offset += 4;
        if (type_id == 0xB1 && length >= 2) {
            // Skip sequence number (2 bytes)
            return std::vector<uint8_t>(payload.begin() + offset + 2,
                                         payload.begin() + offset + length);
        }
        offset += length;
    }
    return {};
}

} // namespace logix
