# On-Screen Display Enhancements

## Integrated Brightness Meter

Prudynt now derives the ambient brightness that controls the day/night toggle directly from the Ingenic ISP statistics instead of the external `daynightd` daemon. The meter samples `/proc/jz/isp/isp-m0` (with a `/tmp/test-isp-m0` fallback for development boards), compensates for the current integration time and gain, and smooths the data across a small history window. When ISP data is unavailable it falls back to a simple time-of-day heuristic, so the overlay never goes blank.

### New configuration keys

Each stream inherits the following options under `streamX.osd` (see `res/prudynt.json` for defaults):

- `brightness_enabled`: turns the overlay on/off (disabled by default).
- `brightness_position`: `"x,y"` anchor (same syntax as other OSD elements; use negative values to offset from the right/bottom edge).
- `brightness_rotation`: optional rotation in degrees.
- `brightness_font_color` / `brightness_font_stroke_color`: RGBA colors encoded as `#RRGGBBAA`.
- `brightness_format`: text template. Supported tokens:
  - `%b` — instantaneous brightness percentage.
  - `%a` — smoothed average brightness.
  - `%m` — current operating mode reported by the ISP (e.g. `DAY`, `NIGHT`).
  - Use `%%` to render a literal percent sign.

Example (stream0):

```jsonc
"osd": {
  "brightness_enabled": true,
  "brightness_format": "Brightness:%b%% Avg:%a%% %m",
  "brightness_position": "10,70",
  "brightness_font_color": "#FFFFFFFF",
  "brightness_font_stroke_color": "#000000FF"
}
```

### Usage notes

1. Enable the overlay per stream, restart Prudynt, and the OSD thread will refresh the brightness text once per second alongside the existing time/user/uptime widgets.
2. No additional services or FIFOs are required; removing `daynightd` or its `/run/daynight/value` export will not affect the overlay.
3. If you are experimenting on a host without live ISP stats, drop a captured `/proc/jz/isp/isp-m0` dump at `/tmp/test-isp-m0` to feed the meter.
