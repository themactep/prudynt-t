#!/bin/bash
# -----------------------------------------------------------------------------
# This script automates the process of setting up a cross-compilation
# environment for the libwebsockets library. It prepares the build
# directory, sets the toolchain for cross-compilation, clones the
# libwebsockets repository if not present, checks out the pinned release,
# applies the local Thingino patches (mirrored from the firmware tree at
# package/all-patches/libwebsockets/<version>/), configures the build using
# CMake, compiles the library, and finally copies the built library and
# relevant headers to the appropriate locations in the repository.
# -----------------------------------------------------------------------------

set -e
set -o pipefail

# Variables
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../3rdparty"
LWS_REPO="https://github.com/warmcat/libwebsockets"
LWS_DIR="${BUILD_DIR}/libwebsockets"
LWS_VER="v4.5.8"
LWS_SOVER="21"
MAKEFILE="$SCRIPT_DIR/../Makefile"

PRUDYNT_CROSS="${PRUDYNT_CROSS#ccache }"

CC="${PRUDYNT_CROSS}gcc"
CXX="${PRUDYNT_CROSS}g++"
STRIP="${PRUDYNT_CROSS}strip --strip-unneeded"

# Create libwebsockets build directory
echo "Creating libwebsockets build directory..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Clone libwebsockets if not already present
if [ ! -d "$LWS_DIR" ]; then
    echo "Cloning libwebsockets..."
    git clone "$LWS_REPO"
fi

cd "$LWS_DIR"

# Drop previously applied patches / local modifications so re-runs are clean
git reset --hard HEAD
git clean -fd

# Checkout desired version
if [[ -n "$LWS_VER" ]]; then
    git checkout $LWS_VER
else
    echo "Pulling libwebsockets master"
fi

# Apply the local Thingino patches
# (kept in sync with package/all-patches/libwebsockets/4.5.8/ in the firmware tree)
git apply ../../res/libwebsockets/0001-fix-ipv6-and-ifa-flags-issues.patch
git apply ../../res/libwebsockets/0002-fix-netlink-sign-conversion.patch

# Create a fresh CMake build dir to avoid stale cross-toolchain cache
rm -rf build
mkdir -p build
cd build

# Configure and build libwebsockets library

CCACHE_LAUNCHER=()
if command -v ccache &>/dev/null; then
    CCACHE_LAUNCHER=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

echo "Configuring libwebsockets library..."
cmake \
-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
-DCMAKE_SYSTEM_NAME=Linux \
-DCMAKE_SYSTEM_PROCESSOR=mipsle \
"${CCACHE_LAUNCHER[@]}" \
-DCMAKE_C_COMPILER=${CC} \
-DCMAKE_CXX_COMPILER=${CXX} \
-DCMAKE_BUILD_TYPE=RELEASE \
-DCMAKE_C_FLAGS="${CMAKE_C_FLAGS} -Os" \
-DCMAKE_CXX_FLAGS="${CMAKE_CXX_FLAGS} -Os" \
-DDISABLE_WERROR=ON \
-DLWS_WITH_LEJP_CONF=ON \
-DLWS_WITH_SSL=OFF \
-DLWS_WITH_LIBCAP=OFF \
-DLWS_CTEST_INTERNET_AVAILABLE=OFF \
-DLWS_WITH_MINIMAL_EXAMPLES=OFF \
-DLWS_IPV6=ON \
-DLWS_WITH_HTTP_BASIC_AUTH=ON \
-DLWS_WITH_HTTP_UNCOMMON_HEADERS=OFF \
-DLWS_WITH_UPNG=OFF \
-DLWS_WITH_GZINFLATE=OFF \
-DLWS_WITH_JPEG=OFF \
-DLWS_WITH_DLO=OFF \
-DLWS_WITH_SECURE_STREAMS=OFF \
-DLWS_WITH_LEJP=ON \
-DLWS_WITH_LHP=OFF \
-DLWS_WITH_JSONRPC=OFF \
-DLWS_WITH_LWSAC=ON \
-DLWS_WITH_CUSTOM_HEADERS=ON \
-DLWS_CLIENT_HTTP_PROXYING=OFF \
-DLWS_WITH_CACHE_NSCOOKIEJAR=OFF \
-DLWS_WITH_EXTERNAL_POLL=ON \
-DLWS_WITHOUT_TESTAPPS=ON \
..

echo "Building libwebsockets library..."
make -j$(nproc)

# Copy libwebsockets library and headers
echo "Copying libwebsockets library and headers..."
$STRIP ./lib/libwebsockets.so.${LWS_SOVER}

mkdir -p ../../install/
mkdir -p ../../install/lib/

cp ./lib/libwebsockets.a ../../install/lib/
cp ./lib/libwebsockets.so.${LWS_SOVER} ../../install/lib/libwebsockets.so

#cp -R ../include/libwebsockets ../../../include/
cp -R include/* ../../install/include/

echo "libwebsockets build complete!"
