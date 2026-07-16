#!/bin/bash
# -----------------------------------------------------------------------------
# NOTE: prudynt no longer links libschrift (burned-in OSD support was removed
# in commit b718ed5). This script is not called by build.sh anymore and is
# kept only for manual/experimental use.
#
# This script automates the process of setting up a cross-compilation
# environment for the libschrift TrueType rendering library. It prepares the
# build directory, clones the libschrift repository if not present, checks
# out the pinned revision, applies the local patches (res/libschrift/), then
# compiles the library (static with -static, shared otherwise) and installs
# the library and header into 3rdparty/install/.
#
# Env: PRUDYNT_CROSS (cross prefix), LIBC_EXTRA_CFLAGS (optional extra cflags)
# -----------------------------------------------------------------------------

set -e
set -o pipefail

# Variables
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../3rdparty"
LIBSCHRIFT_REPO="https://github.com/tomolt/libschrift/"
LIBSCHRIFT_DIR="${BUILD_DIR}/libschrift"
LIBSCHRIFT_VER="8e533fd07acc2f8ae4cffe7f95d2c3392773e2b5" # v0.10.2, matches buildroot package/libschrift
INSTALL_DIR="${BUILD_DIR}/install"

PRUDYNT_CROSS="${PRUDYNT_CROSS#ccache }"

CC="${PRUDYNT_CROSS}gcc"
AR="${PRUDYNT_CROSS}ar"
RANLIB="${PRUDYNT_CROSS}ranlib"

# Determine if building static or shared library
BUILD_STATIC=0
if [[ "$1" == "-static" ]]; then
    BUILD_STATIC=1
fi

# Create libschrift build directory
echo "Creating libschrift build directory..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Clone libschrift if not already present
if [ ! -d "$LIBSCHRIFT_DIR" ]; then
    echo "Cloning libschrift..."
    git clone "$LIBSCHRIFT_REPO"
else
    echo "libschrift directory exists, using existing version..."
fi

cd "$LIBSCHRIFT_DIR"

# Drop previously applied patches / local modifications so re-runs are clean
git reset --hard HEAD 2>/dev/null || true
git clean -fd 2>/dev/null || true

# Checkout pinned revision (fetch it first if the clone lacks it)
git rev-parse -q --verify "${LIBSCHRIFT_VER}^{commit}" >/dev/null || git fetch origin
git checkout -q "$LIBSCHRIFT_VER"

# Apply local libschrift patches if present
if ls "${SCRIPT_DIR}/../res/libschrift/"*.patch >/dev/null 2>&1; then
    for p in "${SCRIPT_DIR}/../res/libschrift/"*.patch; do
        patch -p1 -N < "$p" || true
    done
fi

mkdir -p "$INSTALL_DIR/lib" "$INSTALL_DIR/include"

echo "Building libschrift library..."
if [[ $BUILD_STATIC -eq 1 ]]; then
    "$CC" ${LIBC_EXTRA_CFLAGS:-} -Os -std=c99 -pedantic -Wall -Wextra -Wconversion -c -o schrift.o schrift.c
    "$AR" rc libschrift.a schrift.o
    "$RANLIB" libschrift.a
    cp libschrift.a "$INSTALL_DIR/lib/"
else
    "$CC" ${LIBC_EXTRA_CFLAGS:-} -Os -std=c99 -pedantic -Wall -Wextra -Wconversion -fPIC -c -o schrift.o schrift.c
    "$CC" -shared -o libschrift.so schrift.o
    cp libschrift.so "$INSTALL_DIR/lib/"
fi

# Install header
cp schrift.h "$INSTALL_DIR/include/"

echo "libschrift build complete!"
