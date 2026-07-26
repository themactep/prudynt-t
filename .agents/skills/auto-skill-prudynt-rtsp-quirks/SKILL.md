---
name: prudynt-rtsp-quirks
description: Known quirks in Prudynt's simple-rtsp server and how they were fixed
source: auto-skill
extracted_at: '2026-07-13T06:48:00Z'
---

# Prudynt RTSP Server Quirks & Fixes

The simple-rtsp server in `overrides/prudynt-t/src/simple-rtsp/` has several quirks
that cause compatibility issues with FFmpeg/ffplay and WebRTC consumers (go2rtc).

## Quirk 1: `RTP-Info: seq=0;rtptime=0` in PLAY response

**Symptom:** FFmpeg connects via RTSP but receives zero sequence number and timestamp
in the PLAY response, causing RTP stream tracking to fail.

**Root cause:** In `RtspServer.cpp`, `handlePlay()` sends the RTSP response before any
RTP packets flow, so `s->videoRtp.seq` and `s->videoRtp.timestamp` are still at their
default zero values.

**Fix:** Initialize `videoRtp.seq` and `audioRtp.seq` with random values in
`acceptClient()` so the RTP-Info has non-zero values that match actual packet
sequence numbers.

## Quirk 2: UDP transport silently accepted but not implemented

**Symptom:** `ffplay -rtsp_transport udp` receives "Nonmatching transport in server
reply" then disconnects, or connects but sees "RTP: dropping old packet received too
late" endlessly.

**Root cause:** The SETUP handler accepted `RTP/AVP` (UDP) transport but never created
server-side UDP sockets or sent data via `sendto()`. It just forced TCP interleaved in
response, causing protocol mismatch.

**Fix:** Added UDP socket creation (`socket(AF_INET, SOCK_DGRAM)`) in `handleSetup()`,
proper `server_port` reporting in SETUP response, and `sendto()` calls in
`sendVideoNal()`, `sendAudioFrame()`, and `sendRtcpSr()` for UDP transport. UDP
sockets are cleaned up in `closeClient()`.

**Critical detail:** Audio and video have separate SETUP requests. The per-stream UDP
port fields must be separate (`videoClientRtpPort`, `audioClientRtpPort`, etc.) so the
audio SETUP doesn't overwrite the video client ports.

## Quirk 3: CSeq parsing fails with blank line between method and headers

**Symptom:** FFmpeg reports "CSeq 5 expected, 0 received" — server responds with
`CSeq: 0` instead of echoing the request's CSeq.

**Root cause:** `parseCSeq()` searched for `\r\nCSeq:` but the headers pointer could
start mid-line if the RTSP request had `\r\n\r\n` between the method line and headers.
The CSeq value was never found, defaulting to 0.

**Fix:** Changed `parseCSeq()` to search for `CSeq:` anywhere in the headers buffer
using `stristr()`. Also improved header extraction in `handleRequest()` to skip any
blank lines after the method line.

## Quirk 4: RTCP SR sent before first frame causes FFmpeg jitter buffer drops

**Symptom:** `ffplay -rtsp_transport udp` shows "RTP: dropping old packet received too
late" repeatedly, video doesn't play.

**Root cause:** `handlePlay()` sends an RTCP Sender Report immediately with
`timestamp=0` and the initial sequence number. By the time actual RTP packets arrive,
their timestamps have advanced but FFmpeg uses the RTCP SR mapping as authoritative,
treating the real packets as "from the past."

**Fix:** Defer the initial RTCP SR until after the first video frame is sent
(`s->sendInitialRtcpSr` flag set in `handlePlay()`, cleared in `sendVideoNal()` after
the first frame start), so the timestamp and sequence number match actual RTP packets.

## Build

```bash
CAMERA=<camera> make rebuild-prudynt-t
```

Binary is deployed to `/nfs/prudynt` for camera pickup.