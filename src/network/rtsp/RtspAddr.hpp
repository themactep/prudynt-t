#pragma once

// Address family used by the RTSP server and SDP generator. Prudynt-t is
// single-stack here, not dual-stack: build with RTSP_IPV6 defined for an
// IPv6-only server (socket(AF_INET6, ...), "IN IP6" in SDP origin lines),
// or leave it undefined for the default IPv4-only server. Everything that
// touches a socket address goes through the aliases below instead of
// hardcoding AF_INET/AF_INET6 or sockaddr_in/sockaddr_in6, so the two
// modes share one code path.

#include <arpa/inet.h>
#include <cstddef>
#include <cstdint>
#include <netinet/in.h>

namespace simple_rtsp {

#ifdef RTSP_IPV6

using SockAddr = struct sockaddr_in6;
constexpr int kAddrFamily = AF_INET6;
constexpr size_t kAddrStrLen = INET6_ADDRSTRLEN;
constexpr const char *kAnyAddrStr = "::";
constexpr const char *kSdpAddrType = "IP6";

inline void initAnyAddr(SockAddr &addr, uint16_t port) {
    addr = SockAddr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr   = in6addr_any;
    addr.sin6_port   = htons(port);
}

inline uint16_t addrPort(const SockAddr &addr) { return ntohs(addr.sin6_port); }
inline void setAddrPort(SockAddr &addr, uint16_t port) { addr.sin6_port = htons(port); }

inline const char *addrToStr(const SockAddr &addr, char *buf, size_t len) {
    return inet_ntop(AF_INET6, &addr.sin6_addr, buf, len);
}

#else

using SockAddr = struct sockaddr_in;
constexpr int kAddrFamily = AF_INET;
constexpr size_t kAddrStrLen = INET_ADDRSTRLEN;
constexpr const char *kAnyAddrStr = "0.0.0.0";
constexpr const char *kSdpAddrType = "IP4";

inline void initAnyAddr(SockAddr &addr, uint16_t port) {
    addr = SockAddr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);
}

inline uint16_t addrPort(const SockAddr &addr) { return ntohs(addr.sin_port); }
inline void setAddrPort(SockAddr &addr, uint16_t port) { addr.sin_port = htons(port); }

inline const char *addrToStr(const SockAddr &addr, char *buf, size_t len) {
    return inet_ntop(AF_INET, &addr.sin_addr, buf, len);
}

#endif

} // namespace simple_rtsp
