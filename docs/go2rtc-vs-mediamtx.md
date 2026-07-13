# Browser playback of the prudynt RTSP stream — go2rtc vs MediaMTX

This document records the investigation into why browser WebRTC playback of the
Thingino/prudynt camera stream was black through **go2rtc**, and why **MediaMTX**
was adopted as the working streaming server (WebRTC + MoQ + HLS).

## TL;DR

- The camera RTSP stream (`H264 High` + `AAC-LC`) plays fine through MediaMTX
  over **WebRTC**, **MoQ**, and **HLS**.
- Through go2rtc, the **HTTP/HLS** path worked but **WebRTC** was black (consumer
  connected, no frames, no error).
- The defect is isolated to **go2rtc's WebRTC H.264 path** — not the camera, not
  the codec, and not Chrome's WebRTC H.264 decoder.
- MediaMTX is now the browser-streaming server. go2rtc is left as-is.

## Camera stream

- RTSP URL: `rtsp://thingino:thingino@192.168.88.31:554/ch0` (Ethernet)
- Video: `H264`, `profile-level-id=640029` (High, level 4.1), `1920x1080`, ~30 fps,
  `packetization-mode=1`, `sprop-parameter-sets=Z2QAKa0AzoB4AiflmoCAgPgAAAMACAAAAwHhgQAAtxsAAESqP//gUA==,aO48sA==`
- Audio: `AAC-LC` (`mode=AAC-hbr`, `profile-level-id=15`, `config=1408`), 16000 Hz

The camera's RTSP `DESCRIBE` SDP is well-formed and **does include
`sprop-parameter-sets`** (see verbose-ingress log below).

## Symptom

- go2rtc consumer (WebRTC) connected (`ws+udp`, ICE candidate `prflx`) but the
  video element stayed **black** — no error, no frames.
- go2rtc's HTTP/HLS "probe" consumer delivered the **same** `H264 High` + `AAC`
  stream to the browser successfully (the `<video>` element played it). This proved
  the source, codec, and pipeline were healthy end-to-end.

## Investigation

1. **RTSP corruption fix** — earlier RTSP interleaved/UDP corruption was fixed and
   committed as `f665ef5` on branch `stable` of this repo
   (`src/VideoWorker.cpp`, `src/simple-rtsp/*`). After that, the RTSP pull was
   stable; the earlier `EOF` was a one-off during a prudynt rebuild.
2. **Codec-profile red herring** — the camera was switched from `Baseline` to
   `High` via `jct /etc/prudynt.json set stream0.profile 2` (suspecting a Chrome
   WebRTC H.264 / OpenH264 issue). This did **not** fix go2rtc, and MediaMTX plays
   `High` fine — so the profile was never the cause.
3. **Pivot to MediaMTX** — the go2rtc ffmpeg transcode template is broken in this
   dev build (`video=vp8` / `video=libvpx` produce a malformed ffmpeg command:
   `Unable to choose an output format for 'vp8'`), so forcing Chrome-native VP8/VP9
   as a workaround was impossible without fixing go2rtc.
4. **MediaMTX WebRTC works** — opening the MediaMTX WebRTC player rendered the
   camera. Since this is the **identical H.264 High + AAC stream** that go2rtc's
   WebRTC rendered black, the failure is go2rtc-specific, and Chrome's WebRTC H.264
   decode is fine.
5. **MoQ and HLS also verified** through MediaMTX (see Endpoints).

## Root cause (go2rtc)

Ruled out:

- **Not the camera / codec / pipeline** — go2rtc's own HTTP/HLS consumer played the
  same stream; MediaMTX plays it over WebRTC.
- **Not Chrome's WebRTC H.264 / OpenH264** — MediaMTX serves the same H.264 and it
  decodes and renders.
- **Not missing `sprop-parameter-sets` in the source** — the camera RTSP SDP includes
  them (correcting an earlier hypothesis).

Conclusion:

- The failure is in **go2rtc's WebRTC H.264 track / SDP-offer construction** for this
  particular stream. The consumer connects and the session is established, but the
  browser's H.264 decoder never produces frames (consistent with an offer whose H.264
  codec config isn't relayed correctly, or a mis-packaged H.264 RTP relay). The
  session produces no error, which is why it surfaced only as a black video.
