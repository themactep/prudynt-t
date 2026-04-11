# prudynt-t

**prudynt-t** is a video server based on the **[prudynt-v3](https://git.i386.io/wyze/prudynt-v3)** project originally for the Wyzecam v3. It extends the functionality of the original project while expanding compatibility with modern Ingenic hardware.

## Features

- **Video Compression**: Supports both H264 and H265 codecs for efficient video compression and streaming.
- **Two-Way Audio**: Enables bidirectional audio communication using AAC and PCMU codecs for supported devices.
- **Pre-Trigger Buffer**: Captures video frames before recording triggers, providing context for motion events (configurable 1-10 seconds).
- **Expanded Configuration**: Integrated support for **[libimp_control](https://github.com/gtxaspec/libimp_control)**.
- **Thingino Integration**: Seamlessly integrates with **[thingino](https://github.com/themactep/thingino-firmware)**, enhancing connectivity and control options.

## Building

### Option 1: Using Podman (Recommended for isolation)

This is the simplest way to build prudynt-t in an isolated environment with all dependencies pre-configured. Podman provides a rootless, daemonless container engine.

```bash
# Clone the repo
git clone https://github.com/themactep/prudynt-t.git
cd prudynt-t
git checkout stability-improvements

# Update submodules
git submodule update --init

# Build for a specific target and build type
podman build \
  --build-arg TARGET=T31 \
  --build-arg BUILD_TYPE=dynamic \
  -t prudynt-builder .

podman run --rm -v "$(pwd):/src" prudynt-builder
```

**Build Type Options:**
- `dynamic` (default): Dynamically linked binary
- `static`: Statically linked binary  
- `hybrid`: Hybrid linking mode

The resulting binary will be at: `bin/prudynt-T31-dynamic`

**Note:** If you prefer Docker, simply replace `podman` with `docker` in the commands above.

### Option 2: Using Thingino Buildroot (Recommended for production)

For the best binary compatibility and integration with the Thingino firmware:

1. Set up [Thingino buildroot](https://github.com/themactep/thingino-firmware/wiki/Development) environment
2. Clone prudynt-t into the buildroot:
   ```bash
   git clone https://github.com/themactep/prudynt-t.git
   cd prudynt-t
   git checkout stability-improvements
   git submodule update --init
   ```
3. Run the buildroot build script:
   ```bash
   ./buildroot_dev.sh -b dynamic wyze_cp2
   ```

This method uses the Thingino cross-compilation toolchain (GCC 15) and ensures maximum compatibility with your target device.

## Contributing

Contributions to prudynt-t are welcome! If you have improvements, bug fixes, or new features, please feel free to submit a pull request or open an issue.
