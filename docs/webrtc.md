# WebRTC: Playback and Talkback

How prudynt streams reach web browsers over WebRTC, what the browser
demands from our RTSP/RTP output, the platform-specific pitfalls we have
hit (and fixed), and how to diagnose the next one.

Most deployments do **not** use prudynt's built-in WebRTC support.
Instead a gateway on the LAN consumes prudynt's RTSP stream and
re-serves it as WebRTC:

```
┌──────────┐  RTSP/RTP   ┌──────────────────┐  WebRTC (SRTP)  ┌─────────┐
│ prudynt  │────────────▶│ go2rtc / MediaMTX│────────────────▶│ browser │
│ (camera) │◀────────────│    (gateway)     │◀────────────────│  (mic)  │
└──────────┘  backchannel└──────────────────┘   mic track     └─────────┘
```

The gateway does *not* transcode video: our H.264 RTP packets are
depacketized, re-packetized and forwarded.  Every bitstream and RTP
detail prudynt emits is therefore visible to the browser decoder.

## The RTP marker bit is not optional

**Symptom (fixed 2026-07):** T31 cameras played fine via direct RTSP
(ffplay/VLC) and via MediaMTX WebRTC, but produced *no video at all*
through go2rtc — WebRTC showed a black screen and
`/api/stream.mp4?src=<cam>` returned a header-only MP4 with zero
samples.  T20 cameras worked everywhere.

**Root cause:** RTP receivers have two ways to detect the end of an
access unit (a complete video frame):

1. the **marker bit** (`M=1`) on the last RTP packet of the frame, and
2. a **timestamp change** on the next packet (fallback).

ffmpeg and MediaMTX use the fallback; go2rtc's H.264 depacketizer
requires the marker bit and never completes a frame without it.

prudynt sets the marker from `IMPEncoderPack.frameEnd` — and the T31
`libimp` **never sets `frameEnd`** (always false), while the T20/T23
SDKs set it correctly.  Result: on T31 no RTP packet ever carried
`M=1`.

**Fix (`VideoWorker.cpp`):** `IMP_Encoder_GetStream()` returns exactly
one encoded frame per call on every platform, so the last pack of the
batch is by definition the end of the frame:

```cpp
bool pack_is_frame_end =
    stream.pack[i].frameEnd || (i + 1 == stream.packCount);
```

This effective flag now drives the RTP marker, MP4 sample flush,
prebuffer flush and frame-timestamp tracking.  Never trust
`pack.frameEnd` alone on Ingenic SoCs.

**Litmus test** (no browser needed) — go2rtc's MP4 remuxer uses the
same frame assembly as its WebRTC path:

```sh
curl -s "http://gateway:1984/api/stream.mp4?src=CAM&duration=6" -o /tmp/t.mp4
ffprobe -v error -count_packets -select_streams v \
        -show_entries stream=nb_read_packets /tmp/t.mp4
# healthy 30 fps stream: ~180 packets; broken marker bit: 0 packets
```

## Bitstream requirements for browser decoders

Browsers only accept **H.264** over WebRTC (H.265 support is not
universally negotiated — assume it does not exist).  Streams intended
for WebRTC gateways must use `format: "H264"`.

prudynt already normalizes several Ingenic encoder quirks in
`VideoWorker.cpp` before NALs reach the RTSP tap:

