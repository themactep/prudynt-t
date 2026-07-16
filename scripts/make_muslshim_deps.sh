#!/bin/bash
# -----------------------------------------------------------------------------
# This script builds the ingenic-musl shim library used by musl-based
# prudynt builds. It clones the ingenic-musl repository if not present,
# builds the shim with its upstream Makefile using the cross toolchain
# (shared by default, static + shared when -static is passed), and installs
# the results into 3rdparty/install/.
#
# Env: PRUDYNT_CROSS (cross prefix, ccache prefix supported)
# -----------------------------------------------------------------------------

set -e
set -o pipefail

# Variables
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../3rdparty"
SHIM_REPO="https://github.com/gtxaspec/ingenic-musl"
SHIM_DIR="${BUILD_DIR}/ingenic-musl"
SHIM_VER="be103c48b47ce5491c4ae051793124f877d32f45" # matches buildroot package/ingenic-musl
INSTALL_DIR="${BUILD_DIR}/install"

# Determine if the static archive is needed in addition to the shared library
BUILD_STATIC=0
if [[ "$1" == "-static" ]]; then
    BUILD_STATIC=1
fi

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Clone ingenic-musl if not already present
if [ ! -d "$SHIM_DIR" ]; then
    echo "Cloning ingenic-musl..."
    git clone --depth=1 "$SHIM_REPO"
fi

cd "$SHIM_DIR"

# Checkout pinned revision (fetch it first if the shallow clone lacks it)
git rev-parse -q --verify "${SHIM_VER}^{commit}" >/dev/null 2>&1 || git fetch --depth=1 origin "$SHIM_VER"
git checkout -q "$SHIM_VER"

echo "Building libmuslshim library..."
if [[ $BUILD_STATIC -eq 1 ]]; then
    make CC="${PRUDYNT_CROSS}gcc" -j$(nproc) static
    make CC="${PRUDYNT_CROSS}gcc" -j$(nproc)
else
    make CC="${PRUDYNT_CROSS}gcc" -j$(nproc)
fi

# Install libmuslshim libraries
mkdir -p "$INSTALL_DIR/lib"
cp libmuslshim.* "$INSTALL_DIR/lib/"

echo "libmuslshim build complete!"
