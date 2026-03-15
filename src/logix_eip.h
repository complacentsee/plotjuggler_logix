/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketType = SOCKET;
using ssize_t = int;
#ifndef _SOCKLEN_T_DEFINED
using socklen_t = int;
#define _SOCKLEN_T_DEFINED
#endif
constexpr SocketType kInvalidSocket = INVALID_SOCKET;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <fcntl.h>
using SocketType = int;
constexpr SocketType kInvalidSocket = -1;
#endif

namespace logix
{

// EtherNet/IP constants
constexpr uint16_t kEnipPort = 44818;
constexpr uint16_t kRegisterSession = 0x0065;
constexpr uint16_t kUnregisterSession = 0x0066;
constexpr uint16_t kSendRRData = 0x006F;
constexpr uint16_t kSendUnitData = 0x0070;

// CIP data type codes
constexpr uint16_t kTypeBOOL = 0xC1;
constexpr uint16_t kTypeSINT = 0xC2;
constexpr uint16_t kTypeINT = 0xC3;
constexpr uint16_t kTypeDINT = 0xC4;
constexpr uint16_t kTypeLINT = 0xC5;
constexpr uint16_t kTypeUSINT = 0xC6;
constexpr uint16_t kTypeUINT = 0xC7;
constexpr uint16_t kTypeUDINT = 0xC8;
constexpr uint16_t kTypeLWORD = 0xC9;
constexpr uint16_t kTypeREAL = 0xCA;
constexpr uint16_t kTypeLREAL = 0xCB;

struct CipResponse
{
  uint8_t status = 0xFF;
  uint16_t ext_status = 0;
  std::vector<uint8_t> data;
};

// Build EPATH bytes for class[/instance]
std::vector<uint8_t> buildCipPath(uint16_t cip_class, int instance = -1);

// Build ANSI Extended Symbol segment for tag access
std::vector<uint8_t> buildSymbolicSegment(const std::string& tag_name);

// Parse CIP response header
CipResponse parseCipStatus(const std::vector<uint8_t>& resp);

// Get human-readable name for a CIP data type code
std::string cipTypeName(uint16_t type_code);

// Check if a CIP type code is numeric (trendable)
bool isNumericType(uint16_t type_code);

// Get byte size of a CIP type
int cipTypeSize(uint16_t type_code);

/// EtherNet/IP session + CIP Forward Open connected transport
class EipConnection
{
public:
  EipConnection() = default;
  ~EipConnection();

  EipConnection(const EipConnection&) = delete;
  EipConnection& operator=(const EipConnection&) = delete;

  /// Parse a comma-separated route string into port/link pairs.
  /// Format: "port,link,port,link,..." where link is an integer (slot) or IP string.
  /// Examples: "1,0" (backplane port 1, slot 0), "1,4,2,10.10.10.9"
  /// Empty string means direct connection (no route).
  static std::vector<std::pair<int, std::string>> parseRouteString(const std::string& route_str);

  /// Establish TCP connection, register session, and Forward Open.
  /// route: parsed port/link pairs (empty = direct connection, no routing)
  void connect(const std::string& ip, const std::vector<std::pair<int, std::string>>& route = {},
               double timeout_s = 5.0);

  /// Send CIP message on connected transport, return raw CIP response
  std::vector<uint8_t> sendCip(const std::vector<uint8_t>& cip_message);

  /// Send unconnected CIP message via SendRRData
  std::vector<uint8_t> sendUnconnected(const std::vector<uint8_t>& cip_message);

  /// Forward Close, unregister session, close socket
  void close();

  bool isConnected() const
  {
    return connected_;
  }
  uint32_t connectionSize() const
  {
    return connection_size_;
  }

private:
  void registerSession();
  void forwardOpen();
  void forwardClose();

  // Build CIP port segment bytes from route_
  std::vector<uint8_t> buildRoutePath() const;

  std::vector<uint8_t> buildEnipHeader(uint16_t command, uint32_t session,
                                       const std::vector<uint8_t>& data);
  std::vector<uint8_t> recvEnip();
  std::vector<uint8_t> recvExact(size_t count);

  // Parse SendRRData response to extract unconnected data item
  std::vector<uint8_t> parseSendRRResponse(const std::vector<uint8_t>& payload);
  // Parse SendUnitData response
  std::vector<uint8_t> parseSendUnitResponse(const std::vector<uint8_t>& payload);

  SocketType sock_ = kInvalidSocket;
  double timeout_s_ = 5.0;
  uint32_t session_ = 0;
  uint32_t ot_connection_id_ = 0;
  uint32_t to_connection_id_ = 0;
  uint16_t conn_serial_ = 0;
  uint16_t sequence_ = 0;
  std::vector<std::pair<int, std::string>> route_;
  bool connected_ = false;
  uint32_t connection_size_ = 504;
};

}  // namespace logix