| Quirk | Platform | Normalization |
|---|---|---|
| SPS/PPS emitted with `nal_ref_idc=1` (`0x27`/`0x28`) | T31+ | rewritten to `3` (`0x67`/`0x68`) — go2rtc and browsers expect it |
| SPS always declares `level_idc=51` (5.1) regardless of resolution | T31+ | rewritten to the minimum level that fits the real resolution/fps; prevents decoder buffer under-allocation ("Invalid level prefix", MB errors) above 1080p |
| SPS VUI omits or defaults to limited-range luma (16-235) while the encoder feeds near-full-range pixels | T31+ | VUI rewritten to `video_full_range_flag=1` plus an explicit BT.709 colour description at every resolution (the pipeline is BT.709 throughout; substreams are downscales, not a re-matrix to BT.601); without it players clip shadows/highlights (see issue #1547) |
| `pack.frameEnd` never set | T31 | last pack of the `GetStream` batch treated as frame end (RTP marker bit) |

Known remaining platform differences (harmless so far, but relevant
when reading `webrtc-internals`):

| Property | T20 | T31 |
|---|---|---|
| `pic_order_cnt_type` | 2 (no reordering possible) | 0 |
| VUI | timing only | timing + video signal + VCL HRD |
| `bitstream_restriction` / `max_num_reorder_frames=0` | absent | absent |

With `poc_type=0` and no `bitstream_restriction`, a *strict* hardware
decoder may derive a worst-case DPB reorder depth from the level
(≈4 frames at 1080p L4.1 → ~130 ms extra latency).  ffmpeg-based
decoders (Chrome software path) reorder adaptively and are not
affected.  If T31 latency on hardware-decode clients ever becomes an
issue, the fix is rewriting the SPS VUI to add
`bitstream_restriction(max_num_reorder_frames=0)` — bit-level RBSP
surgery, doable next to the existing level_idc rewrite.

## Audio: AAC does not cross WebRTC

Browsers cannot receive AAC over WebRTC.  With the default
`mic_format: "AAC"` the gateways behave as follows:

- **MediaMTX** logs `skipping track 2 (MPEG-4 Audio)` and serves
  video-only.
- **go2rtc** serves video-only unless an ffmpeg transcode source is
  configured.

For working WebRTC audio set the mic to a WebRTC-native codec:

```json
"audio": { "mic_format": "OPUS" }
```

(OPUS requires `BR2_PACKAGE_PRUDYNT_T_OPUS=y`.)  PCMU/PCMA also pass
through, at telephone quality.  RTSP-only consumers (NVRs, ffmpeg) are
happy with AAC — choose per deployment.

## Two-way audio (backchannel)

prudynt implements the ONVIF audio backchannel
(`Require: www.onvif.org/ver20/backchannel`) in `simple-rtsp/`:

- the sendonly `track0` m-line is only added to the SDP when the
  client sends the ONVIF `Require` header (plain players never see it);
- activation happens on **PLAY**, not RECORD (ONVIF Streaming Spec
  §5.3);
- offered formats: AAC, **OPUS/48000/2**, PCMU, PCMA.

Gateway support:

| Gateway | Backchannel | Notes |
|---|---|---|
| go2rtc | **yes** | sends the `Require` header, SETUPs `track0`, pushes browser mic OPUS straight through (no transcoding). Use `media=video+audio+microphone`. |
| MediaMTX | **no** (as of v1.19.2) | feature request [mediamtx#941] open; gortsplib v5 has full client support (`RequestBackChannels`, `proxy-backchannel` example) but MediaMTX does not use it. It *skips* backchannel tracks cleanly since their #5074 fix. |

[mediamtx#941]: https://github.com/bluenviron/mediamtx/issues/941

Offering OPUS on `track0` is what makes browser talkback free: the
browser microphone is already OPUS 48 kHz, so the gateway forwards
packets untouched and prudynt decodes them (`Opus.cpp`) for the
speaker.

### Sketch: paired-path talkback for MediaMTX

If MediaMTX backchannel support is ever needed, the design that fits
its unidirectional path model is a *paired path* (proposed, not
implemented):

```yaml
paths:
  cam:
    source: rtsp://user:pass@camera:554/ch0
    rtspRequestBackChannels: yes   # NEW: send the ONVIF Require header
    backChannelPath: cam_talk      # NEW: forward audio published there
  cam_talk: {}                     # WHIP/RTSP publish target
```

Mic audio is published to `cam_talk` by any stock protocol (browser
WHIP, `ffmpeg -f rtsp`); the RTSP source client attaches as an
internal reader and writes the RTP to the camera's backchannel track.
No transcoding: the publisher codec must be one of the camera's
`track0` formats.  Prototype base: gortsplib's `proxy-backchannel`
example.

## Diagnostic playbook

Work from the camera outward; each step isolates one leg.

**1. SDP sanity** — what does the camera advertise?

```sh
printf 'DESCRIBE rtsp://CAM:554/ch0 RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n' \
  | nc CAM 554
```

Check: `H264` (not H265), `profile-level-id`, `sprop-parameter-sets`
(decode the SPS if in doubt), audio codec, and that `track0` only
appears when the ONVIF header is sent.

**2. Bitstream sanity** — does ffmpeg decode cleanly?

```sh
ffmpeg -v warning -rtsp_transport tcp -t 12 -i rtsp://user:pass@CAM:554/ch0 -f null -
```

Zero warnings expected.  ffmpeg is *forgiving* (timestamp-based frame
detection, adaptive reordering) — a clean result here does **not**
clear the stream for WebRTC.

**3. RTP-level inspection** — the layer ffmpeg hides.  A ~80-line
python RTSP client (DESCRIBE/SETUP/PLAY over TCP interleaved) that
prints per-packet `seq / ts / M / NAL type / FU-A S,E flags` is the
tool that found the T31 marker bug.  Verify:

- every frame's last packet (single NAL or `FU-A |E`) has `M=1`;
- all packets of one frame share one timestamp;
- frame-to-frame timestamp step is constant (3000 ticks @ 30 fps);
- IDR frames are preceded by SPS+PPS with the same timestamp.

**4. Gateway frame assembly** — the go2rtc MP4 litmus test from the
marker-bit section above.  Also useful:
`http://gateway:1984/api/streams?src=CAM` shows the producer SDP,
per-receiver packet counters, and which consumers get senders.

**5. Browser** — `chrome://webrtc-internals`: `framesReceived` vs
`framesDecoded` (received-but-not-decoded ⇒ bitstream problem;
nothing received ⇒ gateway problem), `pliCount` climbing ⇒ decoder
resets, jitter buffer delay ⇒ timestamp problems.

Reference symptom matrix from the T31 marker-bit incident:

| Consumer | Result | Why |
|---|---|---|
| ffplay direct RTSP | ✔ plays | ts-change frame detection |
| MediaMTX WebRTC | ✔ plays | tolerant AU assembly |
| go2rtc WebRTC | ✘ black | needs marker bit |
| go2rtc stream.mp4 | ✘ 0 frames | needs marker bit |

A partial failure pattern like this almost always means a
*metadata/framing* bug (marker, timestamps, SPS declarations), not a
broken encoder.

## Built-in WebRTC (libdatachannel)

Prudynt can be built with native WebRTC support
(`BR2_PACKAGE_PRUDYNT_T_WEBRTC=y`), which links
[libdatachannel](https://github.com/paullouisageneau/libdatachannel)
(`-ldatachannel -lusrsctp -ljuice` plus OpenSSL or mbedTLS).  This
embeds a `rtc::PeerConnection` directly in prudynt: encoded frames are
pushed as RTP to the peer, signaling (SDP/ICE) is exchanged over the
websocket API.  It is experimental and not the default deployment
path; the gateway model above is what the fleet runs.
