# RTSP Server

Prudynt ships with a custom lightweight RTSP server (`simple-rtsp/`) that
replaces live555.  It supports H.264/H.265 video with AAC audio over TCP
interleaved or UDP transport.

## Architecture

```
┌─────────────┐
│ IMP Encoder │
│ (channel 0) │
└──────┬──────┘
       │ fan-out (one copy per registered tap)
       ▼
┌─────────────────────────────────────────────┐
│ video_stream tap list                        │
│   ├─ MsgChannel ─▶ RtspServer session A      │
│   ├─ MsgChannel ─▶ RtspServer session B      │
│   └─ MsgChannel ─▶ HTTP MJPEG / WS fMP4      │
└─────────────────────────────────────────────┘
```

The encoder has no single shared sink. `VideoWorker` fans every NAL out to
the taps registered on the `video_stream`, and each RTSP session owns one tap.
The RTSP drain loop polls every 10 ms, reads queued NALs from its tap,
packetizes them into RTP (RFC 6184 for H.264, RFC 7798 for H.265), and
interleaves them over the RTSP TCP connection.

## TCP Send Strategy

All TCP interleaved sends are **non-blocking** (`MSG_DONTWAIT`).  When the
socket can't accept data immediately (client slow, TCP window full), the
packet is appended to a per-session `sendQueue`.  Every poll cycle the drain
loop retries queued packets before reading new NALs.

The per-client path is bounded at three points so one stalled consumer cannot
exhaust a 64 MB device:

- The per-client tap is byte-bounded (`VIDEO_TAP_MAX_BYTES` 256 KB,
  `AUDIO_TAP_MAX_BYTES` 64 KB).  It evicts older whole frames and always
  keeps the newest, so a stalled client holds a bounded backlog instead of
  pinning megabytes of NALs.
- `sendQueue` drops new NALs once it passes `SEND_QUEUE_HIGH_WATERMARK`
  (768 KB); a single IDR burst can briefly reach `SEND_QUEUE_HARD_CAP`
  (4 MB).  A dropped client re-syncs on the next IDR.
- `rtsp.max_clients` caps concurrent PLAY sessions per stream (0 = unlimited).
  A PLAY past the cap gets `503 Service Unavailable`.
- `rtsp.session_reclaim` (seconds) reclaims a session that has been idle, or
  whose send queue has stayed backed up, for that long.  A client that keeps
  sending RTCP but never reads its stream would otherwise pin the socket and
  the WiFi airtime forever.

Returning `false` from the output callback stops the drain loop for that
cycle.  The callback returns `true` even when queuing, so the retry loop
catches up.

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
| `rtsp.est_bitrate`       | 5000    | Advertised bitrate in SDP (kbps)       |
| `rtsp.max_clients`       | 0       | Max concurrent PLAYs per stream (0 = unlimited) |
| `rtsp.session_reclaim`   | 65      | Seconds an idle or non-draining session may live |
| `rtsp.auth_required`     | true    | Require authentication before DESCRIBE |
| `rtsp.auth_mode`         | digest  | `digest`, `basic` or `both`            |

## Authentication

RTSP credentials are configured through `rtsp.username` / `rtsp.password`.

`rtsp.auth_mode` selects how they are checked:

- `digest` (default) -- challenges with `WWW-Authenticate: Digest`
  (MD5 + `qop="auth"`).  The password never crosses the wire; only an
  MD5 response that is bound to a server nonce, so a captured request
  cannot be replayed.
- `basic` -- legacy HTTP-style Basic auth.  The credentials are base64
  encoded (i.e. effectively plain text) on every request.  Kept only for
  clients that cannot do Digest.
- `both` -- advertises both schemes and accepts either.  A Digest-capable
  client picks Digest; an older client falls back to Basic.  Use this
  during migration, then switch to `digest`.

The nonce is stateless: it carries a timestamp and an MD5 bound to a
per-process secret, and is accepted for 5 minutes.  A session that has
authenticated once is not re-challenged for subsequent requests.

## URL Endpoints

| URL                          | Description                        |
|------------------------------|------------------------------------|
| `rtsp://<ip>/ch0`            | Main H.264 stream (channel 0)      |
| `rtsp://<ip>/ch1`            | Sub stream (channel 1, if enabled) |

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
