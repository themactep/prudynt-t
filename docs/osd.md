On-Screen Display
=================

Prudynt provides two independent OSD layers:

1. **Burn-in timestamp** — rendered into the video stream by the IMP hardware
   OSD (OSD_REG_PIC region). Always visible in recordings and RTSP streams.
2. **SEI OSD** — text metadata embedded in H.264/H.265 SEI NAL units. Rendered
   by the receiving client (browser overlay, go2rtc, etc.).


Burn-in Timestamp Overlay
--------------------------

A hardware-rendered timestamp burned directly into each video frame. Uses an
embedded 5×7 bitmap font — no external dependencies.

Enable with `osd.burnin.enabled` in `prudynt.json` and rebuild with
`USE_OSD_BURNIN=1` (set via `BR2_PACKAGE_PRUDYNT_T_OSD_BURNIN` in Buildroot).

### Configuration

All keys under `osd.burnin`:

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `enabled` | bool | `false` | Enable burn-in overlay |
| `format` | string | `"%F %T"` | `strftime` format string |
| `scale` | int | `0` (auto) | Font scale 1–10; 0 = auto from stream width |
| `background` | bool | `false` | Semi-transparent dark background box |
| `fill_color` | string | `"#ffffffff"` | Glyph fill color `#RRGGBBAA` |
| `outline_color` | string | `"#000000ff"` | Glyph outline/halo color `#RRGGBBAA` |

Scale is automatically derived from stream width when set to 0: `width / 480`
clamped to [1, 10]. At 1920×1080 this gives scale 4; at 640×360 scale 1.

The overlay automatically appends "PRIVACY" when the privacy screen is active
on that channel. The timestamp sits at layer 2 (above the privacy cover at
layer 1) so it remains visible even with the privacy screen enabled.

### Embedded 5×7 Font

Defined in `src/Font5x7.hpp` (namespace `font5x7`). Covers:

- Digits `0`–`9`
- Symbols `-` `:` `+` (space)
- Uppercase `A`–`Z`

Each glyph is 5 pixels wide × 7 pixels tall, stored as 7 bytes where bit 4
(0x10) is the leftmost pixel. Glyphs are rendered with integer nearest-neighbor
scaling — pixel-perfect at any scale. A circular dilation outline provides a
soft halo around each glyph; thickness is `max(1, scale / 2)` output pixels.

### Color format

Colors use `#RRGGBBAA` hex strings (red, green, blue, alpha). Alpha `ff` is
fully opaque, `00` is fully transparent. The fill is drawn on top of the
outline, so a semi-transparent fill over an opaque outline creates a soft
glow effect.

### Example

```jsonc
"osd": {
  "burnin": {
    "enabled": true,
    "format": "%F %T",
    "scale": 4,
    "background": true,
    "fill_color": "#ffffffff",
    "outline_color": "#000000ff"
  }
}
```


SEI OSD Elements
----------------

Textual metadata embedded in H.264/H.265 SEI NAL units and RTP subtitle tracks
(t.140 / x-ass). Rendered by compatible clients via the `/x/json-osd-sei.cgi`
endpoint or the WebUI preview overlay.

Enable with `osd.enabled` in `prudynt.json`. Elements are configured as a
JSON object under `osd.elements`.

### Element types

| Type | Description | Default format |
|------|-------------|----------------|
| `timestamp` | Current date/time via `strftime` | `%F %T` |
| `hostname` | System hostname | `%s` |
| `ipaddress` | IP address (first non-loopback) | `%s` |
| `uptime` | System uptime | `%02lu:%02lu:%02lu` |
| `gain` | Brightness/gain from daynightd | `%s` |
| `text` | Static text | (from format field) |

### Element attributes

| Attribute | Description |
|-----------|-------------|
| `name` | Unique element ID |
| `type` | One of the types above |
| `format` | `strftime` or `printf` format string |
| `position` | `"x,y"` — negative values offset from right/bottom |

### Example

```jsonc
"osd": {
  "enabled": true,
  "elements": {
    "clock": {
      "type": "timestamp",
      "format": "%F %T",
      "position": "-10,-10"
    },
    "host": {
      "type": "hostname",
      "format": "%s",
      "position": "0,10"
    }
  }
}
```


Brightness Meter (legacy)
-------------------------

When `daynight.enabled` is true, brightness is derived from the embedded
Day/Night worker; otherwise a legacy direct ISP reader with a time-of-day
fallback is used. The `gain` OSD element type reads from this meter.

See the Day/Night documentation for worker configuration.


WebUI Controls
--------------

Both the OSD settings page (`/streamer-osd.html`) and the preview page
OSD modal provide controls for burn-in and SEI OSD:

- **Burn-in**: enabled toggle, `strftime` format, scale (auto or 1–10),
  fill/outline color with swatch + native color picker + alpha slider,
  background toggle.
- **SEI OSD**: enabled toggle, element list with add/remove, per-element
  type/format/position. Visual settings (font size, stroke width, colors)
  stored in browser localStorage.

Changes are saved to `/etc/prudynt.json` and applied immediately (OSD thread
restart).


API Endpoints
-------------

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/x/json-prudynt.cgi` | POST | Read/write OSD config (see `osd.burnin.*` and `osd.elements`) |
| `/x/json-osd-sei.cgi` | GET | Live SEI overlay data (rotation, elements) |
| `:8080/api/v1/osd-sei` | GET | Same as above, via Prudynt HTTP API |
