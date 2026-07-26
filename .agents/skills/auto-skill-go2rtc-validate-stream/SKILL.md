---
name: go2rtc-validate-stream
description: Validate an RTSP stream via go2rtc, diagnose Prudynt RTSP quirks, and apply fixes.
source: auto-skill
extracted_at: '2026-07-13T02:27:12.000Z'
---

# Validate RTSP stream via go2rtc

## When to use
You need to confirm an RTSP stream is reachable and can be served over WebRTC/MSE.

## Step 1 — probe the RTSP stream first

```bash
ffprobe -rtsp_transport tcp -i rtsp://user:pass@<IP>/<path> -show_streams -show_format -v error -timeout 10000000
```

Confirm video codec and profile level. Check `profile-level-id` in the `a=fmtp` SDP line:
- **640033** = H.264 High profile, level 5.1 → **exceeds browser WebRTC support** (max ~4.1)
- **42001f** = H.264 High profile, level 3.1 → works in most browsers

## Thingino/Prudynt RTSP quirks and fixes

### Quirk 1: zero RTP-Info (FIXED)

Prudynt's `simple-rtsp` RTSP server sends `RTP-Info: seq=0;rtptime=0` in the
PLAY response because `RtpState` is zero-initialized at connection time. This
causes FFmpeg to fail tracking RTP packets, making it appear to hang.

**Fix** — `RtspServer.cpp::acceptClient()`:

```cpp
s->videoRtp.seq = static_cast<uint16_t>(rand());   // ← ADD
s->audioRtp.seq = static_cast<uint16_t>(rand());   // ← ADD
```

**Verify** — `RTP-Info: url=...;seq=<random>;rtptime=0` (non-zero seq).

### Quirk 2: UDP transport not implemented (FIXED)

The SETUP handler accepted `RTP/AVP` (UDP) transport but never created UDP
sockets for RTP/RTCP. Clients immediately disconnect with "Nonmatching
transport in server reply" because the server responded with TCP interleaved
when the client requested UDP.

**Fix** — `RtspServer.cpp::handleSetup()`: implement real UDP transport:

1. Create UDP sockets with `socket(AF_INET, SOCK_DGRAM, 0)`, bind to port 0 (kernel picks)
2. Parse `client_port` from the SETUP request
3. Include `server_port=X-Y` in the SETUP response
4. Use `sendto()` with the client address for RTP/RTCP delivery
5. Close UDP sockets in `closeClient()`

Session struct additions:
```cpp
int     videoRtpSock    = -1;
int     videoRtcpSock   = -1;
int     audioRtpSock    = -1;
int     audioRtcpSock   = -1;
uint16_t videoRtpPort   = 0;
uint16_t videoRtcpPort  = 0;
uint16_t audioRtpPort   = 0;
uint16_t audioRtcpPort  = 0;
sockaddr_in clientAddr  = {};
socklen_t clientAddrLen = 0;
```

SETUP response for UDP:
```cpp
snprintf(hdr, sizeof(hdr),
         "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\n"
         "Session: %s\r\n",
         s->videoRtpPort, s->videoRtcpPort,
         s->videoRtpPort, s->videoRtcpPort,
         s->sessionId);
```

sendVideoNal/sendAudioFrame/sendRtcpSr: use `sendto()` with UDP sockets when `!s.tcpInterleaved`.

### Quirk 3: CSeq parsing fails on some clients (FIXED)

When clients send a blank line between the method and headers (e.g. ffplay),
`headersStart` points mid-line and `parseCSeq` can't find `CSeq:`, returning
0. The server responds with `CSeq: 0` and clients reject it.

**Fix** — two parts:

1. `RtspServer.cpp`: skip all blank lines after the method:

```cpp
if (headersStart) {
    if (*headersStart == '\r') headersStart++;
    if (*headersStart == '\n') headersStart++;
    while (*headersStart == '\r' || *headersStart == '\n')
        headersStart++;
}
```

2. `RtspTypes.cpp`: scan entire buffer for `CSeq:` instead of prefix-matching:

```cpp
const char *p = stristr(headers, "CSeq:");
if (!p) return -1;
```

### Quirk 4: RTCP SR sent before first frame causes FFmpeg jitter buffer drops (FIXED)

When `ffplay -rtsp_transport udp` connects, FFmpeg reports "RTP: dropping old packet
received too late" repeatedly and never plays video.

**Root cause:** `handlePlay()` sends an RTCP Sender Report immediately with
`timestamp=0` and the initial sequence number. By the time actual RTP packets arrive,
their timestamps have advanced but FFmpeg uses the RTCP SR mapping as authoritative,
treating the real packets as "from the past."

