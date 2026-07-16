#!/bin/bash
# -----------------------------------------------------------------------------
# This script builds the Helix-MP3 decoder as a static library from the
# sources shipped inside the libhelix-aac (ESP8266Audio) checkout. It expects
# the libhelix-aac sources to be present in 3rdparty/ (run
# make_libhelixaac_deps.sh first), compiles each translation unit with the
# cross toolchain, archives them into libhelix-mp3.a, and installs the
# library and headers into 3rdparty/install/.
#
# Env: PRUDYNT_CROSS (cross prefix), LIBC_EXTRA_CFLAGS (optional extra cflags)
# -----------------------------------------------------------------------------

set -e
set -o pipefail

# Variables
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../3rdparty"
LIBHELIX_DIR="${BUILD_DIR}/libhelix-aac"
MP3_SRC="${LIBHELIX_DIR}/src/libhelix-mp3"
INSTALL_DIR="${BUILD_DIR}/install"
ARDUINO_COMPAT="${SCRIPT_DIR}/../res/arduino_compat"
OBJ_DIR="/tmp/libhelix-mp3-build"

PRUDYNT_CROSS="${PRUDYNT_CROSS#ccache }"

CC="${PRUDYNT_CROSS}gcc"
AR="${PRUDYNT_CROSS}ar"

if [ ! -d "$MP3_SRC" ]; then
    echo "libhelix-mp3 sources not found at $MP3_SRC (run make_libhelixaac_deps.sh first)"
    exit 1
fi

mkdir -p "$INSTALL_DIR/lib" "$INSTALL_DIR/include"

# Compile all translation units
echo "Building libhelix-mp3 library..."
mkdir -p "$OBJ_DIR"
for src in "$MP3_SRC"/*.c; do
    obj="$OBJ_DIR/$(basename "${src%.c}").o"
    "$CC" ${LIBC_EXTRA_CFLAGS:-} -Os -I"$MP3_SRC" -I"$ARDUINO_COMPAT" \
        -DUSE_DEFAULT_STDLIB -DARDUINO -c "$src" -o "$obj"
done

# Install libhelix-mp3 library and headers
echo "Installing libhelix-mp3 library and headers..."
"$AR" rcs "$INSTALL_DIR/lib/libhelix-mp3.a" "$OBJ_DIR"/*.o
find "$MP3_SRC" -maxdepth 1 -name '*.h' -exec cp {} "$INSTALL_DIR/include/" \;
rm -rf "$OBJ_DIR"

echo "libhelix-mp3 build complete!"
