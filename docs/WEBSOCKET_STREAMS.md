## WebSocket API: Streams (Rate Control + Actions)

This document describes prudynt-t's WebSocket JSON API for configuring video stream rate control (RC) parameters and triggering on-demand IDR frames.

- Streams are addressed as JSON roots: `stream0`, `stream1`
- JPEG snapshot stream (`stream2`) is separate and not covered here
- All requests are JSON objects; responses echo the same structure with current values or status strings

### Keys summary

- Common (existing):
  - `enabled` (bool), `format` ("H264"|"H265"), `mode` (RC mode), `fps`, `gop`, `max_gop`, `bitrate`, `profile`, `width`, `height`, `stats` (null to read)
- New RC parameters:
  - `qp_init` (int, -1 or 0..51)
  - `qp_min`  (int, -1 or 0..51)
  - `qp_max`  (int, -1 or 0..51)
  - `ip_delta` (int, -1 or -20..20)
  - `pb_delta` (int, -1 or -20..20)
  - `max_bitrate` (int, 0 or >= 64000)
- New action:
  - `request_idr` (null trigger) — forces one IDR on that stream

Notes:
- "-1" (or 0 for `max_bitrate`) means: keep encoder defaults
- Changing RC parameters or `mode` triggers a minimal video restart to apply safely
- `ip_delta`/`pb_delta` apply on T31/C100/T40/T41 families; legacy platforms ignore them

### RC modes

- T31/C100/T40/T41: `FIXQP`, `CBR`, `VBR`, `CAPPED_VBR`, `CAPPED_QUALITY`
- T10/T20/T21/T23/T30: `FIXQP`, `CBR`, `VBR`, `SMART`

Input to `mode` is case-insensitive; it is stored in uppercase.

### Validation ranges

- `qp_init`, `qp_min`, `qp_max`: -1 or 0..51
- `ip_delta`, `pb_delta`: -1 or -20..20 (T31+ only)
- `max_bitrate`: 0 (no override) or 64000..100000000

### Platform behavior

- T31/C100/T40/T41: All new RC fields are supported per mode; `ip_delta`/`pb_delta` map to SDK fields
- Legacy (T10/T20/T21/T23/T30):
  - `qp_init` affects `FIXQP` (H264) initial QP
  - `qp_min`/`qp_max` apply to H264 `{CBR,VBR,SMART}` and H265 SMART (T30)
  - `max_bitrate` applies to H264 `{VBR,SMART}` and H265 SMART (T30)
  - `ip_delta`/`pb_delta` are ignored (no matching SDK fields)

### Examples

1) Set VBR mode with bounds and cap bitrate (T31+ or legacy where applicable)

Request:
```
{"stream0": {
  "mode": "vbr",
  "bitrate": 3000000,
  "qp_min": 24,
  "qp_max": 42,
  "max_bitrate": 4000000
}}
```

Response example:
```
{"stream0": {
  "mode": "VBR",
  "bitrate": 3000000,
  "qp_min": 24,
  "qp_max": 42,
  "max_bitrate": 4000000
}}
```

2) T31+: fine-tune deltas and initial QP for CBR

Request:
```
{"stream1": {
  "mode": "CBR",
  "bitrate": 1200000,
  "qp_init": 30,
  "ip_delta": 3,
  "pb_delta": 2
}}
```

Response example:
```
{"stream1": {
  "mode": "CBR",
  "bitrate": 1200000,
  "qp_init": 30,
  "ip_delta": 3,
  "pb_delta": 2
}}
```

3) Request an IDR on stream0

Request:
```
{"stream0": {"request_idr": null}}
```

Response:
```
{"stream0": {"request_idr": "initiated"}}
```

4) Read live stats (fps, Bps)

Request:
```
{"stream1": {"stats": null}}
```

Response example:
```
{"stream1": {"stats": {"fps": 25, "Bps": 512000}}}
```

### Operational notes

- Changes are persisted via the existing save_config action (`{"action": {"save_config": null}}`) or via your normal workflow. 
- Some settings may be constrained by the sensor/SoC; if an override cannot be applied, the encoder falls back to its internal default.
- If you see quality instability, consider tightening `qp_min/qp_max` and setting a reasonable `max_bitrate`.

