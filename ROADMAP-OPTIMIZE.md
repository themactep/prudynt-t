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

- [x] **#1 PreTriggerBuffer: move-based drain API** [`8237b3d`]
  - Replace `getFrames()` (returns full copy under lock + sort) with `drainFrames()` (moves frames to output)
  - Update consumer in `MP4ControlSocket.cpp`
  - Test: `tests/test_triggerbuffer.cpp` (8 tests, all pass)

- [x] **#3 jpeg_stream::snapshot_buf: fixed cap + swap** [`93e8b2b`]
  - Added `MAX_SNAPSHOT_BYTES` (512KB) constant; oversized JPEGs are skipped, retaining previous valid snapshot
  - Test: `tests/test_jpegsnapshot.cpp` (7 tests, all pass)

### Phase 2 — Allocator churn

- [x] **#4 MsgChannel: deque → fixed ring buffer** [`f8d65d3`]
  - Pre-allocated vector + head/tail; zero allocations on write/read hot path
  - Test: `tests/test_msgchannel.cpp` (11 tests, all pass)

- [x] **#2 OSD: pool glyph bitmap allocation** [`14b3be0`]
  - Pre-allocate one `imageBuffer.pixels` at max glyph dimensions, reuse across all glyphs in a string
  - Eliminates malloc/free per character
  - Test: `tests/test_osdglyphpool.cpp` (7 tests, all pass)

### Phase 3 — Advanced allocators

- [x] **#5 VideoWorker: migrate to SegmentedBuffer** [`ab2be1b`]
  - Replace `std::vector<uint8_t>` with `SegmentedBuffer` for `mp4_sample`
    and `prebuffer_sample` — eliminates reallocation during frame accumulation
  - `PendingFrame::data` also migrated
  - Cross-compile: T23, 0 errors. Desktop tests: all 9 pass.
  - Test: `tests/test_segmented_buffer.cpp` (10 tests, all pass)

### Phase 4 — Startup & plugin prep

- [x] **#7 timesync_wait: remove busy loop** [`fe234f2`]
  - Reduced 60s poll → 5s poll with warning, proceed anyway
  - Test: `tests/test_timesync.cpp` (7 tests, all pass)

- [x] **Plugin API design** [`49dc49e`]
  - Define `plugin_api.h` with lifecycle hooks + data tap callbacks
  - Create `PluginManager` class (dlopen/dlsym/dlclose)
  - Add example plugin showing the lifecycle pattern
  - Test: `tests/test_plugin_api.c` (8 tests, all pass)

- [x] **Integrate PluginManager into main.cpp** [`c6c379b`]
  - Scans plugin directories on startup, startAll/stopAll around main loop
  - PRUDYNT_PLUGIN_DIR env var override
  - Websocket plugin wrapper (builtin-compatible)
  - Test: `tests/test_pluginmanager.cpp` (9 tests, all pass)

- [ ] **Extract WebSocket as standalone .so plugin**
  - Move `WS.cpp`/`WS.hpp` to `plugins/websocket/`
  - Build as separate .so with -fPIC
  - Replace `#ifdef WEBSOCKET_ENABLED` in main.cpp with plugin load
  - Remove the compile-time toggle dependency

## Tests

All tests are self-contained single-file programs that can be compiled standalone:
```
g++ -std=c++17 tests/test_*.cpp -o /tmp/test && /tmp/test
```

Each commit must include a test for the changed component.
