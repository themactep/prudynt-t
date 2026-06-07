# Memory Optimization & Plugin Roadmap

Branch: `optimize-mem-plugin`
Based on: `stable`

## Strategy

| Phase | Focus | Est. binary saving | Est. runtime saving |
|-------|-------|-------------------|-------------------|
| 1 | Eliminate double-copy patterns in hot paths | — | up to 8MB per trigger |
| 2 | Cap unbounded structures, reduce allocator churn | — | up to 5MB sustained |
| 3 | Pool / segmented allocators for frame data | — | up to 20MB peak |
| 4 | Plugin framework design + first plugin extraction | ~30-40% flash | variable |

## Checklist

### Phase 1 — Double-copy elimination

- [x] **#1 PreTriggerBuffer: move-based drain API**
  - Replace `getFrames()` (returns full copy under lock + sort) with `drainFrames()` (moves frames to output)
  - Update consumer in `MP4ControlSocket.cpp`
  - Write test: `tests/test_triggerbuffer.cpp`
  - Commit: `prebuffer: replace getFrames copy with move-based drainFrames`

- [ ] **#3 jpeg_stream::snapshot_buf: fixed cap + swap**
  - Replace unbounded `std::vector<unsigned char>` with fixed-sized ring of 1 + `std::atomic<unsigned char*>` or capped vector with trim
  - Write test: `tests/test_jpegsnapshot.cpp`
  - Commit: `jpeg: cap snapshot_buf to prevent unbounded growth`

### Phase 2 — Allocator churn

- [ ] **#4 MsgChannel: deque → fixed ring buffer**
  - Replace `std::deque<T>` internals with a pre-allocated circular buffer (`T*` array + head/tail indices)
  - Eliminates allocation on every `write()`/`read()`
  - Write test: `tests/test_msgchannel.cpp`
  - Commit: `msgchannel: replace deque with fixed ring buffer`

- [ ] **#2 OSD: pool glyph bitmap allocation**
  - Pre-allocate one `imageBuffer.pixels` at max glyph dimensions, reuse across all glyphs in a string
  - Eliminates malloc/free per character
  - Commit: `osd: pool glyph render buffer across characters`

### Phase 3 — Advanced allocators

- [ ] **#6 AudioFrame data pool**
  - Pre-allocate fixed-size blocks for audio frame payloads, recycle via free-list
  - Commit: `audio: pool-allocate audio frame data`

- [ ] **#5 VideoWorker: segmented mp4_sample buffer**
  - Replace `std::vector<uint8_t>` append for `mp4_sample` with fixed-block chain (e.g., 16KB blocks)
  - Commit: `video: use segmented buffer for mp4 sample accumulation`

### Phase 4 — Startup & plugin prep

- [ ] **#7 timesync_wait: remove busy loop**
  - Replace 60s polling loop with single check + deferred time-dependent ops
  - Commit: `startup: replace timesync busy-wait with single check`

- [ ] **Plugin API design**
  - Define `plugin_api.h` with lifecycle hooks + data tap callbacks
  - Create `PluginManager` class (dlopen/dlsym/dlclose)
  - Add plugin directory scan on startup
  - Commit: `plugin: add plugin API header and PluginManager`

- [ ] **Extract WebSocket as first plugin**
  - Move `WS.cpp`/`WS.hpp` to `plugins/websocket/`
  - Implement `prudynt_plugin_t` interface
  - Build as separate .so
  - Commit: `plugin: extract websocket as loadable plugin`

## Tests

All tests are self-contained single-file programs that can be compiled standalone:
```
g++ -std=c++17 tests/test_*.cpp -o /tmp/test && /tmp/test
```

Each commit must include a test for the changed component.
