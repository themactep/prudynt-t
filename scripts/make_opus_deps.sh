#!/bin/bash
# -----------------------------------------------------------------------------
# This script automates the process of setting up a cross-compilation
# environment for the Opus library using CMake. It prepares the build
# directory, sets the toolchain for cross-compilation, clones the Opus
# repository if not present, configures the build using CMake, compiles the
# library, and finally copies the built library and relevant headers to the
# appropriate locations in the repository.
# -----------------------------------------------------------------------------

set -e
set -o pipefail

# Variables
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../3rdparty"
OPUS_REPO="https://github.com/xiph/opus"
OPUS_DIR="${BUILD_DIR}/opus"
OPUS_VER="22244de5a79bd1d6d623c32e72bf1954b56235be" # v1.6.1, matches buildroot package/opus
MAKEFILE="$SCRIPT_DIR/../Makefile"

PRUDYNT_CROSS="${PRUDYNT_CROSS#ccache }"

CC="${PRUDYNT_CROSS}gcc"
CXX="${PRUDYNT_CROSS}g++"
STRIP="${PRUDYNT_CROSS}strip --strip-unneeded"

# Determine if building static or shared library
BUILD_SHARED_LIBS=ON
if [[ "$1" == "-static" ]]; then
    BUILD_SHARED_LIBS=OFF
fi

# Create Opus build directory
echo "Creating Opus build directory..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Clone Opus if not already present
if [ ! -d "$OPUS_DIR" ]; then
    echo "Cloning Opus..."
    git clone "$OPUS_REPO"
fi

cd "$OPUS_DIR"

# Checkout desired version (fetch it first if the clone lacks it)
if [[ -n "$OPUS_VER" ]]; then
    git rev-parse -q --verify "${OPUS_VER}^{commit}" >/dev/null || git fetch origin
    git checkout -q $OPUS_VER
else
    echo "Pulling Opus master"
fi

# Create a fresh CMake build directory to avoid stale cross-toolchain cache
rm -rf build
mkdir -p build
cd build

CCACHE_LAUNCHER=()
if command -v ccache &>/dev/null; then
    CCACHE_LAUNCHER=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

# Configure the Opus build with CMake
# (OPUS_FLOAT_APPROX mirrors the firmware tree default from
#  package/all-patches/opus/0001-thingino-default-build-options.patch)
echo "Configuring Opus library..."
cmake \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=mipsle \
    "${CCACHE_LAUNCHER[@]}" \
    -DCMAKE_C_COMPILER=${CC} \
    -DCMAKE_CXX_COMPILER=${CXX} \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="${CMAKE_C_FLAGS} -Os" \
    -DCMAKE_CXX_FLAGS="${CMAKE_CXX_FLAGS} -Os" \
    -DCMAKE_INSTALL_PREFIX="${BUILD_DIR}/install" \
    -DBUILD_SHARED_LIBS=${BUILD_SHARED_LIBS} \
    -DOPUS_FLOAT_APPROX=ON \
    -DOPUS_STACK_PROTECTOR=OFF \
    ..

echo "Building Opus library..."
make -j$(nproc)

if [ -f "${BUILD_DIR}/install/lib/libopus.so" ]; then
	$STRIP "${BUILD_DIR}/install/lib/libopus.so"
fi

# Install Opus library and headers
echo "Installing Opus library and headers..."
make install

echo "Opus build complete!"
