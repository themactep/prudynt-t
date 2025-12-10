# MP4 Control FIFO

Prudynt exposes a lightweight FIFO so other processes can trigger recordings without touching internal APIs. The control endpoint lives at:

```
/run/prudynt/mp4ctl
```

The FIFO is created on startup (the daemon creates `/run/prudynt` if necessary). Writing plain-text commands to the FIFO controls per-channel MP4 recorders.

While a channel is actively recording, Prudynt also creates a small state file:

```
/run/prudynt/mp4ctl-ch<channel>.active
```

Scripts can watch for the presence/removal of this file to know precisely when the muxer has finished writing (more reliable than sleeping for the requested duration).

## START command

```
START <path> [duration] [channel]
START path=<absfile> dur=<seconds> ch=<channel>
START mount=<root> dir=<subdir> name=<strftime> dur=<seconds> ch=<channel> loop=1
```

- **path**: absolute file path for a one-shot recording. Parents must already exist unless you rely on `mount/dir/name`.
- **mount**/**dir**/**name**: when `loop=1`, Prudynt builds each filename by combining the mount point, optional directory, and an `strftime(3)` template. Missing parents are created automatically and `.mp4` is appended when the template does not specify an extension.
- **duration**: recording length in seconds. If omitted or `<= 0`, the recorder keeps running until a `STOP`. Looped sessions must supply a positive duration so clips rotate predictably.
- **channel**: video encoder channel (`0` or `1`). Defaults to `recorder.channel` in `prudynt.json`.
- **loop**: enable seamless looped recording by setting `loop=1`. Omit the flag (or set `loop=0`) for single clips.
- Named arguments may appear in any order; positional arguments fill path, duration, then channel for backward compatibility.
- Every START request is validated against `/proc/mounts`. Prudynt only records onto writable mounts and rejects paths that would land on the root/overlay filesystem, even if a script bypasses `/sbin/record` and writes directly to the FIFO.

### Behaviour

- Only one MP4 recorder may run per channel. If a START arrives while a channel is already recording, the request is rejected and the existing session continues.
- When duration > 0, MP4ControlSocket sets an auto-stop timer; overlapping STARTs no longer cancel the timer.
- START wakes the requested video worker, forces the encoder active, requests an IDR, and waits for SPS/PPS before writing the init segment.
- With `loop=1`, Prudynt takes over scheduling: as soon as one clip finishes (auto-stop timer fires), the next filename is created and recording restarts without a shell-side delay. `STOP` cleanly terminates the loop and any in-flight segment.
- Looped sessions are normalized to whole-minute boundaries. The first clip starts immediately and runs until the next `HH:MM:00` (or the following minute when fewer than ~15 seconds remain) so that all subsequent clips start exactly on the minute. Durations that are not multiples of 60 seconds may introduce padding while the recorder realigns to the minute grid.

## STOP command

```
STOP
STOP <channel>
STOP ch=<channel>
```

- Without arguments: stops every active recorder.
- With a channel index: stops that channel only. Cancels its auto-stop timer before shutting down the muxer.

## Example usage

Trigger a 20-second recording on channel 1:

```sh
printf 'START /mnt/nfs/event.mp4 20 1\n' > /run/prudynt/mp4ctl
```

Stop channel 0 (including any loop) immediately:

```sh
printf 'STOP ch=0\n' > /run/prudynt/mp4ctl
```

## Diagnostics

- Control events are logged at `INFO` level (`MP4ControlSocket.cpp`).
- Each recorder logs when it starts/stops and when the auto-stop timer fires, making it easy to trace expected durations.
- If START fails, check the surrounding log lines for reasons such as missing SPS/PPS, invalid path, or a busy channel.

## Related scripts and config

- `package/prudynt-t/files/record` is the user-facing CLI. It reads the `recorder` domain in `/etc/prudynt.json` (falling back to `/etc/recorder.json` for legacy keys) to assemble START options and writes them to `/run/prudynt/mp4ctl`. The `recorder.device_path` field understands the `%hostname` placeholder, which the script expands before sending requests.
- `package/prudynt-t/files/S98recorder` is the init hook that calls `/sbin/record` once Prudynt is ready and issues `STOP` during shutdown.
- Custom automation can write to `/run/prudynt/mp4ctl` directly; the motion example in `overlay/lower/usr/sbin/motion` shows a simple START trigger.

Use the shared FIFO paths above so Prudynt sees a consistent control stream.
