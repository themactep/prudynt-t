## Snapshot Serving Options (Design Notes)

This document summarizes alternative approaches for serving on-demand JPEG snapshots without wasting CPU/IO when no one is reading them.

### A) In-memory buffer + HTTP/WS (implemented)
- Store the latest JPEG in RAM (vector<uint8_t>) with LWS_PRE headroom.
- On request, if JPEG channel is idle, wake, capture one frame, write into buffer, respond from memory.
- File writes disabled by default.
- Pros: no unnecessary file writes; simple; concurrent clients supported.
- Cons: external tools must use HTTP/WS.

### B) WebSocket-only snapshot
- Serve only via WS binary; drop HTTP endpoint.
- Pros: single transport; already implemented preview path.
- Cons: tools that expect HTTP GET must adapt.

### C) On-demand file write (compatibility bridge)
- Do not write continuously. Save /tmp/snapshot.jpg only when asked.
- HTTP: /snapshot.jpg?save=1 writes one file and returns it
- WS: stream2.save_snapshot (null) writes once
- Pros: gradual migration for file-based tools
- Cons: still touches disk per save; readers must coordinate

### D) Unix domain socket (UDS) one-shot
- Expose /run/prudynt/snapshot.sock; on connect, send one fresh JPEG and close.
- Pros: local & efficient; back-pressure; no disk IO
- Cons: tools must be adapted; not as widely compatible as HTTP

### E) FIFO (named pipe)
- Replace file path with a FIFO. When a reader opens, capture and write one JPEG.
- Pros: superficially similar to a file
- Cons: FIFO quirks; not compatible with libs that expect regular file semantics (seek, stat)

### Notes
- A and C can coexist: default (A) with optional `?save=1` bridge.
- Rate-limit or throttle can be applied as today through the preview logic.
- For multi-stream support, provide per-channel endpoints (e.g., /ch0.jpg, /ch1.jpg) as implemented.

