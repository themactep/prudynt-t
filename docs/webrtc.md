# Tiny WebRTC integration for prudynt-t streamer

This document describes how to add minimal WebRTC support using [libdatachannel](https://github.com/paullouisageneau/libdatachannel).

## 1. Add libdatachannel as a dependency

- Recommended: add as a git submodule or fetch+build in your build system.
- Example (submodule):

    git submodule add https://github.com/paullouisageneau/libdatachannel.git external/libdatachannel

- Or download and place in `external/libdatachannel`.

## 2. Build integration

- Add to your CMakeLists.txt or Makefile:
    - Include headers from `external/libdatachannel/include`
    - Link with the built static or shared library

## 3. Minimal C++ wrapper

- Create `src/WebRTCStreamer.hpp` and `src/WebRTCStreamer.cpp`.
- Implement a class that:
    - Creates a `rtc::PeerConnection`
    - Handles offer/answer and ICE signaling
    - Accepts encoded video/audio frames and sends them as RTP
    - Exposes hooks for signaling (SDP, ICE)

## 4. Example usage

- On offer from client, create a new `WebRTCStreamer` instance, set remote SDP, and return answer.
- Push encoded frames from your pipeline to the `WebRTCStreamer`.

---

See `src/WebRTCStreamer.hpp` and `src/WebRTCStreamer.cpp` for a minimal implementation scaffold.
