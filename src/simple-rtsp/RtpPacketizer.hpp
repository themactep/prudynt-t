#pragma once

#include "RtspTypes.hpp"
#include <cstdint>
#include <cstddef>
#include <functional>

namespace simple_rtsp {

// -- RTP packet header builder -----------------------------------------------
// Writes 12-byte RTP header. Returns number of bytes written (always 12).
// marker = 1 for the last packet of a frame.
int buildRtpHeader(uint8_t *buf, size_t bufSize,
                   uint8_t payloadType, bool marker,
                   uint16_t seq, uint32_t timestamp, uint32_t ssrc);

// -- H.264 packetization (RFC 6184) ------------------------------------------
// Callback receives each RTP payload (with 12-byte header already prepended).
// - single NAL <= 1450 bytes  -> one Single NAL Unit packet
// - larger NAL               -> FU-A fragmentation
//
// `nalData` points to raw NAL (no start code). `nalLen` is its length.
// `isFirst`/`isLast` control the RTP marker bit.
// Returns false if the output callback returned false (backpressure).
using RtpOutput = std::function<bool(const uint8_t *rtpPacket, size_t len)>;

bool packetizeH264(const uint8_t *nalData, size_t nalLen,
                   bool isFirstFrame, bool isLastFrame,
                   uint8_t payloadType, RtpState &state,
                   const RtpOutput &output);

// -- H.265 packetization (RFC 7798) ------------------------------------------

bool packetizeH265(const uint8_t *nalData, size_t nalLen,
                   bool isFirstFrame, bool isLastFrame,
                   uint8_t payloadType, RtpState &state,
                   const RtpOutput &output);

// -- L16 packetization (RFC 3551 S4.5.10) -------------------------------------
// Fragments raw PCM into MTU-sized RTP packets.  Each packet shares the same
// timestamp; only the last packet carries the marker bit.
// `sampleBytes` is the size of one interleaved sample (2 for 16-bit mono, 4 for
// stereo).  Chunk sizes are rounded down to a multiple of `sampleBytes`.
// Returns false if the output callback returned false (backpressure).
bool packetizeL16(const uint8_t *pcmData, size_t pcmLen,
                  int sampleBytes,
                  uint8_t payloadType, RtpState &state,
                  const RtpOutput &output);

// -- Raw RTP send (single packet, no codec-specific framing) ----------------
// Used for PCMU, PCMA, OPUS, L16 --- sends payload directly with RTP header.
// Returns false if the output callback returned false (backpressure).
bool sendOne(const uint8_t *payload, size_t payloadLen,
             uint8_t pt, bool marker,
             RtpState &state, const RtpOutput &output);

// -- AAC packetization (RFC 3640 / 6416) -------------------------------------
// Each call = one AAC access unit.  AU-header-len = 2 bytes (13-bit size +
// 3-bit index).
// Returns false if the output callback returned false (backpressure).
bool packetizeAAC(const uint8_t *auData, size_t auLen,
                  int64_t ptsUs, int sampleRate,
                  uint8_t payloadType, RtpState &state,
                  const RtpOutput &output);

} // namespace simple_rtsp
