# JsonAPI Audio Parameter Changes

## Overview

The JsonAPI audio handling has been updated to apply real-time audio parameter changes immediately to the running hardware, matching the behavior of the FIFO control channel (WS.cpp). This allows for seamless audio adjustments without requiring an audio subsystem restart.

## Changes Made

### 1. Parameter Renaming

All audio parameters have been renamed to match the config schema:

| Old Name | New Name | Description |
|----------|----------|-------------|
| `input_enabled` | `mic_enabled` | Enable/disable microphone |
| `input_format` | `mic_format` | Audio codec format |
| `input_vol` | `mic_vol` | Microphone volume |
| `input_gain` | `mic_gain` | Microphone analog gain |
| `input_bitrate` | `mic_bitrate` | Encoding bitrate |
| `input_sample_rate` | `mic_sample_rate` | Microphone sample rate |
| `input_alc_gain` | `mic_alc_gain` | Automatic Level Control gain |
| `input_noise_suppression` | `mic_noise_suppression` | Noise suppression level |
| `input_high_pass_filter` | `mic_high_pass_filter` | High-pass filter enable |
| `input_agc_enabled` | `mic_agc_enabled` | Automatic Gain Control enable |
| `input_agc_target_level_dbfs` | `mic_agc_target_level_dbfs` | AGC target level |
| `input_agc_compression_gain_db` | `mic_agc_compression_gain_db` | AGC compression gain |
| `output_enabled` | `spk_enabled` | Enable/disable speaker output |
| `output_sample_rate` | `spk_sample_rate` | Speaker sample rate |
| `output_vol` | `spk_vol` | Speaker volume |
| `output_gain` | `spk_gain` | Speaker analog gain |

### 2. Real-Time Parameter Application

The following audio parameters now apply changes immediately to the hardware without requiring an audio restart:

#### Microphone (Input) Parameters

- **`mic_vol`** - Microphone volume
  - Range: -30 to 120 (-30 = mute, 120 = +30dB, step 0.5dB)
  - Applied via: `IMP_AI_SetVol()` for each audio channel
  - No restart required

- **`mic_gain`** - Microphone analog gain
  - Range: -1 to 31 (varies by platform)
  - Applied via: `IMP_AI_SetGain()` for each audio channel
  - No restart required

- **`mic_alc_gain`** - Automatic Level Control gain
  - Range: -1 to 7
  - Applied via: `IMP_AI_SetAlcGain()`
  - Platform-specific: T21, T31, C100 only
  - No restart required

- **`mic_high_pass_filter`** - High-pass filter enable/disable
  - Type: Boolean
  - Applied via: `IMP_AI_EnableHpf()` / `IMP_AI_DisableHpf()`
  - No restart required

#### Speaker (Output) Parameters

- **`spk_vol`** - Speaker volume
  - Range: -30 to 120 (-30 = mute, 120 = +30dB, step 0.5dB)
  - Applied via: `AudioOutputWorker::applyVolumeGain()`
  - No restart required

- **`spk_gain`** - Speaker analog gain
  - Range: 0 to 31
  - Applied via: `AudioOutputWorker::applyVolumeGain()`
  - No restart required

### 3. Parameters Requiring Restart

The following parameters still require an audio subsystem restart as they affect codec, DSP, or fundamental audio pipeline settings:

- `mic_enabled` - Enable/disable microphone (restart audio workers only)
- `mic_format` - Audio codec (AAC, OPUS, PCM, G711A, G711U, G726)
- `mic_bitrate` - Encoding bitrate (6-256 kbps)
- `mic_sample_rate` - Sample rate (8000, 16000, 44100, 48000 Hz)
- `mic_noise_suppression` - Noise suppression level (0-3)
- `mic_agc_enabled` - AGC enable/disable
- `mic_agc_target_level_dbfs` - AGC target level (0-31)
- `mic_agc_compression_gain_db` - AGC compression gain (0-90)
- `force_stereo` - Force stereo mode
- `spk_enabled` - Enable/disable speaker (restart audio workers only)
- `spk_sample_rate` - Speaker sample rate

## Implementation Details

### Added Dependencies

```cpp
#include "AudioOutputWorker.hpp"
```

### Code Structure

Real-time parameters use custom handling instead of the generic `add_int()` / `add_boolk_a()` functions:

```cpp
// Example: mic_vol
if (JsonValue *v = obj_get(obj, "mic_vol")) {
  if (v->type == JSON_NUMBER) {
    int vol = (int)v->value.number;
    if (cfg->set<int>("audio.mic_vol", vol)) {
      // Apply to hardware immediately
      for (int i = 0; i < NUM_AUDIO_CHANNELS; i++) {
        if (global_audio[i]) {
          IMP_AI_SetVol(i, global_audio[i]->aiChn, vol);
        }
      }
    }
  }
  add_key(out, s2, "mic_vol");
  add_num(out, cfg->get<int>("audio.mic_vol"));
  wrote = true;
}
```

## Benefits

1. **Seamless Adjustments**: Volume and gain can be adjusted without audio dropouts
2. **Consistent Behavior**: JsonAPI now matches the FIFO control channel behavior
3. **Better UX**: Real-time feedback when adjusting audio levels
4. **API Parity**: Same functionality available via REST API and WebSocket control

## Usage Example

### Before (Required Restart)
```bash
curl -X POST http://camera/api/v1/config \
  -H "Content-Type: application/json" \
  -d '{"audio": {"input_vol": 90}}'
# Audio would restart, causing brief interruption
```

### After (Immediate Application)
```bash
curl -X POST http://camera/api/v1/config \
  -H "Content-Type: application/json" \
  -d '{"audio": {"mic_vol": 90}}'
# Volume changes immediately, no interruption
```

## Testing

To verify the changes work correctly:

1. **Test volume adjustment**:
   ```bash
   curl -X POST http://camera/api/v1/config -d '{"audio": {"mic_vol": 60}}'
   curl -X POST http://camera/api/v1/config -d '{"audio": {"mic_vol": 80}}'
   ```
   Volume should change immediately without audio restart.

2. **Test gain adjustment**:
   ```bash
   curl -X POST http://camera/api/v1/config -d '{"audio": {"mic_gain": 15}}'
   curl -X POST http://camera/api/v1/config -d '{"audio": {"mic_gain": 25}}'
   ```
   Gain should change immediately without audio restart.

3. **Test speaker parameters**:
   ```bash
   curl -X POST http://camera/api/v1/config -d '{"audio": {"spk_vol": 70}}'
   curl -X POST http://camera/api/v1/config -d '{"audio": {"spk_gain": 20}}'
   ```
   Speaker settings should apply immediately.

## Platform Compatibility

| Parameter | All Platforms | Platform-Specific |
|-----------|---------------|-------------------|
| `mic_vol` | ✓ | |
| `mic_gain` | ✓ | |
| `mic_alc_gain` | | T21, T31, C100 only |
| `mic_high_pass_filter` | ✓ | |
| `spk_vol` | ✓ | |
| `spk_gain` | ✓ | |

## Migration Notes

If you have existing scripts or applications using the old parameter names (`input_*`, `output_*`), update them to use the new names (`mic_*`, `spk_*`):

```diff
{
  "audio": {
-   "input_vol": 80,
-   "input_gain": 25,
-   "output_vol": 70
+   "mic_vol": 80,
+   "mic_gain": 25,
+   "spk_vol": 70
  }
}
```
