#!/bin/bash
# -----------------------------------------------------------------------------
# This script automates the process of setting up a cross-compilation
# environment for the FAAC (AAC encoder) library. It prepares the build
# directory, clones the FAAC repository if not present, checks out the
# pinned revision, applies the local patches (res/faac/), generates a meson
# cross-file for the mipsel toolchain, builds the library with meson/ninja
# (static with -static, shared otherwise), and installs it into
# 3rdparty/install/.
#
# Env: PRUDYNT_CROSS (cross prefix, ccache prefix supported)
# -----------------------------------------------------------------------------

set -e
set -o pipefail

# Variables
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../3rdparty"
FAAC_REPO="https://github.com/knik0/faac.git"
FAAC_DIR="${BUILD_DIR}/faac"
FAAC_VER="79329efee51c9d3545bc4c7179b43a23fe350b6b" # faac-1.50
INSTALL_DIR="${BUILD_DIR}/install"

# Determine if building static or shared library
if [[ "$1" == "-static" ]]; then
    FAAC_DEFAULT_LIB=static
else
    FAAC_DEFAULT_LIB=shared
fi

# Create faac build directory
echo "Creating faac build directory..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Clone faac if not already present
if [[ ! -d "$FAAC_DIR" ]]; then
    echo "Cloning faac..."
    git clone "$FAAC_REPO"
else
    echo "faac directory exists, using existing version..."
fi

cd "$FAAC_DIR"

# Drop previously applied patches / local modifications so re-runs are clean
git reset --hard HEAD 2>/dev/null || true
git clean -fd 2>/dev/null || true
git fetch origin

# Checkout desired version
git checkout "$FAAC_VER"

# Apply local FAAC patches (warnings/portability fixes)
if ls "${SCRIPT_DIR}/../res/faac/"*.patch >/dev/null 2>&1; then
    for p in "${SCRIPT_DIR}/../res/faac/"*.patch; do
        patch -p1 < "$p"
    done
fi

# faac uses meson; create a cross-file for mipsel
# Fix meson.build for newer meson versions (change c_std=gnu99,c99 to c_std=gnu99)
sed -i "s/'c_std=gnu99,c99'/'c_std=gnu99'/g" meson.build

# Meson treats binary values as single executable paths — split ccache from
# the compiler using array syntax so "ccache <prefix>gcc" works.
_BARE_CROSS="${PRUDYNT_CROSS#ccache }"
if [[ "$PRUDYNT_CROSS" != "$_BARE_CROSS" ]]; then
    _MESON_C="['ccache', '${_BARE_CROSS}gcc']"
    _MESON_CPP="['ccache', '${_BARE_CROSS}g++']"
else
    _MESON_C="'${_BARE_CROSS}gcc'"
    _MESON_CPP="'${_BARE_CROSS}g++'"
fi
cat > /tmp/faac-meson-cross.ini <<-CROSSFILE
	[binaries]
	c = ${_MESON_C}
	cpp = ${_MESON_CPP}
	ar = '${_BARE_CROSS}ar'
	strip = '${_BARE_CROSS}strip'
	pkg-config = 'pkg-config'

	[host_machine]
	system = 'linux'
	cpu_family = 'mips'
	cpu = 'mipsel'
	endian = 'little'
CROSSFILE

# Configure the faac build with meson
echo "Configuring faac library..."
rm -rf builddir
CFLAGS="-ffast-math" meson setup builddir \
    --cross-file /tmp/faac-meson-cross.ini \
    --prefix="$INSTALL_DIR" \
    --default-library="$FAAC_DEFAULT_LIB" \
    -Db_lto=false \
    -Dfloating-point=single \
    -Dmax-channels=2

echo "Building faac library..."
ninja -C builddir -j$(nproc)

# Install faac library and headers
echo "Installing faac library and headers..."
ninja -C builddir install

echo "faac build complete!"
