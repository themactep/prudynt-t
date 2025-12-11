Video
=====

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
