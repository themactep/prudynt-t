## MJPEG streaming (HTTP multipart) — pure sensor, adjustable size/quality/fps

This document describes prudynt-t's OSD-free MJPEG streaming path, designed for low latency and simple HTTP consumption.

- Transport: HTTP multipart/x-mixed-replace (aka MJPEG)
- Source: pure sensor frames (pre‑OSD), encoded by a dedicated JPEG pipeline (stream3)
- Adjustable per request: width, height, fps, JPEG quality
- Safe reconfiguration: width/height/fps are applied on demand; settings restore after client disconnect

---

### Endpoints

- Generic (recommended)
  - /x/video.mjpg?ch=0|1&f=FPS&q=QUALITY&w=WIDTH&h=HEIGHT
- Convenience (legacy-compatible)
  - /x/ch0.mjpg?f=FPS&q=QUALITY&w=WIDTH&h=HEIGHT
  - /x/ch1.mjpg?f=FPS&q=QUALITY&w=WIDTH&h=HEIGHT

Query parameters:
- ch: video channel (0 = stream0 source; 1 = stream1 source) [only for /x/video.mjpg]
- f: frames per second (int)
  - clamped to [1, cfg.sensor.fps]
- q: JPEG quality (int 1..100)
- w, h: desired output size (pixels)
  - Quantized to multiples of 16
  - Capped to source channel's width/height (no upscaling)

Response headers:
- Content-Type: multipart/x-mixed-replace; boundary=<boundary>
- Each part: Content-Type: image/jpeg; Content-Length: <bytes> followed by CRLFCRLF and JPEG payload

Notes:
- Reconfiguration (w/h/f) can cause a brief stutter while the encoder restarts.
- After client disconnect, the MJPEG encoder restores its previous width/height and fps from config.

---

### How it works (high level)

- Dedicated JPEG worker (stream3) is started at boot when enabled in config.
- The IPC server implements a MJPEG command that:
  - Validates/clamps parameters
  - Requests a dynamic reconfiguration from the JPEG worker (width/height/fps)
  - Streams a continuous sequence of JPEG frames to the IPC client (prudyntctl)
  - Restores original settings on disconnect
- CGI scripts call prudyntctl to expose this stream over HTTP via BusyBox httpd.

---

### Configuration

The dedicated MJPEG pipeline is defined as stream3 in prudynt.json. Example defaults:

- stream3.enabled: true
- stream3.jpeg_channel: 1         # 0 follows stream0 size, 1 follows stream1 size (source caps)
- stream3.jpeg_idle_fps: 5        # background capture rate when idle
- stream3.fps: 15                 # active rate (e.g., when streaming)
- stream3.jpeg_quality: 75        # default JPEG quality (per-request q overrides this)

Width/height for stream3 default to the selected source channel’s resolution at boot (no upscaling). Per-request w/h override at runtime via /x/video.mjpg.

Sensor FPS constraints are read from proc and made available in cfg.sensor.fps (max) and cfg.sensor.min_fps.

---

### HTTP endpoints (CGI)

- /x/video.mjpg — generic, supports ch, f, q, w, h
- /x/ch0.mjpg, /x/ch1.mjpg — convenience wrappers for fixed channel

Make sure CGI scripts are executable in the target filesystem (done by packaging), e.g.:
- chmod +x /www/cgi-bin/video.mjpg /www/cgi-bin/ch0.mjpg /www/cgi-bin/ch1.mjpg

---

### CLI usage

- Single snapshot (still JPEG):
  - prudyntctl snapshot -c 1 -q 75 > frame.jpg
- MJPEG to stdout (pass-through MIME):
  - prudyntctl mjpeg -c 1 -f 10 -q 70 -w 640 -h 360 > stream.mjpeg

prudyntctl mjpeg is a thin IPC client for the server-side MJPEG command; it does not assemble parts itself.

---

### Constraints and clamping

- FPS: min 1; max = cfg.sensor.fps (falls back to 30 if unknown). If requested fps > max, it is reduced to max.
- Size: w and h are rounded down to multiples of 16; minimum 16. Values are capped to the source channel’s current resolution.
- Quality: 1..100. Values outside are ignored (defaults remain).

---

### Behavior details

- OSD free: The MJPEG path uses a dedicated encoder pipeline that does not go through the OSD blending path.
- Reconfiguration latency: Changing w/h/f triggers encoder stop/init/start on the JPEG worker; clients will see a short stutter.
- Auto-restore: On client disconnect, width/height/fps revert to the encoder’s original config values.
- Multiple clients: Each HTTP request operates independently and triggers on-demand activity in the worker; ensure fps caps to avoid CPU overload.

---

### Examples

- Default channel 1 MJPEG @10 fps, q=70, 640x360:
  - http://<ip>/x/video.mjpg?ch=1&f=10&q=70&w=640&h=360
- Minimum CPU (low rate, small size):
  - http://<ip>/x/video.mjpg?ch=1&f=2&w=320&h=240
- Maximum within sensor fps (e.g., sensor 25fps):
  - http://<ip>/x/video.mjpg?ch=0&f=25

---

### Troubleshooting

- Channel 1 returns no frames:
  - Ensure stream3.enabled is true and stream3.jpeg_channel is set to 1 (or use ch=0).
  - Verify stream1 is configured and running (source size comes from stream1 at boot).
- Size requests ignored or clipped:
  - Check requested w/h are <= source resolution; sizes are snapped to multiples of 16.
- FPS not honoring requested value:
  - The server clamps fps between 1 and cfg.sensor.fps (see prudynt logs).
- MJPEG stutter when changing size:
  - Expected during reinit; request stable sizes/fps to avoid repeated reconfigs.

---

### Implementation pointers (for developers)

- Server command: IPCServer.cpp ("MJPEG" path)
- Worker reconfig: JPEGWorker.cpp (applies req_width/req_height/req_fps)
- Config: stream3 in Config.hpp/Config.cpp and res/prudynt.json
- CGI scripts: res/cgi-bin/video.mjpg, ch0.mjpg, ch1.mjpg
- CLI: prudyntctl mjpeg

All reconfigurations are bounded by hardware constraints (multiples of 16, no upscaling) and sensor FPS.

