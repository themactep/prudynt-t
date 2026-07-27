#pragma once

#include <cstdint>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>

namespace simple_rtsp {

// ── Portable case-insensitive strstr (no _GNU_SOURCE needed) ────────────────
inline const char *stristr(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return haystack;
    size_t nlen = std::strlen(needle);
    for (; *haystack; haystack++) {
        size_t i = 0;
        for (; i < nlen; i++) {
            if (std::tolower(static_cast<unsigned char>(haystack[i])) !=
                std::tolower(static_cast<unsigned char>(needle[i])))
                break;
        }
        if (i == nlen) return haystack;
    }
    return nullptr;
}

// ── Constants ───────────────────────────────────────────────────────────────

constexpr int MAX_CLIENTS   = 8;
constexpr int RTSP_BUF_SIZE  = 16384;
constexpr int RTP_MAX_PAYLOAD = 1200;  // stay under typical path MTU to avoid
                                        // IP fragmentation of UDP RTP datagrams
                                        // (fragment loss desyncs the decoder)
constexpr int SDP_BUF_SIZE   = 4096;

// ── RTSP Methods / Status ───────────────────────────────────────────────────

enum class Method {
    OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN,
    GET_PARAMETER, SET_PARAMETER, ANNOUNCE, RECORD,
    UNKNOWN
};

enum class Status {
    OK                    = 200,
    BAD_REQUEST           = 400,
    UNAUTHORIZED          = 401,
    NOT_FOUND             = 404,
    METHOD_NOT_ALLOWED    = 405,
    SESSION_NOT_FOUND     = 454,
    INTERNAL_ERROR        = 500,
    NOT_IMPLEMENTED       = 501
};

const char *methodToString(Method m);
const char *statusToString(Status s);
Method parseMethod(const char *s);
int parseCSeq(const char *headers);

// ── RTP state per stream ────────────────────────────────────────────────────

struct RtpState {
    uint16_t seq       = 0;   // wraps naturally at 16 bits
    uint32_t timestamp = 0;   // 32-bit RTP timestamp
    uint32_t ssrc      = 0;   // random per session
};

// ── Stream descriptors for SDP ──────────────────────────────────────────────

struct VideoStreamConfig {
    std::string name;            // "ch0", "ch1"
    std::string endpoint;        // "/ch0", "/ch1"
    std::string codec;           // "H264" or "H265"
    int width       = 1920;
    int height      = 1080;
    int fps         = 30;
    int payloadType = 96;
    int clockRate   = 90000;
    int bitrate     = 0;         // kbps, 0 = unspecified
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    std::vector<uint8_t> vps;    // H.265 only
    bool haveCodecConfig = false;
};

struct AudioStreamConfig {
    std::string endpoint;      // empty = multiplexed with video; "/mic" = standalone
    std::string codec        = "AAC";
    int sampleRate           = 16000;
    int channels             = 1;
    int payloadType          = 97;
    std::vector<uint8_t> aacConfig; // raw ASC bytes from encoder (overrides hardcoded)
};

// Backchannel (talkback) audio formats the server can receive from a client.
struct BackchannelConfig {
    std::string codec;          // "PCMU", "PCMA", "mpeg4-generic"
    int sampleRate;
    int payloadType;
};

// Subtitle stream — ASS-formatted overlay events.
struct SubtitleStreamConfig {
    std::string codec = "t140";
    int payloadType = 98;
    int clockRate = 90000;
};

// ── Base-64 helper (RFC 4648) for SDP ───────────────────────────────────────

std::string base64Encode(const uint8_t *data, size_t len);

// ── NTP timestamp helper ────────────────────────────────────────────────────

uint64_t ntpTimestamp();

} // namespace simple_rtsp
