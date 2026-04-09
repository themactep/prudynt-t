# Stability Improvement Roadmap

This document tracks the back-port of stability and timestamp improvements from
[Prudynt-SE](https://github.com/prudynt-t/Prudynt-SE) into prudynt-t. The goal
is to improve multi-client RTSP reliability, correct startup timestamps, and
reduce timing-related crashes — while **preserving all existing features**
(MP4 recording, audio output, day/night, MJPEG, privacy masking, etc.).

---

## Phase 1 — TimestampManager ✅ Done

**Problem:** `VideoWorker` and `AudioWorker` stamp frames with `gettimeofday()`
which is wall-clock time and subject to NTP jumps. This can produce negative
deltas or large forward jumps that confuse downstream consumers.

**Fix:** Introduce a singleton `TimestampManager` that wraps
`IMP_System_GetTimeStamp()` — the same hardware-monotonic clock the IMP encoder
uses for its own `pack_ts_us` values. All frame timestamps now share one source
of truth.

**Files changed/added:**
- `src/TimestampManager.hpp` — new singleton class
- `src/TimestampManager.cpp` — implementation
- `src/IMPSystem.cpp` — call `TimestampManager::getInstance().initialize()` after `IMP_System_Init()`
- `src/VideoWorker.cpp` — replace `gettimeofday(&nalu.time, ...)` with `TimestampManager`
- `src/AudioWorker.cpp` — replace `gettimeofday(&af.time, ...)` with `TimestampManager`

---

## Phase 2 — H264 SPS Timing Patch ✅ Done

**Problem:** The SPS NAL unit sent in the RTSP `SETUP` response does not carry
`timing_info_present_flag`, so many decoders (ffprobe, VLC, some ONVIF clients)
cannot determine the frame rate from the stream itself and must guess. This leads
to incorrect RTP-Info offset calculations and negative DTS/PTS values at startup.

**Fix:** Port `H264TimingPatch` from Prudynt-SE. After the SPS is captured from
the encoder (either from the parameter cache or the live stream), call
`patch_h264_sps_timing()` to write `timing_info_present_flag = 1` and the
correct `num_units_in_tick / time_scale` pair derived from the configured FPS.

**Files changed/added:**
- `src/H264TimingPatch.hpp` — new (ported from SE)
- `src/H264TimingPatch.cpp` — new (ported from SE, full bit-level SPS rewriter)
- `src/RTSP.cpp` — call `patch_h264_sps_timing(sps.data, stream.fps)` before
  handing the SPS to `IMPServerMediaSubsession::createNew()`

---

## Phase 3 — ParameterCache Struct Upgrade ✅ Done

**Problem:** On RTSP startup `RTSP::addSubsession()` calls
`msgChannel->wait_read()` in a loop to fish out the SPS/PPS/VPS NAL units. This
**consumes and discards frames** from the shared channel, causing the first few
video frames to never reach any RTSP client. Under multi-client load the race is
worse because each client setup drains its own frames.

`IMPServerMediaSubsession::sdpLines()` holds `codec_config_mutex` to compare
the raw `latest_sps`/`latest_pps`/`latest_vps` byte vectors. The fields are
unguarded booleans (`have_sps` etc.) read without the lock.

**Fix:** Replace the flat fields in `video_stream` with a proper
`video_parameter_cache` struct (ported from SE) that contains:
- A single `std::mutex` + `std::condition_variable`
- Full `H264NALUnit` entries for SPS, PPS, VPS, and latest keyframe
- `last_idr_us` — hardware timestamp of most recent IDR
- `profile_idc` / `level_idc` — parsed from SPS for the timing patch

`VideoWorker` writes to the cache and notifies the condition variable.
`RTSP::addSubsession()` waits on the condition variable instead of consuming
frames, so zero live frames are lost on startup.

**Files changed:**
- `src/globals.hpp` — add `video_parameter_cache` struct; replace flat fields in
  `video_stream`
- `src/VideoWorker.cpp` — write SPS/PPS/VPS + IDR timestamps into `parameterCache`
- `src/RTSP.cpp` — add `wait_for_parameter_sets()` helper; use cache on startup
- `src/IMPServerMediaSubsession.cpp` — update `sdpLines()` to read from
  `parameterCache`

---

## Phase 3.5 — Runtime Stability Fixes ✅ Done

**Problem:** On-device testing revealed several runtime crashes and performance issues:
- Exit-time SIGSEGV in backchannel teardown due to incorrect destruction order
- 26-second hang on exit waiting for websocket thread to stop
- Missing timestamps in console logs made debugging timing issues difficult
- Extra blank lines in libwebsockets log output

**Fix:** Targeted runtime hardening based on on-device crash reports:
- Fix `BackchannelStreamState` destructor order: close `mediaSink` before `rtpSource`
- Add `WS::stop()` method with `lws_cancel_service()` to wake the service loop
- Add timestamped console logging with millisecond precision
- Trim trailing newlines from libwebsockets log bridge
- Add diagnostic breadcrumbs in ISP init and RTSP/media-source setup paths

**Files changed:**
- `src/Logger.cpp` — add timestamp formatting using stack buffers + `fprintf`
- `src/BackchannelStreamState.cpp` — fix teardown order
- `src/WS.hpp` / `src/WS.cpp` — add `stop()` method and atomic stop flag
- `src/main.cpp` — call `ws.stop()` before joining websocket thread
- `src/IMPSystem.cpp` — add ISP init breadcrumbs for fault isolation
- `src/RTSP.cpp`, `src/IMPServerMediaSubsession.cpp`, `src/IMPDeviceSource.cpp`,
  `src/BackchannelServerMediaSubsession.cpp` — add diagnostic logging

**Validation:** Binary now starts cleanly, runs without crashes, and exits fast (<1s).

---

## Phase 4 — StreamCore Pub-Sub (Planned)

**Problem:** The `VideoTapEntry` / `AudioTapEntry` tap mechanism uses
`std::weak_ptr<MsgChannel<T>>` — if a client disconnects between the `notify`
call and the actual read, the `weak_ptr` is already expired and the notification
is silently dropped. Under multi-client stress this produces missed-frame
bursts. All subscribers share one `MsgChannel` which has a fixed depth; a slow
client causes faster clients to drop frames.

**Fix:** Replace the tap mechanism with `StreamCore<T>` from Prudynt-SE.
Each subscriber receives an independent `Cursor` with per-subscriber sequence
tracking. A slow client falls behind without affecting others. The producer
publishes atomically to all cursors via callbacks. Start policies (`LiveEdge`,
`LatestSync`) allow RTSP clients to start cleanly on a keyframe boundary.

`src/StreamCore.hpp` is already present in this branch as preparation.

**Files to change (not yet started):**
- `src/globals.hpp` — replace `MsgChannel` + tap vectors with `StreamCore`
- `src/VideoWorker.cpp` — call `streamCore.publish()` instead of `msgChannel->write()`
- `src/AudioWorker.cpp` — same for audio
- `src/IMPDeviceSource.cpp` — register/unregister `StreamCore::Cursor` instead
  of tap callbacks
- `src/MP4Recorder.cpp` — subscribe via `StreamCore::Cursor`

**Complexity:** High — touches every producer and every consumer. Recommend a
dedicated branch and integration tests before merging.

---

## Validation Checklist

After each phase, verify with the target hardware:

- [ ] `ffprobe rtsp://<ip>/ch0` — clean output, no negative DTS/PTS
- [ ] `ffmpeg -i rtsp://<ip>/ch0 -t 20 /dev/null` — 20 seconds without warnings
- [ ] VLC opens stream, plays back without stutter
- [ ] Two simultaneous clients (e.g. ffplay + VLC) — neither drops packets
- [ ] MP4 recording produces valid files with correct duration
- [ ] WebSocket / HTTP MJPEG / JSON API continue to function
- [ ] Reboot then immediate RTSP connect — no startup crash
