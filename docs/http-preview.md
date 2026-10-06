# HTTP fMP4 Preview

Prudynt serves a live fragmented MP4 (fMP4) preview over its HTTP port
(`http.port`, default 8080). The stream reuses the running IMP encoder through
a video tap, so there is no second encode and no extra library on the device.

## Endpoints

| URL | Description |
|-----|-------------|
| `http://<ip>:8080/ch0.mp4` | Main stream, fragmented MP4 over chunked transfer |
| `http://<ip>:8080/ch1.mp4` | Sub stream (channel 1, if enabled) |
| `http://<ip>:8080/mjpg` | MJPEG (`/x/mjpg`, `/mjpeg`, `/x/mjpeg` are aliases) |

`/chN.mp4` answers with `Content-Type: video/mp4` and
`Transfer-Encoding: chunked`. The first chunk is the init segment
(`ftyp` + `moov`); every later chunk is one fragment (`moof` + `mdat`). The
bundled web UI opens these from `/preview.html` and `/preview-fmp4.html`.

The endpoint honours the same auth as the rest of the HTTP API: an API-key
token (`?token=...`) or a trusted client IP can bypass the password.

## Codec handling

The muxer follows the stream's configured `format`:

- **H.264**: SPS/PPS are read from the encoder's latest codec config and built
  into an `avcC`; the sample entry is `avc1`.
- **H.265**: VPS/SPS/PPS are read and built into an `hvcC`; the sample entry is
  `hvc1`. The server waits for the VPS as well, since it belongs in the decoder
  config. In-band parameter sets (NAL types 32/33/34) are dropped from the
  samples; VCL NALs are types 0..31 and IRAP key frames are 16..21.

A sample is one access unit: 4-byte big-endian length-prefixed NALs
(`lengthSizeMinusOne = 3`), matching the `avcC`/`hvcC` records. Parameter sets
stay in the init segment and are never repeated per frame.

The AAC audio track is muxed only when the mic is enabled, the input format is
AAC, and the audio tap is actually registered. Advertising a track that never
receives samples makes browsers drop the whole fragment stream.

## Browser playback

The bundled preview picks a path from the codec in the init segment:

- **Media Source Extensions (MSE)**: H.264 on every browser; H.265 only where
  MSE supports HEVC (Safari, Edge, Chromium with a hardware HEVC decoder). The
  page builds the RFC 6381 `hvc1...` codec string from the `hvcC` box and gates
  playback on `MediaSource.isTypeSupported`.
- **WebCodecs**: when MSE will not take H.265, the page demuxes the same
  fragments, feeds `VideoDecoder`, and paints frames to a canvas. WebCodecs is
  a secure-context API, so this path runs only over HTTPS or from localhost.

On plain HTTP with a browser that has neither MSE HEVC nor WebCodecs, H.265
preview is unavailable. Use the H.264 substream, or a browser with HEVC
support.

## Configuration

| Key | Default | Description |
|-----|---------|-------------|
| `http.enabled` | true | Master switch for the HTTP server |
| `http.port` | 8080 | Port for `/chN.mp4` and MJPEG |
| `http.mjpeg_enabled` | true | Serve the MJPEG endpoints |
| `http.api_enabled` | true | Serve the `/api/v1` endpoints |
| `http.auth_required` | true | Require auth unless the client IP is trusted |
| `http.username` / `http.password` | thingino | HTTP credentials |
