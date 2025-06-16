# Prudynt-T Buildroot Package Updates

This directory contains the updated buildroot package files for prudynt-t with JSON configuration support.

## Changes Made

### Dependencies
- **Removed**: `libconfig` dependency
- **Added**: `json-for-modern-cpp` dependency
- **Added**: GCC >= 4.9 and C++ standard library requirements

### Configuration Format
- **Old**: libconfig format (`.cfg` files)
- **New**: Pure JSON format (`.json` files)

### Files Updated

#### Config.in
- Replaced `BR2_PACKAGE_LIBCONFIG` with `BR2_PACKAGE_JSON_FOR_MODERN_CPP`
- Added dependency requirements for C++11 support
- Updated help text to mention JSON configuration

#### prudynt-t.mk
- Updated `PRUDYNT_T_DEPENDENCIES` to use `json-for-modern-cpp`
- Modified install commands to handle `.json` files instead of `.cfg`
- Simplified configuration processing (no more complex awk scripts)
- Updated low-memory device buffer adjustment for JSON format

#### Migration Tools
- Added `cfg-to-json.sh` script for converting existing configurations

## Installation

1. Copy these files to your thingino buildroot package directory:
   ```bash
   cp -r buildroot-package/* /path/to/thingino/package/prudynt-t/
   ```

2. Rebuild the package:
   ```bash
   make prudynt-t-rebuild
   ```

## Configuration Migration

For existing installations with `.cfg` files:

1. Use the migration script:
   ```bash
   ./files/cfg-to-json.sh /etc/prudynt.cfg /etc/prudynt.json
   ```

2. Validate the JSON:
   ```bash
   jq . /etc/prudynt.json
   ```

3. Review and adjust manually if needed

## Benefits

- **Standard Format**: JSON is widely supported and standardized
- **Better Tooling**: JSON validation, formatting, and editing tools
- **Cleaner Config**: No comments in config files for pure data format
- **Smaller Dependencies**: Header-only library vs compiled libconfig++
- **Modern C++**: Uses modern C++ JSON library with intuitive syntax

## Compatibility

- Requires GCC 4.9+ for C++11 support
- Requires C++ standard library
- All existing prudynt-t functionality remains unchanged
- Only configuration file format changes
