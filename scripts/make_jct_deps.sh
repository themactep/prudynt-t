#!/bin/bash
# -----------------------------------------------------------------------------
# This script automates the process of setting up a cross-compilation
# environment for the JCT (JSON Configuration Tool) library. It prepares the
# build directory, clones the JCT repository if not present, builds the
# library with the cross toolchain (static with -static, shared otherwise),
# and installs the library and header into 3rdparty/install/.
#
# Env: PRUDYNT_CROSS (cross prefix, ccache prefix supported)
# -----------------------------------------------------------------------------

set -e
set -o pipefail

# Variables
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../3rdparty"
JCT_REPO="https://github.com/themactep/jct"
JCT_DIR="${BUILD_DIR}/jct"
JCT_VER="v1.0.0" # matches buildroot package/thingino-jct
INSTALL_DIR="${BUILD_DIR}/install"

# Determine if building static or shared library
BUILD_STATIC=0
if [[ "$1" == "-static" ]]; then
    BUILD_STATIC=1
fi

# Create JCT build directory
echo "Creating JCT build directory..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Clone JCT if not already present
if [ ! -d "$JCT_DIR" ]; then
    echo "Cloning JCT..."
    git clone --depth=1 --branch "$JCT_VER" "$JCT_REPO"
fi

cd "$JCT_DIR"

# Checkout pinned release (fetch the tag first if the shallow clone lacks it)
git rev-parse -q --verify "${JCT_VER}^{commit}" >/dev/null 2>&1 || git fetch --depth=1 origin tag "$JCT_VER"
git checkout -q "$JCT_VER"

echo "Building JCT library..."
make clean

mkdir -p "$INSTALL_DIR/lib" "$INSTALL_DIR/include"

if [[ $BUILD_STATIC -eq 1 ]]; then
    echo "Building JCT static library..."
    make static CROSS_COMPILE="${PRUDYNT_CROSS}"
    cp libjct.a "$INSTALL_DIR/lib/"
else
    echo "Building JCT shared library..."
    make shared CROSS_COMPILE="${PRUDYNT_CROSS}"
    cp libjct.so "$INSTALL_DIR/lib/"
    # Also copy the symlink for proper versioning
    cp -P libjct.so.1 "$INSTALL_DIR/lib/" 2>/dev/null || true
fi

# Install header
cp src/json_config.h "$INSTALL_DIR/include/"

echo "JCT build complete!"
