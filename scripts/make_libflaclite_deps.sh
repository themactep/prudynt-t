#!/bin/bash
# -----------------------------------------------------------------------------
# This script builds the FLAC-lite decoder as a static library from the
# sources shipped inside the libhelix-aac (ESP8266Audio) checkout. It expects
# the libhelix-aac sources to be present in 3rdparty/ (run
# make_libhelixaac_deps.sh first), compiles each translation unit with the
# cross toolchain, archives them into libflac-lite.a, and installs the
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
FLAC_SRC="${LIBHELIX_DIR}/src/libflac"
INSTALL_DIR="${BUILD_DIR}/install"
ARDUINO_COMPAT="${SCRIPT_DIR}/../res/arduino_compat"
OBJ_DIR="/tmp/libflac-build"

PRUDYNT_CROSS="${PRUDYNT_CROSS#ccache }"

CC="${PRUDYNT_CROSS}gcc"
AR="${PRUDYNT_CROSS}ar"

if [ ! -d "$FLAC_SRC" ]; then
    echo "libflac sources not found at $FLAC_SRC (run make_libhelixaac_deps.sh first)"
    exit 1
fi

mkdir -p "$INSTALL_DIR/lib" "$INSTALL_DIR/include"

# Compile all translation units
echo "Building libflac-lite library..."
mkdir -p "$OBJ_DIR"
for src in "$FLAC_SRC"/*.c; do
    obj="$OBJ_DIR/$(basename "${src%.c}").o"
    "$CC" ${LIBC_EXTRA_CFLAGS:-} -Os -I"$FLAC_SRC" -I"$ARDUINO_COMPAT" \
        -DUSE_DEFAULT_STDLIB -c "$src" -o "$obj"
done

# Install libflac-lite library and headers
echo "Installing libflac-lite library and headers..."
"$AR" rcs "$INSTALL_DIR/lib/libflac-lite.a" "$OBJ_DIR"/*.o
cp -r "$FLAC_SRC/FLAC" "$INSTALL_DIR/include/"
find "$FLAC_SRC" -maxdepth 1 -name '*.h' -exec cp {} "$INSTALL_DIR/include/" \;
rm -rf "$OBJ_DIR"

echo "libflac-lite build complete!"
