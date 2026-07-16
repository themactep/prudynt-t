#!/bin/bash
# -----------------------------------------------------------------------------
# This script automates the process of setting up a cross-compilation
# environment for the curl library. It prepares the build directory,
# downloads and unpacks the pinned curl release tarball if not present,
# configures a minimal HTTP-only build (no SSL, no compression, most
# protocols disabled), compiles the library, and installs it into
# 3rdparty/install/.
#
# Env: PRUDYNT_CROSS (cross prefix, ccache prefix supported),
#      CFLAGS (inherited by configure, e.g. libc-specific flags)
# -----------------------------------------------------------------------------

set -e
set -o pipefail

# Variables
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../3rdparty"
CURL_VER="8.20.0"
CURL_TAR="curl-${CURL_VER}.tar.bz2"
CURL_URL="https://curl.se/download/${CURL_TAR}"
CURL_DIR="${BUILD_DIR}/curl-${CURL_VER}"
INSTALL_DIR="${BUILD_DIR}/install"

# Determine if building static or shared library
if [[ "$1" == "-static" ]]; then
    CURL_ENABLE_SHARED="--disable-shared --enable-static"
else
    CURL_ENABLE_SHARED="--enable-shared --disable-static"
fi

# Create curl build directory
echo "Creating curl build directory..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Download and unpack curl if not already present
if [[ ! -d "$CURL_DIR" ]]; then
    echo "Downloading curl ${CURL_VER}..."
    if command -v wget &>/dev/null; then
        wget -q --show-progress "${CURL_URL}" -O "${CURL_TAR}"
    else
        curl -L --progress-bar "${CURL_URL}" -o "${CURL_TAR}"
    fi
    tar -xf "${CURL_TAR}"
    rm -f "${CURL_TAR}"
fi

cd "$CURL_DIR"
mkdir -p build-cross
cd build-cross

# Configure the curl build
echo "Configuring curl library..."
../configure \
    --host=mipsel-linux \
    CC="${PRUDYNT_CROSS}gcc" \
    --prefix="$INSTALL_DIR" \
    --without-ssl \
    --without-libpsl \
    --disable-dict --disable-file --disable-ftp \
    --disable-gopher --disable-imap --disable-ldap \
    --disable-pop3 --disable-rtsp --disable-smtp \
    --disable-telnet --disable-tftp \
    --disable-manual --disable-docs \
    --without-zlib --without-brotli --without-zstd \
    $CURL_ENABLE_SHARED \
    --quiet

echo "Building curl library..."
make -j$(nproc)

# Install curl library and headers
echo "Installing curl library and headers..."
make install

echo "curl build complete!"
