On-Screen Display
=================

Integrated Brightness Meter
---------------------------

Prudynt now derives the ambient brightness from the embedded Day/Night worker that continuously samples the Ingenic ISP EV counters and AWB gains via `hal::isp`. The worker normalizes those readings into a 0–100% scale, keeps running averages, and exposes live telemetry to every consumer (OSD, telemetry sockets, automation). When the worker is disabled the OSD transparently falls back to the legacy direct ISP reader with the time-of-day heuristic, so brightness never disappears.

Enable the worker by setting `daynight.enabled` in `prudynt.json` (see `res/prudynt.json` for defaults). The same section exposes the percent-based thresholds (`switch_below_percent`, `switch_above_percent`, `tolerance_percent`) plus advanced EV overrides if you need to match older scripts. The worker also keeps invoking `/sbin/daynight` (or the configured `daynight.script_path`) so IR-cut, illumination, and ISP running mode stay in sync with the overlay.

### Configuration keys

Each stream inherits the following options under `streamX.osd` (see `res/prudynt.json` for defaults):

- `brightness_enabled`: turns the overlay on/off (disabled by default).
- `brightness_position`: `"x,y"` anchor (same syntax as other OSD elements; use negative values to offset from the right/bottom edge).
- `brightness_rotation`: optional rotation in degrees.
- `brightness_fill_color` / `brightness_stroke_color`: RGBA colors encoded as `#RRGGBBAA`.
- `brightness_format`: text template. Supported tokens:
  - `%b` — instantaneous brightness percentage.
  - `%a` — smoothed average brightness.
  - `%m` — current operating mode reported by the ISP (e.g. `DAY`, `NIGHT`).
  - Use `%%` to render a literal percent sign.

### Example (stream0):

```jsonc
"osd": {
  "brightness_enabled": true,
  "brightness_format": "Brightness:%b%% Avg:%a%% %m",
  "brightness_position": "10,70",
  "brightness_fill_color": "#FFFFFFFF",
  "brightness_stroke_color": "#000000FF"
}
```

### Usage notes

1. Enable the overlay per stream, restart Prudynt, and the OSD thread will refresh the brightness text once per second alongside the existing time/user/uptime widgets.
2. When `daynight.enabled` is true no external `daynightd` service is required—the worker feeds both the IR-cut automation and the OSD overlay. If you disable the worker, the overlay keeps operating using the legacy ISP reader.
3. If you are experimenting on a host without live ISP stats, drop a captured `/proc/jz/isp/isp-m0` dump at `/tmp/test-isp-m0` to feed the meter.
