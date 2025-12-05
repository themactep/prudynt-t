Audio
=====

Audio Output FIFO
-----------------

Prudynt exposes a text-based FIFO at `/run/prudynt/audio_out` whenever `audio.output_enabled` is true. Each line written into the FIFO is treated as a standalone command:

- `PLAY <path> [format=pcm|wav|aac|opus|mp3|flac] [rate=16000] [vol=NN] [gain=NN] [append=0|1] [loop=1] [delay=0]` – Plays a 16-bit mono PCM file. WAV files are auto-detected (or `format=wav`) and their embedded sample-rate is honored; raw PCM defaults to the configured `audio.output_sample_rate` unless `rate=` is provided. AAC/ADTS files can be auto-detected by their `.aac`/`.adts` suffix or forced with `format=aac`; Opus-in-Ogg assets (`.opus`/`.oga`/`.ogg`) can likewise be auto-detected or explicitly selected with `format=opus`. MP3 assets (`.mp3`, `.mp2`, `.mpeg`) are auto-recognized through their extensions or by scanning for ID3/sync words, and FLAC files (`.flac`) are detected by the `fLaC` marker; either format can also be forced via `format=mp3`/`format=flac`. If no helpful extension is present the FIFO inspects file contents, looking for RIFF/WAVE, ADTS sync words, Ogg BOS pages with `OpusHead`, FLAC magic, or MP3 sync words to pick the right decoder. Non-mono inputs are averaged to mono and resampled to the configured output rate, and codec-specific metadata (Opus pre-skip, FLAC sample-rate, MP3 channel count) is honored before enqueueing. When `append=0` (default) the pending queue is cleared before playback. `loop=N` re-queues the same asset up to 32 times (default 1) before moving on, `delay=MS` inserts up to 5000 ms of silence between loops, and volume/gain options apply before the first chunk.
- `STOP` – Flushes the playback queue and calls `IMP_AO_FlushChnBuf`, effectively interrupting whatever is queued.
- `VOLUME <value>` / `GAIN <value>` – Immediate adjustments without requiring audio data; values are clamped to the same ranges accepted in `thingino.json` (volume 0–100, gain 0–31).
- `SET volume=<value> gain=<value>` – Convenience variant that can set one or both parameters on a single line.

Commands may also be written using `key=value` tokens separated by whitespace, mirroring the MP4 control FIFO style. Responses are logged via `AudioOutputControl`, so invoking `tail -f /var/log/prudynt.log` helps diagnose parse failures.

Decoder Coverage
-----------------

The firmware now ships four fixed-point decoder families for FIFO playback:

- Helix AAC (HE-AAC/LATM) via `libhelix-aac`, used for `format=aac` and ADTS detection.
- Helix MP3 via `libhelix-mp3`, used for `format=mp3`/`format=mpeg` inputs.
- Xiph FLAC via the lightweight port bundled in `libflac`, used for `format=flac`.
- Xiph Opus via upstream `libopus`, used for `format=opus` (Ogg-wrapped) inputs.

Each decoder installs its shared object into `/usr/lib` and exposes headers in staging, so prudynt links dynamically without adding extra runtime daemons. If a format is unavailable it can be toggled off in Buildroot by deselecting the corresponding `BR2_PACKAGE_LIBHELIX_*`, `BR2_PACKAGE_LIBFLAC`, or `BR2_PACKAGE_OPUS` option.

Microphone Tap
---------------

Prudynt can expose the live microphone feed over a local FIFO so that wake-word
detectors or other tooling can consume raw PCM without touching RTSP or the
network stack. Set the following keys in `/etc/prudynt.json` and restart the
service:

```
jct /etc/prudynt.json set audio.tap_enabled true
jct /etc/prudynt.json set audio.tap_path /run/prudynt/audio_in.pcm   # optional
service prudynt restart
```

- The FIFO defaults to `/run/prudynt/audio_in.pcm` (mode `0660`).
- Audio is always little-endian signed 16-bit PCM and mirrors the configured
  `audio.mic_sample_rate` (legacy `audio.input_sample_rate`) and channel count
  (typically 16 kHz mono).
- The writer never blocks: if no reader is attached or a consumer falls behind,
  samples are dropped so the capture thread keeps up.

Reading from the tap is just standard FIFO consumption. For example, to monitor
the stream with SoX:

```
sox -t raw -b 16 -e signed-integer -c 1 -r 16000 \
    /run/prudynt/audio_in.pcm -d
```

You can also point analysis tools (e.g. `porcupine`, `sonic-pi`) at the FIFO or
pipe it into another process:

```
cat /run/prudynt/audio_in.pcm | your-detector --rate 16000 --format s16le
```

Disable the tap by setting `audio.tap_enabled` back to `false`. The FIFO is
removed automatically when Prudynt stops or restarts.

References
----------

- WebM Project
  https://www.webmproject.org/

- Common MIME types
  https://developer.mozilla.org/en-US/docs/Web/HTTP/MIME_types/Common_types

- Ogg Media Types
  https://datatracker.ietf.org/doc/html/rfc5334

- RTP Payload Format for Vorbis Encoded Audio
  https://datatracker.ietf.org/doc/html/rfc5215

- The audio/mpeg Media Type
  https://datatracker.ietf.org/doc/html/rfc3003

- WAVE and AVI Codec Registries
  https://datatracker.ietf.org/doc/html/rfc2361

- The Audio/L16 MIME content type
  https://datatracker.ietf.org/doc/html/rfc2586

- RTP Payload Format for 12-bit DAT Audio and 20- and 24-bit Linear Sampled Audio
  https://datatracker.ietf.org/doc/html/rfc3190
