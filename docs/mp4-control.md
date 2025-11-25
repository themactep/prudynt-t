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
START <path> dur=<seconds> ch=<channel>
```

- **path** (required): absolute file path for the new MP4 file. Parents must already exist.
- **duration** (optional): recording length in seconds. If omitted or `<= 0`, the recorder keeps running until it receives a `STOP`.
- **channel** (optional): video encoder channel (0 or 1). Defaults to `0`.
- Named arguments `dur=`/`duration=` and `ch=`/`channel=` can appear in any order; unnamed arguments fill duration, then channel.

### Behaviour

- Only one MP4 recorder may run per channel. If a START arrives while a channel is already recording, the request is rejected and the existing session continues.
- When duration > 0, MP4ControlSocket sets an auto-stop timer; overlapping STARTs no longer cancel the timer.
- START wakes the requested video worker, forces the encoder active, requests an IDR, and waits for SPS/PPS before writing the init segment.

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

Stop channel 0 immediately:

```sh
printf 'STOP ch=0\n' > /run/prudynt/mp4ctl
```

## Diagnostics

- Control events are logged at `INFO` level (`MP4ControlSocket.cpp`).
- Each recorder logs when it starts/stops and when the auto-stop timer fires, making it easy to trace expected durations.
- If START fails, check the surrounding log lines for reasons such as missing SPS/PPS, invalid path, or a busy channel.

## Related scripts

- `overlay/lower/usr/sbin/motion` sends START commands when motion events fire.
- `package/prudynt-t/files/record` wraps long-running storage management and emits START/STOP commands according to its schedule.

These scripts already target `/run/prudynt/mp4ctl`; any custom tooling should do the same.
