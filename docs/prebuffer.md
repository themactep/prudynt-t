# Pre-Trigger Buffer

The pre-trigger buffer captures video frames before motion detection or manual triggers, enabling recordings that include context from before the event occurred.

## Overview

When enabled, the prebuffer maintains a time-based buffer of recent video frames. Frames older than the configured duration are automatically evicted. When recording starts, these buffered frames are written first, followed by live frames, creating a seamless recording with pre-trigger content.

**Example Timeline:**
- Motion detected at T=0
- Recording includes frames from T-10 to T+10 (20 seconds total with 10s prebuffer)
- Provides up to 10 seconds of context before the trigger

## Configuration

Add prebuffer settings to `/etc/prudynt.json`:

```json
{
  "recorder": {
    "prebuffer_enabled": false,
    "prebuffer_seconds": 10,
    "prebuffer_keyframe_only": false,
    "prebuffer_max_memory_mb": 6
  }
}
```

### Configuration Parameters

- **`prebuffer_enabled`** (bool, default: `false`)
  - Master enable/disable switch
  - When `false`: Zero memory allocation, no processing overhead
  - When `true`: Activates prebuffer for all channels

- **`prebuffer_seconds`** (int, default: `10`)
  - Target duration of prebuffer in seconds
  - Uses time-based eviction: frames older than this duration are automatically removed
  - Actual prebuffer duration may be slightly less if the oldest frames don't start with a keyframe

- **`prebuffer_keyframe_only`** (bool, default: `false`)
  - When `true`: Store only IDR/keyframes (saves ~70% memory)
  - When `false`: Store all frames for smooth playback

- **`prebuffer_max_memory_mb`** (int, default: `6`)
  - Maximum memory per channel in MB
  - Safety limit to prevent OOM on low-memory devices
  - Buffer auto-removes oldest frames if limit exceeded

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

1. **Continuous Capture**: Frames stored in time-based buffer during normal operation
2. **Recording Trigger**: START command flushes prebuffer frames first
3. **Keyframe Alignment**: Recording starts from the first keyframe in the prebuffer (frames before the first keyframe are skipped)
4. **Timestamp Handling**: Prebuffer frames use timestamps starting from 0ms, live frames continue with an offset equal to the prebuffer duration
5. **Frame Queuing**: Frames arriving during prebuffer flush are queued and written after the flush completes (no gaps)
6. **Duration Calculation**: Total recording ≈ prebuffer_duration + requested_duration (prebuffer duration depends on keyframe position)

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
- Prebuffer frames use positive timestamps starting from 0ms
- Live frames continue with an offset (e.g., if prebuffer is 7.5s, first live frame starts at ~7500ms)
- Check recording starts at expected time offset
- Verify timestamp continuity across prebuffer/live boundary

## Example Usage

### Basic Setup
```json
{
  "recorder": {
    "prebuffer_enabled": true,
    "prebuffer_seconds": 10,
    "prebuffer_keyframe_only": false,
    "prebuffer_max_memory_mb": 6
  }
}
```

### Memory-Optimized Setup
```json
{
  "recorder": {
    "prebuffer_enabled": true,
    "prebuffer_seconds": 5,
    "prebuffer_keyframe_only": true,
    "prebuffer_max_memory_mb": 2
  }
}
```

### Recording with Prebuffer
```bash
# Trigger 10-second recording (includes ~10s prebuffer = ~20s total)
echo "START path=/tmp/event.mp4 duration=10" > /run/prudynt/mp4ctl

# Result: Recording from T-10 to T+10 with seamless playback
```

## Technical Implementation

The prebuffer system consists of:

- **PreTriggerBuffer Class**: Time-based buffer with thread-safe operations and automatic eviction
- **VideoWorker Integration**: Frame capture in existing processing pipeline, with frame queuing during flush
- **MP4ControlSocket Integration**: Prebuffer flush on recording start with keyframe alignment
- **Configuration System**: JSON parsing with validation and defaults
- **Memory Management**: Automatic time-based eviction and memory limit enforcement

### Key Implementation Details

1. **Time-Based Eviction**: Frames older than `prebuffer_seconds` are automatically removed when new frames arrive
2. **Keyframe Alignment**: Recording always starts from a keyframe to ensure decodable video
3. **Frame Queuing**: Live frames arriving during prebuffer flush are queued (not dropped) to prevent gaps
4. **Timestamp Offset**: Live frames are offset by the prebuffer duration to ensure continuous timestamps

For implementation details, see `src/PreTriggerBuffer.hpp` and related source files.