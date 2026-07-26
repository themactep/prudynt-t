#include "RtpPacketizer.hpp"
#include <algorithm>
#include <cstring>

namespace simple_rtsp {

// ── RTP header builder ──────────────────────────────────────────────────────

int buildRtpHeader(uint8_t *buf, size_t bufSize,
                   uint8_t payloadType, bool marker,
                   uint16_t seq, uint32_t timestamp, uint32_t ssrc) {
    if (bufSize < 12) return -1;
    memset(buf, 0, 12);
    buf[0] = 0x80; // V=2
    buf[1] = (marker ? 0x80 : 0x00) | (payloadType & 0x7F);
    buf[2] = static_cast<uint8_t>((seq >> 8) & 0xFF);
    buf[3] = static_cast<uint8_t>(seq & 0xFF);
    buf[4] = static_cast<uint8_t>((timestamp >> 24) & 0xFF);
    buf[5] = static_cast<uint8_t>((timestamp >> 16) & 0xFF);
    buf[6] = static_cast<uint8_t>((timestamp >> 8) & 0xFF);
    buf[7] = static_cast<uint8_t>(timestamp & 0xFF);
    buf[8] = static_cast<uint8_t>((ssrc >> 24) & 0xFF);
    buf[9] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
    buf[10] = static_cast<uint8_t>((ssrc >> 8) & 0xFF);
    buf[11] = static_cast<uint8_t>(ssrc & 0xFF);
    return 12;
}

// ── Internal: send one RTP packet ───────────────────────────────────────────

bool sendOne(const uint8_t *payload, size_t payloadLen,
             uint8_t pt, bool marker,
             RtpState &state, const RtpOutput &output) {
    uint8_t pkt[1500];
    int hdrLen = buildRtpHeader(pkt, sizeof(pkt), pt, marker,
                                state.seq, state.timestamp, state.ssrc);
    if (hdrLen < 0) return false;
    size_t total = static_cast<size_t>(hdrLen) + payloadLen;
    if (total > sizeof(pkt)) return false;
    memcpy(pkt + hdrLen, payload, payloadLen);
    uint16_t thisSeq = state.seq++;
    (void)thisSeq;
    return output(pkt, total);
}

// ── H.264 packetization (RFC 6184) ──────────────────────────────────────────

bool packetizeH264(const uint8_t *nalData, size_t nalLen,
                   bool /*isFirstFrame*/, bool isLastFrame,
                   uint8_t payloadType, RtpState &state,
                   const RtpOutput &output) {
    if (!nalData || nalLen == 0) return true;

    uint8_t nalHeader = nalData[0];
    uint8_t nalType   = nalHeader & 0x1F;

    if (nalLen <= static_cast<size_t>(RTP_MAX_PAYLOAD)) {
        // Single NAL Unit packet
        bool marker = isLastFrame && (nalType == 1 || nalType == 5);
        return sendOne(nalData, nalLen, payloadType, marker, state, output);
    }

    // ── FU-A fragmentation ────────────────────────────────────────────
    const uint8_t *fragData = nalData + 1;  // skip NAL header
    size_t fragLen = nalLen - 1;

    uint8_t fuIndicator = (nalHeader & 0xE0) | 28;  // F + NRI + type=28

    size_t offset = 0;
    while (offset < fragLen) {
        size_t chunk = std::min(fragLen - offset,
                                static_cast<size_t>(RTP_MAX_PAYLOAD - 2));
        bool start = (offset == 0);
        bool end   = (offset + chunk >= fragLen);
        bool marker = isLastFrame && end;

        uint8_t fuHeader = (start ? 0x80 : 0x00) |
                           (end   ? 0x40 : 0x00) |
                           nalType;

        uint8_t fuBuf[RTP_MAX_PAYLOAD];
        fuBuf[0] = fuIndicator;
        fuBuf[1] = fuHeader;
        memcpy(fuBuf + 2, fragData + offset, chunk);
        if (!sendOne(fuBuf, chunk + 2, payloadType, marker, state, output))
            return false;
        offset += chunk;
    }
    return true;
}

// ── H.265 packetization (RFC 7798) ──────────────────────────────────────────

bool packetizeH265(const uint8_t *nalData, size_t nalLen,
                   bool /*isFirstFrame*/, bool isLastFrame,
                   uint8_t payloadType, RtpState &state,
                   const RtpOutput &output) {
    if (!nalData || nalLen < 2) return true;

    uint8_t nalType = (nalData[0] >> 1) & 0x3F;

    if (nalLen <= static_cast<size_t>(RTP_MAX_PAYLOAD)) {
        bool marker = isLastFrame;
        return sendOne(nalData, nalLen, payloadType, marker, state, output);
    }

    // ── FU fragmentation ──────────────────────────────────────────────
    const uint8_t *fragData = nalData + 2;
    size_t fragLen = nalLen - 2;
    uint8_t fuIndicator = (nalData[0] & 0x81) | (49 << 1);

    size_t offset = 0;
    while (offset < fragLen) {
        size_t chunk = std::min(fragLen - offset,
                                static_cast<size_t>(RTP_MAX_PAYLOAD - 3));
        bool start = (offset == 0);
        bool end   = (offset + chunk >= fragLen);
        bool marker = isLastFrame && end;

        uint8_t fuBuf[RTP_MAX_PAYLOAD];
        fuBuf[0] = fuIndicator;
        fuBuf[1] = nalData[1];
        fuBuf[2] = (start ? 0x80 : 0x00) |
                   (end   ? 0x40 : 0x00) |
                   nalType;
        memcpy(fuBuf + 3, fragData + offset, chunk);
        if (!sendOne(fuBuf, chunk + 3, payloadType, marker, state, output))
            return false;
        offset += chunk;
    }
    return true;
}

// ── L16 packetization (RFC 3551 §4.5.10) ─────────────────────────────────────

bool packetizeL16(const uint8_t *pcmData, size_t pcmLen,
                  int sampleBytes,
                  uint8_t payloadType, RtpState &state,
                  const RtpOutput &output) {
    if (!pcmData || pcmLen == 0) return true;
    if (sampleBytes < 1 || sampleBytes > 4) sampleBytes = 2;

    size_t maxChunk = RTP_MAX_PAYLOAD;
    // Align chunk to sample boundary so we never split a sample
    maxChunk = (maxChunk / sampleBytes) * sampleBytes;
    if (maxChunk == 0) maxChunk = sampleBytes;

    size_t offset = 0;
    while (offset < pcmLen) {
        size_t chunk = std::min(pcmLen - offset, maxChunk);
        bool last = (offset + chunk >= pcmLen);
        if (!sendOne(pcmData + offset, chunk, payloadType, last, state, output))
            return false;
        offset += chunk;
    }
    return true;
}

// ── AAC packetization (RFC 3640 / 6416) ─────────────────────────────────────

bool packetizeAAC(const uint8_t *auData, size_t auLen,
                  int64_t /*ptsUs*/, int /*sampleRate*/,
                  uint8_t payloadType, RtpState &state,
                  const RtpOutput &output) {
    if (!auData || auLen == 0 || auLen > 0x1FFF) return true;

    // RFC 3640: AU-headers-length (16 bits, in bits) + AU-header (16 bits) + raw data
    uint8_t buf[1500];
    // AU-headers-length = 16 bits for one 16-bit AU header
    buf[0] = 0;
    buf[1] = 16;
    // AU-header: 13-bit AU-size | 3-bit AU-index (0 for single AU)
    uint16_t auHeader = static_cast<uint16_t>(auLen << 3);
    buf[2] = static_cast<uint8_t>((auHeader >> 8) & 0xFF);
    buf[3] = static_cast<uint8_t>(auHeader & 0xFF);
    memcpy(buf + 4, auData, auLen);

    return sendOne(buf, auLen + 4, payloadType, /*marker*/ true, state, output);
}

} // namespace simple_rtsp
