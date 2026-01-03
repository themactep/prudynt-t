# Pre-Trigger Buffer

The pre-trigger buffer captures video frames before motion detection or manual triggers, enabling recordings that include context from before the event occurred.

## Overview

When enabled, the prebuffer maintains a circular ring buffer of recent video frames. When recording starts, these buffered frames are written first, followed by live frames, creating a seamless recording with pre-trigger content.

**Example Timeline:**
- Motion detected at T=0
- Recording includes frames from T-3 to T+7 (10 second total)
- Provides 3 seconds of context before the trigger

## Configuration

Add prebuffer settings to `/etc/prudynt.json`:

```json
{
  "recorder": {
    "prebuffer_enabled": false,
    "prebuffer_seconds": 3,
    "prebuffer_keyframe_only": false,
    "prebuffer_max_memory_mb": 2
  }
}
```

### Configuration Parameters

- **`prebuffer_enabled`** (bool, default: `false`)
  - Master enable/disable switch
  - When `false`: Zero memory allocation, no processing overhead
  - When `true`: Activates prebuffer for all channels

- **`prebuffer_seconds`** (int, default: `3`, range: 1-10)
  - Duration of prebuffer in seconds
  - Automatically calculates frame capacity based on stream FPS
  - Example: 3s @ 25fps = 75 frames

- **`prebuffer_keyframe_only`** (bool, default: `false`)
  - When `true`: Store only IDR/keyframes (saves ~70% memory)
  - When `false`: Store all frames for smooth playback

- **`prebuffer_max_memory_mb`** (int, default: `2`, range: 1-8)
  - Maximum memory per channel in MB
  - Safety limit to prevent OOM on low-memory devices
  - Buffer auto-reduces capacity if limit exceeded

## Memory Requirements

**Typical Usage:**
- Full frames: ~1.5MB per channel (3 seconds @ 25fps)
- Keyframe-only: ~0.5MB per channel (70% reduction)
- Multiple channels: Memory limit applies per channel

**Frame Size Estimates:**
- Keyframe (IDR): ~50KB (1920x1080)
- P-frame: ~15KB average
- Buffer capacity: Dynamically calculated based on FPS

## Integration with Recording

The prebuffer integrates seamlessly with the MP4 control system:

1. **Continuous Capture**: Frames stored in circular buffer during normal operation
2. **Recording Trigger**: START command flushes prebuffer frames first
3. **Timestamp Adjustment**: Prebuffer frames get negative timestamps (-3000ms to -33ms)
4. **Live Transition**: Seamless switch to live frames at timestamp 0ms
5. **Duration Calculation**: Total recording = prebuffer_seconds + requested_duration

## Performance Impact

**When Disabled (Default):**
- Zero memory allocation
- No frame copying overhead
- Fast-path checks skip all prebuffer logic
- No impact on existing functionality

**When Enabled:**
- Minimal CPU overhead for frame copying
- Memory usage within configured limits
- Circular buffer prevents memory growth
- Automatic cleanup on recording stop

## Troubleshooting

### High Memory Usage
- Reduce `prebuffer_seconds` (1-5 seconds recommended)
- Enable `prebuffer_keyframe_only` for 70% memory savings
- Lower `prebuffer_max_memory_mb` limit
- Monitor with `cat /proc/meminfo`

### Missing Pre-Trigger Content
- Verify `prebuffer_enabled: true` in config
- Check that recording duration > prebuffer_seconds
- Ensure sufficient memory available
- Review logs for buffer overflow warnings

### Timestamp Issues
- Prebuffer frames use negative timestamps
- MP4 players should handle negative timestamps correctly
- Check recording starts at expected time offset
- Verify timestamp continuity across prebuffer/live boundary

## Example Usage

### Basic Setup
```json
{
  "recorder": {
    "prebuffer_enabled": true,
    "prebuffer_seconds": 3,
    "prebuffer_keyframe_only": false,
    "prebuffer_max_memory_mb": 2
  }
}
```

### Memory-Optimized Setup
```json
{
  "recorder": {
    "prebuffer_enabled": true,
    "prebuffer_seconds": 2,
    "prebuffer_keyframe_only": true,
    "prebuffer_max_memory_mb": 1
  }
}
```

### Recording with Prebuffer
```bash
# Trigger 10-second recording (includes 3s prebuffer = 13s total)
echo "START path=/tmp/event.mp4 duration=10" > /run/prudynt/mp4ctl

# Result: Recording from T-3 to T+10 with seamless playback
```

## Technical Implementation

The prebuffer system consists of:

- **PreTriggerBuffer Class**: Circular buffer with thread-safe operations
- **VideoWorker Integration**: Frame capture in existing processing pipeline
- **MP4ControlSocket Integration**: Prebuffer flush on recording start
- **Configuration System**: JSON parsing with validation and defaults
- **Memory Management**: Automatic size adjustment and cleanup

For implementation details, see `src/PreTriggerBuffer.hpp` and related source files.