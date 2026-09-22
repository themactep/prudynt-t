Video
=====

Framesource Buffers (`stream0.buffers` / `stream1.buffers`)
------------------------------------------------------------

Each framesource channel has a ring of DMA buffers (`nrVBs`) shared between
the sensor and the encoder.  Too few buffers stall the sensor while the
encoder holds the only available slot, cutting the delivered frame rate in
half (or worse).

### Auto-calculation (`-1`, the default)

Setting `buffers` to `-1` tells prudynt to choose the count at runtime:

1. **FPS-based target** — `ceil(fps / 8)`, minimum 2.  
   Gives 2 at ≤15 fps, 4 at 25–30 fps, 8 at 60 fps.
2. **RAM cap** — the result is capped so total framesource memory for that
   channel stays within **~15 % of total RAM**, protecting low-memory devices.

Effective values by common configuration:

| Device RAM | Resolution  | FPS | Buffers | Framesource mem |
|-----------|-------------|-----|---------|-----------------|
| 64 MB     | 1920 × 1080 | 30  | 3       | ~8 MB           |
| 64 MB     | 1920 × 1080 | 15  | 2       | ~6 MB           |
| 64 MB     | 1280 × 720  | 30  | 4       | ~5 MB           |
| 128 MB    | 1920 × 1080 | 30  | 4       | ~12 MB          |
| 128 MB    | 1280 × 720  | 30  | 4       | ~5 MB           |

If `/proc/meminfo` is unreadable, prudynt conservatively assumes 64 MB.
The minimum is always 2 (double-buffering), regardless of RAM.

The chosen count is logged at startup:

```
[INFO:IMPFramesource.cpp]: Channel 0: auto buffers=4 (fps=30, frame=2970KB, RAM=128MB)
```

### Manual override

Set an explicit integer (1–8) to bypass auto-calculation entirely:

```json
"stream0": { "buffers": 4 }
```

Use this when you need to trade memory for throughput on an unusual workload,
or to reproduce a specific buffer depth for debugging.

Bitrate Auto-Default (`stream0.bitrate` / `stream1.bitrate`)
------------------------------------------------------------

Setting a stream bitrate to `0` makes prudynt derive it from the encoded
geometry instead of passing it to the encoder:

```
kbps = general.bitrate_auto_bppf * scale * width * height * fps / 1000
```

- `general.bitrate_auto_bppf` is bits per pixel per frame. Default `0.067`,
  the SDK's own level, about 1 Mbps per megapixel at 15 fps.
- `scale` is `1.0` for `stream0` and `2.0` for `stream1`. The substream gets
  the higher factor because the Web UI shows it upscaled, where a low bitrate
  is obvious.
- The result is rounded to the nearest 100 kbps and clamped to `[256, 8000]`.

Effective values at the default `0.067`:

| Stream  | Resolution  | FPS | Auto bitrate |
|---------|-------------|-----|--------------|
| stream0 | 1920 × 1080 | 15  | 2100 kbps    |
| stream1 | 960 × 540   | 15  | 1000 kbps    |
| stream1 | 1280 × 720  | 15  | 1900 kbps    |
| stream1 | 640 × 360   | 15  | 500 kbps     |

An explicit non-zero bitrate always wins and is passed to the encoder
unchanged. To retune the auto values without rebuilding, set
`general.bitrate_auto_bppf` (e.g. `0.08` puts a 960 × 540 substream at
1200 kbps).

The chosen value is logged at startup:

```
[INFO:IMPSystem.cpp]: stream1: bitrate auto from 960x540@15 -> 1000 kbps (bppf=0.067, scale=2)
```

Video Privacy
-------------

Prudynt exposes `/run/prudynt/video_ctrl` for coarse privacy control. Each
newline-delimited command toggles a full-frame OSD cover on one of the encoder
channels, forcing RTSP, MP4 recordings, and JPEG taps to see a solid black
stream while the ISP and exposure pipelines keep running.

```
printf 'PRIVACY ch=0 value=on\n' > /run/prudynt/video_ctrl
printf 'PRIVACY ch=0 value=off\n' > /run/prudynt/video_ctrl
```

- `ch=` selects the encoder. Omitting it (or using `ch=all`) toggles every encoder;
    set `ch=0`, `ch=1`, etc. to target a single stream.
- `value=`/`state=` accepts `on|off`, `true|false`, or `1|0`.
- Commands are idempotent; repeating the same state is a no-op.
- When the worker is not yet running the request is latched and applied as
	soon as the stream boots.

Internally the privacy state uses a hardware OSD cover layer, so frame cadence
and timestamps remain monotonic and decoders see legal access units (bitrates
typically collapse to a few hundred bits/s while muted).

RTSP Streaming
--------------

See [rtsp.md](rtsp.md) for details on the RTSP server architecture,
timestamp handling, and MJPEG preview interaction.
