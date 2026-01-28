# Pre-Trigger Buffer - Conditional Compilation

## Overview

The pre-trigger buffer feature can be conditionally compiled in/out using the `USE_PREBUFFER` build flag. This allows memory-constrained devices to exclude the feature entirely, saving both binary size and runtime memory overhead.

## Build Configuration

### Enable Pre-Trigger Buffer (Default)

```bash
make
# or explicitly
USE_PREBUFFER=1 make
```

### Disable Pre-Trigger Buffer

```bash
USE_PREBUFFER=0 make
```

### Cross-Compilation Examples

```bash
# T31 with prebuffer enabled (default)
./build.sh t31

# T31 with prebuffer disabled
USE_PREBUFFER=0 ./build.sh t31

# T23 with prebuffer disabled
USE_PREBUFFER=0 ./build.sh t23
```

## Memory Impact

### With Prebuffer Enabled (`USE_PREBUFFER=1`)
- **Binary size**: +3-4KB code
- **Runtime memory** (per channel):
  - Full frames (3s): ~1.5-2MB
  - Keyframe-only (3s): ~0.5MB
- **Configuration**: All prebuffer options available in `/etc/prudynt.json`

### With Prebuffer Disabled (`USE_PREBUFFER=0`)
- **Binary size**: Baseline (prebuffer code stripped)
- **Runtime memory**: Zero overhead
- **Configuration**: Prebuffer options not available in config

## Recommended Usage

### 64MB+ Devices (Leave Enabled)
- **T31**: Use default (`USE_PREBUFFER=1`)
- **T40/T41**: Use default (`USE_PREBUFFER=1`)
- Prebuffer provides valuable context for motion events
- Sufficient memory available

### 32MB Devices (Disable Recommended)
- **T20/T21**: Set `USE_PREBUFFER=0`
- **T10**: Set `USE_PREBUFFER=0`
- Limited memory requires prioritizing core functionality
- Can still use external triggers without prebuffer

## Buildroot Integration

To disable prebuffer in a buildroot defconfig:

```makefile
# package/prudynt-t/prudynt-t.mk
PRUDYNT_T_MAKE_OPTS += USE_PREBUFFER=0
```

Or create a config option:

```kconfig
config BR2_PACKAGE_PRUDYNT_T_PREBUFFER
    bool "Enable pre-trigger buffer support"
    default y if BR2_PACKAGE_INGENIC_OSDRV_T31
    default n if BR2_PACKAGE_INGENIC_OSDRV_T20
    help
      Enable pre-trigger buffer for motion detection.
      Requires ~2MB additional RAM per channel.
```

## Technical Details

All prebuffer-related code is wrapped with `#ifdef PREBUFFER_ENABLED`:

- `src/PreTriggerBuffer.hpp` - Class definition
- `src/PreTriggerBuffer.cpp` - Implementation
- `src/Config.hpp` - Config struct fields
- `src/Config.cpp` - Config loading/parsing
- `src/VideoWorker.cpp` - Frame capture and buffering
- `src/MP4ControlSocket.cpp` - Recording integration
- `src/globals.hpp` - Global state

When disabled:
- Compiler completely removes prebuffer code
- No runtime checks or overhead
- Configuration options don't exist
- Zero memory allocation

## Verification

Check if prebuffer is compiled in:

```bash
# Check for prebuffer symbols
nm bin/prudynt | grep -i prebuffer

# Check binary size difference
ls -lh bin/prudynt
```

## Migration Note

Existing configurations with prebuffer settings will not cause errors when running a non-prebuffer build. Unknown config keys are silently ignored by the config parser.

