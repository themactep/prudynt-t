# Audio/Video Synchronization Fixes

## Overview

This document describes the fixes implemented to resolve audio/video synchronization issues in the streaming system, particularly focusing on Opus frame accumulation timing and improved debugging capabilities.

## Issues Identified and Fixed

### 1. Critical Opus Frame Accumulation Bug

**Problem**: The AudioWorker was correctly calculating timestamps for accumulated Opus frames but then discarding them in `process_audio_frame_direct()`, which always used fresh timestamps from TimestampManager.

**Root Cause**: 
- Opus frames are accumulated from 320 samples to 960 samples (20ms frames)
- `bufferStartTimestamp` was carefully calculated for accumulated frames
- `process_audio_frame_direct()` ignored `frame.timeStamp` and always called `TimestampManager::getInstance().getTimestamp()`
- This caused accumulated frames to have incorrect timestamps, breaking A/V sync

**Fix**: Modified `process_audio_frame_direct()` to:
- Check if `frame.timeStamp > 0` (indicating an accumulated frame with pre-calculated timestamp)
- Use the pre-calculated timestamp for accumulated frames
- Only use fresh TimestampManager timestamp for direct frames

### 2. Audio Logging Rate Limiting

**Problem**: Critical audio debug logs were rate-limited to only first 3 occurrences, preventing visibility into ongoing sync issues.

**Fix**: 
- Removed rate limiting from Opus frame accumulation logs
- Added clear "AUDIO_SYNC_" prefixes for easy filtering
- Always log frame accumulation start and frame ready events

### 3. TimestampManager Logging Improvements

**Problem**: TimestampManager debug logs were heavily rate-limited (first 10, then every 100th), making it difficult to track timestamp issues.

**Fix**:
- Improved rate limiting to log first 20 calls, then every 1 second
- Added detection of significant timestamp jumps (>100ms)
- Enhanced logging with timestamp differences for better debugging

### 4. Video Timestamp Logging Enhancement

**Fix**: Added consistent timestamp logging format in VideoWorker to match AudioWorker for easier correlation.

## Code Changes Summary

### AudioWorker.cpp
```cpp
// OLD: Always used fresh timestamp
TimestampManager::getInstance().getTimestamp(&encoder_time);

// NEW: Use pre-calculated timestamp for accumulated frames
if (frame.timeStamp > 0) {
    // Accumulated frame - use pre-calculated timestamp
    encoder_time.tv_sec = frame.timeStamp / 1000000;
    encoder_time.tv_usec = frame.timeStamp % 1000000;
} else {
    // Direct frame - use fresh timestamp
    TimestampManager::getInstance().getTimestamp(&encoder_time);
}
```

### TimestampManager.cpp
- Added time-based rate limiting (every 1 second)
- Added timestamp jump detection (>100ms changes)
- Enhanced debug output with timestamp differences

### VideoWorker.cpp
- Improved timestamp logging format for consistency
- Added microsecond timestamp calculation for correlation

## Verification Process

### 1. Build and Deploy
```bash
# Build the updated binary
./build.sh full T23 -static

# Deploy to camera
ssh root@192.168.88.171 /mnt/nfs/prudynt
```

### 2. Test RTSP Stream
```bash
# Check stream info
ffprobe -v quiet -print_format json -show_streams rtsp://thingino:thingino@192.168.88.171:554/ch0

# Test streaming
ffmpeg -i rtsp://thingino:thingino@192.168.88.171:554/ch0 -t 10 -f null -
```

### 3. Monitor Logs
Look for these log patterns:
- `AUDIO_SYNC_ACCUMULATION_START`: Opus frame accumulation begins
- `AUDIO_SYNC_FRAME_READY`: Accumulated frame ready for processing
- `AUDIO_TIMESTAMP_ACCUMULATED`: Using pre-calculated timestamp
- `AUDIO_TIMESTAMP_DIRECT`: Using fresh timestamp
- `VIDEO_TIMESTAMP_PROCESS`: Video frame processing
- `TIMESTAMP_MANAGER_SOURCE`: TimestampManager output

### 4. Verify Synchronization
- Both audio and video streams should be present in RTSP output
- No "Invalid PTS" or "Reset playback" messages
- Smooth playback without audio/video drift

## Configuration for Debugging

To enable full debug logging, ensure the configuration includes:
```json
{
  "general": {
    "loglevel": "DEBUG",
    "audio_debug_verbose": true,
    "timestamp_validation_enabled": true
  }
}
```

## Expected Behavior

### Normal Operation
- Audio frames accumulate properly for Opus encoding
- Timestamps maintain consistency between accumulated frames
- Video and audio streams stay synchronized
- No timestamp jumps or resets

### Debug Output
- Clear logging of frame accumulation process
- Timestamp correlation between audio and video
- Detection of any timing anomalies

## Troubleshooting

### If sync issues persist:
1. Check log level is set to DEBUG
2. Monitor for timestamp jump warnings
3. Verify shared RTP timestamp base is being used
4. Check for buffer overflow/underflow messages

### Common Issues:
- **Large timestamp jumps**: Check TimestampManager initialization
- **Audio dropouts**: Monitor buffer capacity warnings
- **Video stuttering**: Check encoder performance logs

## Testing Results

The fixes have been tested with:
- ✅ RTSP stream establishment
- ✅ Audio/video stream detection
- ✅ Opus frame accumulation
- ✅ Timestamp consistency
- ✅ Debug logging functionality

## Future Improvements

1. Add real-time A/V sync measurement
2. Implement adaptive buffer management
3. Add metrics for sync drift detection
4. Consider hardware timestamp validation
