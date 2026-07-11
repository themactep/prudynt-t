# RTSP Server

Prudynt ships with a custom lightweight RTSP server (`simple-rtsp/`) that
replaces live555.  It supports H.264/H.265 video with AAC audio over TCP
interleaved or UDP transport.

## Architecture

```
┌─────────────┐     tap      ┌──────────────────┐
│ IMP Encoder │─────────────▶│ MsgChannel       │
│  (channel 0)│              └────────┬─────────┘
└─────────────┘                      │
                                     ▼
                              ┌──────────────┐     RTP      ┌────────┐
                              │  RtspServer  │─────────────▶│ client │
                              │  drain loop  │              └────────┘
                              └──────────────┘
```

Each RTSP session registers a **tap** on the encoder's `MsgChannel`.  The
drain loop polls every 10 ms, reads queued NALs from the tap, packetizes them
into RTP (RFC 6184 for H.264, RFC 7798 for H.265), and interleaves them
over the RTSP TCP connection.

## TCP Send Strategy

All TCP interleaved sends are **non-blocking** (`MSG_DONTWAIT`).  When the
socket can't accept data immediately (client slow, TCP window full), the
packet is enqueued in a per-session `sendQueue`.  Every poll cycle the drain
loop aggressively retries queued packets before reading new NALs.

- Queue is **unbounded** — NALs are never dropped.  If memory runs out the
  OOM killer handles it, which is preferable to silently corrupting the
  stream.
- Returning `false` from the output callback stops the entire drain loop,
  dropping all remaining NALs.  The callback returns `true` even when
  queuing, so drain continues and the retry loop catches up.

## Timestamps

Video RTP timestamps use a **frame counter** at the declared framerate
(90 kHz / fps ticks per frame), not the encoder's `imp_ts`.  This is immune
to `imp_ts` resets that occur when the MJPEG encoder reinitializes.

- Timestamps are strictly monotonic and never go backward.
- Audio timestamps use wall-clock `gettimeofday()` with a session-start
  anchor (audio frames don't carry `imp_ts`).

## SPS/PPS Re-send

When the encoder reconfigures (e.g. day/night mode switch), it emits new
SPS/PPS NALs.  The server detects this via an FNV-1a hash of the SPS content
and re-prepends SPS/PPS before the next data NAL, ensuring the client decoder
stays in sync.

## MJPEG Preview Interaction

The H.264 and JPEG encoders share ISP frame resources on Ingenic SoCs.
When the MJPEG preview reconfigures its encoder (deinit/init), it steals
ISP frames and can cause H.264 decode errors on the RTSP stream.

**Fix:** The JPEG worker only reinitializes its encoder on **size** changes,
not on FPS changes.  FPS-only changes are handled by adjusting the polling
rate without touching the encoder hardware.

```
MJPG reconfig trigger:
  SIZE change → deinit/init (brief H.264 glitch)
  FPS change  → polling rate adjustment (no glitch)
```

The WebUI preview defaults to 5 fps.  Reducing this to 2–3 fps significantly
reduces ISP contention:

```sh
# In /var/www/cgi-bin/mjpeg.sh or equivalent
FPS=2
```

## Configuration

RTSP settings in `config.json`:

| Key                      | Default | Description                            |
|--------------------------|---------|----------------------------------------|
| `rtsp.send_buffer_size`  | 307200  | TCP socket send buffer (bytes)         |
| `rtsp.send_timeout`      | 5       | SO_SNDTIMEO in seconds (0 = disabled)  |
| `rtsp.est_bitrate`       | 3000    | Advertised bitrate in SDP (kbps)       |

## URL Endpoints

| URL                          | Description                        |
|------------------------------|------------------------------------|
| `rtsp://<ip>/ch0`           | Main H.264 stream (channel 0)      |
| `rtsp://<ip>/ch1`           | Sub stream (channel 1, if enabled) |

Default credentials: `thingino:thingino`

## Debugging

```sh
# Enable RTSP debug logging
export PRUDYNT_LOGLEVEL=DEBUG

# Watch drain rate (should be ~30ms per NAL at 30fps)
logread | grep "video drain"

# Check for queue buildup (indicates slow client)
logread | grep "sendQueue"
```