**Fix** — defer the initial RTCP SR until after the first video frame:

1. Add `bool sendInitialRtcpSr = false;` to Session struct
2. In `handlePlay()`, set `s->sendInitialRtcpSr = true;` instead of calling `sendRtcpSr(*s)`
3. In `sendVideoNal()`, after packetizing the first frame start:

```cpp
if (ok && s.sendInitialRtcpSr && nal.is_frame_start) {
    sendRtcpSr(s);
    s.sendInitialRtcpSr = false;
}
```

---

**Build command** (run from Thingino tree root):
```bash
CAMERA=<camera_name> IP=<camera_ip> make rebuild-prudynt-t
```

The binary is automatically copied to `/nfs/prudynt` and picked up by the camera.

## Step 2 — write minimal go2rtc config

### Direct passthrough (browser-compatible streams only)

Use **raw RTSP URL** — no ffmpeg wrapper:

```yaml
log:
  level: info

api:
  listen: ":1984"

streams:
  thingino: rtsp://user:pass@<IP>/<path>

webrtc:
  listen: ":8555"
  candidates:
    - <HOST_IP>:8555   # must be a real LAN IP, not 127.0.0.1
```

### Transcode pattern (for incompatible codecs)

When the camera outputs H.264 > level 4.1 or non-Opus audio:

```yaml
log:
  level: info

api:
  listen: ":1984"

streams:
  # Raw passthrough from camera (go2rtc native RTSP client works)
  raw: rtsp://user:pass@<IP>/<path>
  # Browser-compatible: Main profile video + Opus audio
  thingino:
    - exec:ffmpeg -probesize 1M -analyzeduration 1M -rtsp_transport tcp -timeout 5000000 -i rtsp://127.0.0.1:8554/raw -c:v libx264 -profile:v main -level:v 4.1 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 60 -c:a libopus -application:a lowdelay -min_comp 0 -f rtsp -rtsp_transport tcp {output}

webrtc:
  listen: ":8555"
  candidates:
    - <HOST_IP>:8555
```

**Key points for transcode pattern:**
- Read from go2rtc's internal RTSP server (`rtsp://127.0.0.1:8554/raw`), not the camera directly
- `-probesize 1M -analyzeduration 1M` — go2rtc's relayed SDP strips the `fmtp` line (SPS/PPS)
- Do NOT use `-fflags nobuffer -flags low_delay` — these override `analyzeduration` to 0, causing "unspecified size" errors

## Step 3 — launch container

```bash
podman run --rm --network host \
  -v /tmp/go2rtc-thingino.yaml:/config/go2rtc.yaml:ro \
  docker.io/alexxit/go2rtc:latest \
  go2rtc -config /config/go2rtc.yaml
```

Use `--network host` so WebRTC candidates resolve correctly.

## Step 4 — validate data flow

Trigger a consumer, then check the API:

```bash
timeout 8 curl -s -o /dev/null "http://localhost:1984/api/stream.mp4?src=thingino" &
curl -s http://localhost:1984/api/streams | python3 -m json.tool
```

Look for:
- `"producers"` with `"remote_addr"` pointing to the camera
- `"receivers"` with non-zero `"bytes"` and `"packets"` — confirms data is flowing
- `"consumers"` with matching `"senders"`

## Troubleshooting: browser spinning with no video

If the API shows data flowing but the browser player spins:

1. **Check profile-level-id** — H.264 level 5.1 exceeds WebRTC browser support. go2rtc data flows but the browser can't decode it.
2. **Try MSE player instead** — has broader codec support: `http://localhost:1984/stream.html?src=<stream-name>`
3. **If both fail** — open browser DevTools console (F12) and check for codec decode errors.
4. **Transcode needed** — use the transcode pattern (Step 2) to re-encode to Main profile + Opus.
5. **FFmpeg won't detect video** — add `-probesize 1M -analyzeduration 1M` and remove `-fflags nobuffer` when pulling from go2rtc's internal RTSP server.
6. **FFmpeg hangs on direct camera RTSP** — Prudynt's RTSP server is incompatible with FFmpeg's RTSP client. Always use go2rtc as intermediary.

## Access URLs

| Service | URL |
|---|---|
| Web UI | http://localhost:1984 |
| WebRTC player | http://localhost:1984/webrtc.html?src=<stream-name> |
| MSE player | http://localhost:1984/stream.html?src=<stream-name> |
| MP4 download | http://localhost:1984/api/stream.mp4?src=<stream-name> |
| MJPEG stream | http://localhost:1984/api/stream.mjpeg?src=<stream-name> |
| API | http://localhost:1984/api/streams |
