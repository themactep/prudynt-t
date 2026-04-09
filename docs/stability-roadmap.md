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

## Phase 4 — StreamCore Pub-Sub ✅ Done

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

**Files changed:**
- `src/globals.hpp` — add `StreamCoreTraits` specializations; replace `msgChannel`
  + tap vectors with `videoCore`/`audioCore` in `video_stream`/`audio_stream`;
  remove `VideoTapEntry`/`AudioTapEntry` structs and tap functions
- `src/VideoWorker.cpp` — call `videoCore->publish()` instead of `msgChannel->write()`
  and tap iteration
- `src/AudioWorker.cpp` — call `audioCore->publish()` instead of `msgChannel->write()`
  and tap iteration
- `src/IMPDeviceSource.hpp` / `src/IMPDeviceSource.cpp` — add `cursor` member;
  register/unregister with StreamCore; use `cursor.read()` instead of `msgChannel->read()`
- `src/WS.cpp` — replace `preview_video_queue`/`preview_audio_queue` +
  `video_tap_entry`/`audio_tap_entry` with `video_cursor`/`audio_cursor`; register
  cursors with StreamCore; read SPS/PPS from parameter cache for MP4 init
- `src/main.cpp` — remove `msgChannel` initialization (now in stream constructors)
- `src/IMPAudioServerMediaSubsession.cpp` / `src/IMPServerMediaSubsession.hpp` —
  remove `msgChannel->clear()` calls (cursors start fresh automatically)

**Result:** Net -62 lines. Cleaner architecture. Improved multi-client stability.

---

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

---

## Phase 4.5: Critical Timestamp Fixes ✅ Done

**Problem 1:** Hardware validation revealed that RTSP clients (mpv, ffprobe) were showing
timestamp errors:
- "No video PTS! Making something up. Using 30.000000 FPS"  
- "Invalid audio PTS: X -> Y"
- "Audio/Video desynchronisation detected!"
- "Could not find codec parameters for stream 0 (Video: h264, none): unspecified size"

**Root Cause 1:** `IMPDeviceSource::deliverFrame()` was calling `gettimeofday()` for every
frame instead of using the hardware timestamps from `TimestampManager` that were
already captured in `nal.time` by `VideoWorker`/`AudioWorker`. This defeated the
entire purpose of Phase 1.

**Fix 1:** Use the pre-captured hardware timestamps from `TimestampManager` directly.

**Problem 2:** Even after using hardware timestamps, RTSP still showed "No video PTS!" errors.

**Root Cause 2:** Missing presentation time normalization. RTSP/RTP requires presentation
times to start near zero and progress monotonically, but we were sending raw wallclock
timestamps without normalization or anchoring.

**Fix 2:** Ported Prudynt-SE's presentation time normalization algorithm:
1. Establish anchor point on first frame: `anchor = source_timestamp - frame_duration`
2. Normalize all frames relative to anchor: `presentation = source - anchor`
3. Calculate and set `fDurationInMicroseconds` for proper RTP timing
4. Handle backwards jumps and zero timestamps gracefully

This ensures streams start at PTS=0 and progress smoothly, which RTSP/RTP demuxers require.

**Files changed:**
- `src/IMPDeviceSource.hpp` — added normalization state and method declaration
- `src/IMPDeviceSource.cpp` — implemented `normalizePresentationTimeUs()` and updated
  `deliverFrame()` to calculate duration and normalize timestamps

**Result:** RTSP streams now have proper presentation times starting at zero with
monotonic progression. Frame durations are calculated based on FPS (video) or
sample rate (audio). Net +26 lines.

---

## Phase 4.6: RTCP Timestamp Discontinuity Guard ✅ Done

**Problem:** Some clients (mpv/ffmpeg) intermittently reported large DTS jumps
mid-stream (e.g. `... packet N with DTS X, packet N+1 with DTS Y`) after RTSP
startup, even when frame delivery was otherwise stable.

**Root Cause:** Unlike Prudynt-SE, prudynt-t was leaving RTCP sender reports
enabled on video/audio RTP sinks. Certain client demuxers could re-anchor packet
timing when SR data arrived, producing discontinuity warnings and playback
instability.

**Fix:** Match Prudynt-SE behavior by disabling RTCP reports on RTP sinks:
- Video: H264/H265 sinks in `IMPServerMediaSubsession::createNewRTPSink()`
- Audio: AAC + generic audio sinks in `IMPAudioServerMediaSubsession::createNewRTPSink()`

**Files changed:**
- `src/IMPServerMediaSubsession.cpp`
- `src/IMPAudioServerMediaSubsession.cpp`

---

## Phase 4.7: Subscriber Timeline Reset + Audio Gap Clamp ✅ Done

**Problem:** Intermittent client runs still showed invalid A/V PTS after reconnects
or startup races, especially when source queues dropped old audio frames and the
next delivered timestamp jumped far ahead.

**Fixes:**
1. Reset timestamp normalization state when the first RTSP subscriber appears
   (audio and video workers), matching the SE pattern of re-anchoring timelines
   at subscriber transitions.
2. Add `duration_us` to `AudioFrame` and propagate it from `AudioWorker`.
3. In `IMPDeviceSource`, use subscriber-count-based `hasDataCallback` updates and
   clamp only large audio source timestamp deltas to the expected frame duration
   before presentation-time normalization.
4. Remove high-frequency timestamp debug logging in the hot path.

**Files changed:**
- `src/globals.hpp`
- `src/AudioWorker.hpp`
- `src/AudioWorker.cpp`
- `src/VideoWorker.cpp`
- `src/IMPDeviceSource.cpp`

---

## Phase 4.8: Require-Gated Backchannel SDP ✅ Done

**Problem:** Generic RTSP clients (e.g. mpv) were listing backchannel audio tracks
in DESCRIBE responses, even though backchannel should only be exposed to clients
that explicitly request ONVIF backchannel support.

**Root Cause:** `BackchannelServerMediaSubsession` stored a Require tag but SDP
generation never consumed request headers, so the backchannel subsession always
contributed SDP lines.

**Fix:** Add DESCRIBE-time Require tracking and gate backchannel SDP lines:
1. Parse `Require:` in a custom `PrudyntRTSPServer` client-connection override.
2. Propagate backchannel-request state into SDP generation context via a scoped env flag.
3. Return no SDP lines from backchannel subsession unless ONVIF backchannel was
   required by the current DESCRIBE request.

**Files changed:**
- `src/PrudyntRTSPServer.hpp`
- `src/PrudyntRTSPServer.cpp`
- `src/RTSP.cpp`
- `src/BackchannelServerMediaSubsession.cpp`

---

## Current Status

**Completed:** All 4 phases plus runtime fixes (3.5), critical timestamp fixes (4.5),
RTCP discontinuity guard (4.6), subscriber/timeline hardening (4.7), and
Require-gated backchannel SDP handling (4.8)
are implemented and building successfully. The binary starts cleanly, runs without
crashes, and exits quickly.

**Next Steps:**

1. **Hardware Validation** — Run the validation checklist below on target hardware
   to confirm all phases are working correctly in production. The critical timestamp
   fixes (Phase 4.5) should eliminate all RTSP timestamp errors. Monitor for:
   - Clean startup/shutdown without crashes
   - Correct timestamps in RTSP streams (no negative DTS/PTS, no "making something up")
   - Stable multi-client RTSP connections with no frame drops
   - MP4 recording integrity
   - WebSocket/HTTP preview streaming functionality

2. **Merge to main** — After validation passes, merge stability-improvements branch
   to main/stable branch for production deployment.

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
