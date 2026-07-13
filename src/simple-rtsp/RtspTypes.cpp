#include "RtspTypes.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h>
#include <sys/time.h>

namespace simple_rtsp {

// ── Method / Status helpers ─────────────────────────────────────────────────

const char *methodToString(Method m) {
    switch (m) {
    case Method::OPTIONS:       return "OPTIONS";
    case Method::DESCRIBE:      return "DESCRIBE";
    case Method::SETUP:         return "SETUP";
    case Method::PLAY:          return "PLAY";
    case Method::PAUSE:         return "PAUSE";
    case Method::TEARDOWN:      return "TEARDOWN";
    case Method::GET_PARAMETER: return "GET_PARAMETER";
    case Method::SET_PARAMETER: return "SET_PARAMETER";
    case Method::ANNOUNCE:      return "ANNOUNCE";
    case Method::RECORD:        return "RECORD";
    default:                    return "UNKNOWN";
    }
}

const char *statusToString(Status s) {
    switch (s) {
    case Status::OK:                  return "OK";
    case Status::BAD_REQUEST:          return "Bad Request";
    case Status::UNAUTHORIZED:        return "Unauthorized";
    case Status::NOT_FOUND:           return "Not Found";
    case Status::METHOD_NOT_ALLOWED:  return "Method Not Allowed";
    case Status::SESSION_NOT_FOUND:   return "Session Not Found";
    case Status::INTERNAL_ERROR:      return "Internal Server Error";
    case Status::NOT_IMPLEMENTED:     return "Not Implemented";
    default:                          return "Unknown";
    }
}

Method parseMethod(const char *s) {
    if (!s) return Method::UNKNOWN;
    if (!strcmp(s, "OPTIONS"))       return Method::OPTIONS;
    if (!strcmp(s, "DESCRIBE"))      return Method::DESCRIBE;
    if (!strcmp(s, "SETUP"))         return Method::SETUP;
    if (!strcmp(s, "PLAY"))          return Method::PLAY;
    if (!strcmp(s, "PAUSE"))         return Method::PAUSE;
    if (!strcmp(s, "TEARDOWN"))      return Method::TEARDOWN;
    if (!strcmp(s, "GET_PARAMETER")) return Method::GET_PARAMETER;
    if (!strcmp(s, "SET_PARAMETER")) return Method::SET_PARAMETER;
    if (!strcmp(s, "ANNOUNCE"))      return Method::ANNOUNCE;
    if (!strcmp(s, "RECORD"))        return Method::RECORD;
    return Method::UNKNOWN;
}

int parseCSeq(const char *headers) {
    if (!headers) return -1;
    // Search for "CSeq:" anywhere in the headers.
    // The headers pointer may start at the first header or mid-line if
    // the request had \r\n\r\n between method and headers, so we scan
    // the entire buffer rather than relying on a prefix match.
    const char *p = stristr(headers, "CSeq:");
    if (!p) return -1;
    while (*p && *p != ':') p++;
    if (*p == ':') p++;
    while (*p == ' ') p++;
    return atoi(p);
}

// ── Base-64 ─────────────────────────────────────────────────────────────────

static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const uint8_t *data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
        out.push_back(kB64[(n >> 18) & 0x3F]);
        out.push_back(kB64[(n >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? kB64[(n >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? kB64[n & 0x3F] : '=');
    }
    return out;
}

// ── NTP timestamp (1900 epoch, seconds << 32 | fractional) ──────────────────

uint64_t ntpTimestamp() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    // NTP epoch = 1900-01-01, Unix epoch = 1970-01-01 → offset = 2208988800 s
    constexpr uint64_t kNtpEpochOffset = 2208988800ULL;
    uint64_t sec  = static_cast<uint64_t>(tv.tv_sec) + kNtpEpochOffset;
    uint64_t frac = static_cast<uint64_t>(tv.tv_usec) * 4294967296ULL / 1000000ULL;
    return (sec << 32) | frac;
}

} // namespace simple_rtsp