- The exact mechanism was not captured (no SDP/packet trace taken). It is irrelevant
  now that MediaMTX is the server.
- Independently, this go2rtc dev build cannot transcode to VP8/VP9
  (`video=vp8`/`libvpx` template bug), removing the obvious workaround.

## Solution: MediaMTX

Container (podman) on the streaming host `192.168.88.20`, image
`docker.io/bluenviron/mediamtx:latest` (v1.19.2). Config at
`/home/paul/Files2/containers/mediamtx/mediamtx.yml`.

Run:

```bash
podman run -d --name mediamtx --restart unless-stopped \
  -p 8654:8654 -p 8889:8889 -p 8189:8189/udp -p 9997:9997 -p 8888:8888 \
  -p 8892:8892 -p 8892:8892/udp \
  -v /home/paul/Files2/containers/mediamtx/mediamtx.yml:/mediamtx.yml \
  docker.io/bluenviron/mediamtx:latest /mediamtx.yml
```

Config (`mediamtx.yml`):

```yaml
logLevel: info
rtspAddress: :8654
rtspTransports: [tcp, udp]
webrtc: true
webrtcAddress: :8889
webrtcAdditionalHosts: ["192.168.88.20"]
api: true
apiAddress: :9997
authInternalUsers:
  - user: any          # open auth — local trusted net only
    pass:
    ips: []
    permissions:
      - action: publish
        path:
      - action: read
        path:
      - action: playback
        path:
      - action: api
        path:
      - action: metrics
      - action: pprof
paths:
  cam:
    source: rtsp://thingino:thingino@192.168.88.31:554/ch0
    sourceOnDemand: true
```

### Notes

- **Auth is open** (`any` with no password, all permissions). Acceptable only on a
  trusted LAN. Add a password or restrict `ips` before exposing beyond it.
- **MoQ** (`:8892`) is **HTTPS-only**. MediaMTX auto-generates a self-signed
  certificate; the browser must accept it. Both the TCP/HTTP2 (`:8892`) and
  UDP/HTTP3 (`:8892`) listeners must be reachable (WebTransport/QUIC).
- **HLS** (`:8888`): the stream loads but playback is blocked by the browser
  **autoplay policy** for media-with-audio. A single click / play gesture starts it
  (muted autoplay would start on its own, but the MediaMTX HLS page does not force
  `muted`).

## Endpoints (host 192.168.88.20)

| Protocol | URL                                                  | Notes |
|----------|------------------------------------------------------|-------|
| WebRTC   | `http://192.168.88.20:8889/cam/`                     | Works, autoplays |
| MoQ      | `https://192.168.88.20:8892/cam`                     | Needs `https` + cert accept |
| HLS      | `http://192.168.88.20:8888/cam/`                     | Works; click to start (autoplay policy) |
| RTSP     | `rtsp://192.168.88.20:8654/cam`                      | Republish of the camera |
| API      | `http://192.168.88.20:9997/v3/paths/list`           | JSON path list |

## Ingress health

Verbose (`logLevel: debug`) logs of the RTSP pull from the camera show **zero**
WARN/ERROR. The handshake is clean:

```
OPTIONS -> 200
DESCRIBE -> 200
SETUP x2 -> 200
PLAY -> 200
[path cam] stream is available and online, 2 tracks (H264, MPEG-4 Audio)
```

The camera SDP is standard (H.264 `packetization-mode=1`,
`profile-level-id=640029`, `sprop-parameter-sets=...`; AAC `mode=AAC-hbr`,
`profile-level-id=15`, `config=1408`). Full debug log was captured at
`/tmp/mediamtx_debug.log` during the investigation.

## Open items

- If go2rtc is ever revived as the streaming server, the fix is in its WebRTC H.264
  track construction (ensuring the H.264 codec config / `sprop-parameter-sets` is
  forwarded into the WebRTC offer), not in the camera or Chrome.
- A VP8/VP9 transcode fallback (Chrome-native decode) would require ffmpeg. The
  MediaMTX image has no ffmpeg; it relays H.264 directly, which works because
  Chrome's WebRTC H.264 decode is functional.
