# Encoder Bitrate & GOP Auto-Tuning Proposal

## Context
- Sensors expose their physical capabilities via `/proc/jz/sensor/*` (width, height, min/max FPS).
- We already clamp stream width/height/FPS to these limits during `IMPSystem::init()`.
- Remaining mismatches stem from overly aggressive bitrate and GOP settings relative to the actual sensor output, which can cause encoder buffer pressure and `hwicodec` init failures on newer SDKs (e.g., T23 SDK 1.1.2).

## Goals
1. Auto-bound stream bitrates to realistic ceilings derived from the clamped resolution/FPS.
2. Keep GOP size within a sensible range relative to FPS (e.g., at least one IDR per second, no more than 2× the FPS) to avoid long recovery periods and excessive buffer usage.
3. Preserve user intent when their values fall inside the safe envelope; only clamp when they exceed derived limits, and log any adjustments.
4. Share the same enforcement logic across stream0/1/2 for consistency.

## Proposed Approach
1. **Extend the existing clamping helper** (`clamp_stream_to_sensor_limits`) with bitrate/GOP logic:
   - Compute `pixels_per_second = width × height × fps`.
   - Derive `max_bitrate_kbps ≈ pixels_per_second × 0.07 / 1000` (≈0.07 bits per pixel per frame for H.264 HP). Clamp between [256 kbps, 8000 kbps] as global guard rails.
   - If the configured bitrate exceeds this, log and reduce it; otherwise leave untouched.
   - GOP: for each stream, set `min_gop = max(1, fps / 2)` and `max_gop = fps × 2`. Clamp both `gop` and `max_gop` into that window, ensuring `max_gop ≥ gop`. This keeps IDR cadence reasonable and prevents huge GOPs that violate encoder expectations.
2. Optionally expose tuning constants in config (e.g., `general.bitrate_scale`, `general.max_gop_factor`) so integrators can relax/tighten the heuristic without touching code.
3. Emit informative logs (mirroring the existing width/height/fps clamps) whenever bitrate/GOP adjustments occur. This helps OEMs notice when their config exceeds hardware constraints.

## Alternatives Considered
- **Reject startup when values are out of range**: simpler but less user-friendly; prefer automatic clamping + logging.
- **Hardcode chipset-specific tables**: could be more precise but adds maintenance burden. Starting with resolution/FPS-derived heuristics keeps things generic.
- **Expose an API for runtime scaler/FPS requests**: heavier change; out of scope for this proposal.

## Next Steps
1. Gather empirical bitrate/GOP vs. stability data on T23 SDK 1.1.2 to validate the 0.07 bppf constant and GOP window.
2. Implement the helper changes once numbers are agreed upon.
3. Document the behavior in the Prudynt docs so integrators know their settings may be clamped.
