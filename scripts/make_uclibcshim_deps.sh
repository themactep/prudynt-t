#!/bin/bash
# -----------------------------------------------------------------------------
# This script builds the ingenic-uclibc shim library used by uClibc-based
# prudynt builds. It clones the ingenic-uclibc repository if not present,
# compiles the single-source shim directly with the cross toolchain (there is
# no Makefile upstream), always producing the shared library and additionally
# the static archive when -static is passed, then installs the results into
# 3rdparty/install/.
#
# Env: PRUDYNT_CROSS (cross prefix), LIBC_EXTRA_CFLAGS (optional extra cflags)
# -----------------------------------------------------------------------------

set -e
set -o pipefail

# Variables
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../3rdparty"
SHIM_REPO="https://github.com/gtxaspec/ingenic-uclibc"
SHIM_DIR="${BUILD_DIR}/ingenic-uclibc"
SHIM_VER="97c9ba8549febefd9f06fad793ba6afca0676054" # matches buildroot package/ingenic-uclibc
INSTALL_DIR="${BUILD_DIR}/install"

PRUDYNT_CROSS="${PRUDYNT_CROSS#ccache }"

CC="${PRUDYNT_CROSS}gcc"
AR="${PRUDYNT_CROSS}ar"

# Determine if a static archive is needed in addition to the shared library
BUILD_STATIC=0
if [[ "$1" == "-static" ]]; then
    BUILD_STATIC=1
fi

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Clone ingenic-uclibc if not already present
if [ ! -d "$SHIM_DIR" ]; then
    echo "Cloning ingenic-uclibc..."
    git clone --depth=1 "$SHIM_REPO"
fi

cd "$SHIM_DIR"

# Checkout pinned revision (fetch it first if the shallow clone lacks it)
git rev-parse -q --verify "${SHIM_VER}^{commit}" >/dev/null 2>&1 || git fetch --depth=1 origin "$SHIM_VER"
git checkout -q "$SHIM_VER"

echo "Building libuclibcshim library..."
# ingenic-uclibc has no Makefile; single-source shim compiled directly.
"$CC" ${LIBC_EXTRA_CFLAGS:-} -fPIC -shared -o libuclibcshim.so uclibc_shim.c
if [[ $BUILD_STATIC -eq 1 ]]; then
    "$CC" ${LIBC_EXTRA_CFLAGS:-} -fPIC -c uclibc_shim.c -o uclibc_shim.o
    "$AR" rcs libuclibcshim.a uclibc_shim.o
fi

# Install libuclibcshim libraries
mkdir -p "$INSTALL_DIR/lib"
cp libuclibcshim.* "$INSTALL_DIR/lib/"

echo "libuclibcshim build complete!"
