#!/bin/bash
set -e

# Capture whether PRUDYNT_CROSS was explicitly set before applying the default
_PRUDYNT_CROSS_EXPLICIT=${PRUDYNT_CROSS+set}
: "${PRUDYNT_CROSS:=ccache mipsel-linux-}"

TOP=$(pwd)
NFS_SHARE="/nfs/"

TOOLCHAIN_RELEASE="toolchain-x86_64"
TOOLCHAIN_ARCHIVE="thingino-toolchain-x86_64_xburst1_musl_gcc15-linux-mipsel.tar.gz"
TOOLCHAIN_URL="https://github.com/themactep/thingino-firmware/releases/download/${TOOLCHAIN_RELEASE}/${TOOLCHAIN_ARCHIVE}"
TOOLCHAIN_SDK="${TOP}/toolchain/mipsel-thingino-linux-musl_sdk-buildroot"

ensure_toolchain() {
	[[ -n "$_PRUDYNT_CROSS_EXPLICIT" ]] && return 0

	if [[ ! -d "${TOOLCHAIN_SDK}/bin" ]]; then
		echo "Thingino toolchain not found, downloading..."
		mkdir -p "${TOP}/toolchain"
		if command -v wget &>/dev/null; then
			wget -q --show-progress "${TOOLCHAIN_URL}" -O "${TOP}/toolchain/${TOOLCHAIN_ARCHIVE}"
		else
			curl -L --progress-bar "${TOOLCHAIN_URL}" -o "${TOP}/toolchain/${TOOLCHAIN_ARCHIVE}"
		fi
		echo "Extracting toolchain to ${TOP}/toolchain/ ..."
		tar -xf "${TOP}/toolchain/${TOOLCHAIN_ARCHIVE}" -C "${TOP}/toolchain"
		rm -f "${TOP}/toolchain/${TOOLCHAIN_ARCHIVE}"
		if [[ -x "${TOOLCHAIN_SDK}/relocate-sdk.sh" ]]; then
			echo "Relocating SDK..."
			"${TOOLCHAIN_SDK}/relocate-sdk.sh"
		fi
		echo "Toolchain ready."
	fi

	export PRUDYNT_CROSS="${TOOLCHAIN_SDK}/bin/mipsel-linux-"
}

prudynt() {
	ensure_toolchain
	echo "Build prudynt"

	cd $TOP
	make clean

	# Rebuild live555 to ensure latest changes are included
	echo "Rebuilding live555 with latest changes..."
	cd 3rdparty/live
	if [[ -f Makefile ]]; then
		# Apply local live555 patches if present
		if ls ../../res/live555/*.patch >/dev/null 2>&1; then
			for p in ../../res/live555/*.patch; do
				patch -p1 -N < "$p" || true
			done
		fi

		LIVE555_PREFIX="${TOP}/3rdparty/install"
		LIVE555_LIBDIR="${LIVE555_PREFIX}/lib"
		make clean
		PRUDYNT_ROOT="${TOP}" PRUDYNT_CROSS="${PRUDYNT_CROSS}" \
			make -j$(nproc) PREFIX="${LIVE555_PREFIX}" LIBDIR="${LIVE555_LIBDIR}"
		PRUDYNT_ROOT="${TOP}" PRUDYNT_CROSS="${PRUDYNT_CROSS}" \
			make install PREFIX="${LIVE555_PREFIX}" LIBDIR="${LIVE555_LIBDIR}"
		echo "live555 rebuilt successfully"
	else
		echo "Warning: live555 Makefile not found, skipping live555 rebuild"
	fi
	cd $TOP

	# Parse build type flags - default to dynamic linking (ideal for buildroot/firmware)
	BIN_TYPE=""
	DEBUG_BUILD=0
	for arg in "$@"; do
		if [ "$arg" = "-static" ]; then
			BIN_TYPE="-DBINARY_STATIC"
		elif [ "$arg" = "-hybrid" ]; then
			BIN_TYPE="-DBINARY_HYBRID"
		elif [ "$arg" = "-debug" ]; then
			DEBUG_BUILD=1
			# Force static build for debug to ensure all symbols are included
			if [ -z "$BIN_TYPE" ]; then
				BIN_TYPE="-DBINARY_STATIC"
			fi
		fi
	done
	# If no explicit flag provided, default to dynamic (no flag needed in Makefile)

	# Set debug or release build flags
	if [ $DEBUG_BUILD -eq 1 ]; then
		echo "Building with debug information (no optimization, debug symbols, debug logging)"
		OPTIMIZATION="-g -O0"
		DEBUG_FLAGS="-DENABLE_LOG_DEBUG"
		STRIP_FLAG="DEBUG_STRIP=0"
	else
		OPTIMIZATION="-O2"
		DEBUG_FLAGS=""
		STRIP_FLAG=""
	fi

	# Ensure locally built third-party pkg-configs are found
	export PKG_CONFIG_PATH="$TOP/3rdparty/install/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

	/usr/bin/make -j$(nproc) \
	ARCH= CROSS_COMPILE="${PRUDYNT_CROSS}" \
	CFLAGS="-DPLATFORM_$1 $BIN_TYPE $OPTIMIZATION $DEBUG_FLAGS -DALLOW_RTSP_SERVER_PORT_REUSE=1 -DNO_OPENSSL=1 \
	-isystem ./3rdparty/install/include \
	-isystem ./3rdparty/install/include/liveMedia \
	-isystem ./3rdparty/install/include/groupsock \
	-isystem ./3rdparty/install/include/UsageEnvironment \
	-isystem ./3rdparty/install/include/BasicUsageEnvironment" \
	LDFLAGS=" -L./3rdparty/install/lib" \
	$STRIP_FLAG \
	-C $PWD all

	if [ -d "$NFS_SHARE" ]; then
		echo "DONE. COPYING BINARY TO $NFS_SHARE"
		cp -vf bin/prudynt "$NFS_SHARE"
		cp -vf res/prudynt.json "$NFS_SHARE"
	fi

	exit 0
}

deps() {
	ensure_toolchain
	# Parse flags for dependency builds
	CLEAN_ALL=0
	STATIC_BUILD=0
	HYBRID_BUILD=0
	for arg in "$@"; do
		if [ "$arg" = "--clean-all" ]; then CLEAN_ALL=1; fi
		if [ "$arg" = "-static" ]; then STATIC_BUILD=1; fi
		if [ "$arg" = "-hybrid" ]; then HYBRID_BUILD=1; fi
	done
	if [ $CLEAN_ALL -eq 1 ]; then
		echo "Cleaning 3rdparty/ (requested via --clean-all)"
		rm -rf 3rdparty
	fi
	mkdir -p 3rdparty/install
	mkdir -p 3rdparty/install/include
	CROSS_COMPILE=${PRUDYNT_CROSS}

	echo "Build libhelix-aac"
	cd 3rdparty
	if [[ $STATIC_BUILD -eq 1 || $HYBRID_BUILD -eq 1 ]]; then
		PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_libhelixaac_deps.sh -static
	else
		PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_libhelixaac_deps.sh
	fi
	cd ../

	echo "Build libhelix-mp3"
	LIBHELIX_DIR="$TOP/3rdparty/libhelix-aac"
	MP3_SRC="$LIBHELIX_DIR/src/libhelix-mp3"
	INSTALL_DIR="$TOP/3rdparty/install"
	ARDUINO_COMPAT="$TOP/res/arduino_compat"
	_CC="${PRUDYNT_CROSS#ccache }gcc"
	mkdir -p /tmp/libhelix-mp3-build
	for src in "$MP3_SRC"/*.c; do
		obj="/tmp/libhelix-mp3-build/$(basename "${src%.c}").o"
		"$_CC" -Os -I"$MP3_SRC" -I"$ARDUINO_COMPAT" \
			-DUSE_DEFAULT_STDLIB -DARDUINO -c "$src" -o "$obj"
	done
	"${PRUDYNT_CROSS#ccache }ar" rcs "$INSTALL_DIR/lib/libhelix-mp3.a" /tmp/libhelix-mp3-build/*.o
	find "$MP3_SRC" -maxdepth 1 -name '*.h' -exec cp {} "$INSTALL_DIR/include/" \;
	rm -rf /tmp/libhelix-mp3-build

	echo "Build libflac-lite"
	FLAC_SRC="$LIBHELIX_DIR/src/libflac"
	mkdir -p /tmp/libflac-build
	for src in "$FLAC_SRC"/*.c; do
		obj="/tmp/libflac-build/$(basename "${src%.c}").o"
		"$_CC" -Os -I"$FLAC_SRC" -I"$ARDUINO_COMPAT" \
			-DUSE_DEFAULT_STDLIB -c "$src" -o "$obj"
	done
	"${PRUDYNT_CROSS#ccache }ar" rcs "$INSTALL_DIR/lib/libflac-lite.a" /tmp/libflac-build/*.o
	cp -r "$FLAC_SRC/FLAC" "$INSTALL_DIR/include/"
	find "$FLAC_SRC" -maxdepth 1 -name '*.h' -exec cp {} "$INSTALL_DIR/include/" \;
	rm -rf /tmp/libflac-build

	echo "Build libwebsockets"
	cd 3rdparty
	if [[ $STATIC_BUILD -eq 1 ]]; then
		PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_libwebsockets_deps.sh -static
	else
		PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_libwebsockets_deps.sh
	fi
	cd ../

	echo "Build opus"
	cd 3rdparty
	if [[ $STATIC_BUILD -eq 1 || $HYBRID_BUILD -eq 1 ]]; then
		PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_opus_deps.sh -static
	else
		PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_opus_deps.sh
	fi
	cd ../

	echo "Build libschrift"
	cd 3rdparty

	LIBSCHRIFT_VER="24737d2922b23df4a5692014f5ba03da0c296112"

	# Smart libschrift handling
	if [[ ! -d libschrift ]]; then
		echo "Cloning libschrift..."
		git clone https://github.com/tomolt/libschrift/
		cd libschrift
	else
		echo "libschrift directory exists, using existing version..."
		cd libschrift
	fi
	git checkout "$LIBSCHRIFT_VER"
	# Apply local libschrift patches if present
	if ls ../../res/libschrift/*.patch >/dev/null 2>&1; then
		for p in ../../res/libschrift/*.patch; do
			patch -p1 -N < "$p" || true
		done
	fi
	mkdir -p $TOP/3rdparty/install/lib
	mkdir -p $TOP/3rdparty/install/include
	if [[ $STATIC_BUILD -eq 1 || $HYBRID_BUILD -eq 1 ]]; then
		${PRUDYNT_CROSS}gcc -std=c99 -pedantic -Wall -Wextra -Wconversion -c -o schrift.o schrift.c
		${PRUDYNT_CROSS}ar rc libschrift.a schrift.o
		${PRUDYNT_CROSS}ranlib libschrift.a
		cp libschrift.a $TOP/3rdparty/install/lib/
	else
		${PRUDYNT_CROSS}gcc -std=c99 -pedantic -Wall -Wextra -Wconversion -fPIC -c -o schrift.o schrift.c
		${PRUDYNT_CROSS}gcc -shared -o libschrift.so schrift.o
		cp libschrift.so $TOP/3rdparty/install/lib/
	fi
	cp schrift.h $TOP/3rdparty/install/include/
	cd ../../

	echo "Build JCT (JSON Configuration Tool)"
	cd 3rdparty

	# Smart JCT handling - use existing directory
	if [[ ! -d jct ]]; then
		echo "Cloning JCT..."
		git clone --depth=1 https://github.com/themactep/jct
		cd jct
	else
		cd jct
	fi

	echo "Building JCT library..."
	make clean

	if [[ $STATIC_BUILD -eq 1 || $HYBRID_BUILD -eq 1 ]]; then
		echo "Building JCT static library..."
		make static CROSS_COMPILE="${PRUDYNT_CROSS}"
		cp libjct.a $TOP/3rdparty/install/lib/
	else
		echo "Building JCT shared library..."
		make shared CROSS_COMPILE="${PRUDYNT_CROSS}"
		cp libjct.so $TOP/3rdparty/install/lib/
		# Also copy the symlink for proper versioning
		cp -P libjct.so.1 $TOP/3rdparty/install/lib/ 2>/dev/null || true
	fi

	# Install header
	cp src/json_config.h $TOP/3rdparty/install/include/
	cd ../../

	echo "Build live555"
	cd 3rdparty

	# Smart live555 handling - only clone if directory doesn't exist
	if [[ ! -d live ]]; then
		echo "Cloning live555..."
		git clone --depth=1 https://github.com/themactep/thingino-live555.git live
		cd live
	else
		echo "live555 directory exists, checking for updates..."
		cd live
		# Reset to clean state and pull latest changes
		git reset --hard HEAD
		git clean -fd
		git pull origin master
	fi

	# Workaround: Ensure a trailing space after 'ar cr' in Makefiles (avoids 'crlib...' issue)
	# We do this post-genMakefiles for both static and shared builds
	fix_ar_space() {
		for mk in liveMedia/Makefile groupsock/Makefile UsageEnvironment/Makefile BasicUsageEnvironment/Makefile testProgs/Makefile mediaServer/Makefile proxyServer/Makefile hlsProxy/Makefile; do
			if [[ -f "$mk" ]]; then
				sed -i 's/ar cr$/ar cr /' "$mk" || true
			fi
		done
	}

	if [[ -f Makefile ]]; then
		make distclean
	fi

	# Apply local live555 patches if present
	if ls ../../res/live555/*.patch >/dev/null 2>&1; then
		for p in ../../res/live555/*.patch; do
			patch -p1 -N < "$p" || true
		done
	fi

	if [[ "$2" == "-static" || "$2" == "-hybrid" ]]; then
		echo "STATIC LIVE555"
		cp ../../res/live555-config.prudynt-static ./config.prudynt-static
		./genMakefiles prudynt-static
		fix_ar_space
	else
		echo "SHARED LIVE555"
		patch config.linux-with-shared-libraries ../../res/live555-prudynt.patch --output=./config.prudynt
		./genMakefiles prudynt
		fix_ar_space
	fi

	LIVE555_PREFIX="${TOP}/3rdparty/install"
	LIVE555_LIBDIR="${LIVE555_PREFIX}/lib"
	PRUDYNT_ROOT="${TOP}" PRUDYNT_CROSS="${PRUDYNT_CROSS}" \
		make PREFIX="${LIVE555_PREFIX}" LIBDIR="${LIVE555_LIBDIR}"
	PRUDYNT_ROOT="${TOP}" PRUDYNT_CROSS="${PRUDYNT_CROSS}" \
		make install PREFIX="${LIVE555_PREFIX}" LIBDIR="${LIVE555_LIBDIR}"
	cd ../../

	echo "import libimp"
	cd 3rdparty
	if [[ $CLEAN_ALL -eq 1 ]]; then rm -rf ingenic-lib; fi
	if [[ ! -d ingenic-lib ]]; then
		git clone --depth=1 https://github.com/gtxaspec/ingenic-lib
	fi

	INGENIC_LIB_SRC=""
	case "$1" in
		T10|T20)
			echo "use T20 libs"
			INGENIC_LIB_SRC="ingenic-lib/T20/lib/3.12.0/uclibc/4.7.2"
			;;
		T21)
			echo "use $1 libs"
			INGENIC_LIB_SRC="ingenic-lib/$1/lib/1.0.33/uclibc/5.4.0"
			;;
		T23)
			echo "use $1 libs"
			INGENIC_LIB_SRC="ingenic-lib/$1/lib/1.1.0/uclibc/5.4.0"
			;;
		T30)
			echo "use $1 libs"
			INGENIC_LIB_SRC="ingenic-lib/$1/lib/1.0.5/uclibc/5.4.0"
			;;
		T31)
			echo "use $1 libs"
			INGENIC_LIB_SRC="ingenic-lib/$1/lib/1.1.6/uclibc/5.4.0"
			;;
		C100)
			echo "use $1 libs"
			INGENIC_LIB_SRC="ingenic-lib/$1/lib/2.1.0/uclibc/5.4.0"
			;;
		T40)
			echo "use $1 libs"
			INGENIC_LIB_SRC="ingenic-lib/$1/lib/1.2.0/uclibc/7.2.0"
			;;
		T41)
			echo "use $1 libs"
			INGENIC_LIB_SRC="ingenic-lib/$1/lib/1.2.0/uclibc/7.2.0"
			;;
		*)
			echo "Unsupported or unspecified SoC model."
			;;
	esac

	if [[ -n "$INGENIC_LIB_SRC" ]]; then
		cp -Pf "$INGENIC_LIB_SRC"/* "$TOP/3rdparty/install/lib/"
	fi

	cd ../

	echo "import libmuslshim"
	cd 3rdparty
	if [[ $CLEAN_ALL -eq 1 ]]; then rm -rf ingenic-musl; fi
	if [[ ! -d ingenic-musl ]]; then
		git clone --depth=1 https://github.com/gtxaspec/ingenic-musl
	fi
	cd ingenic-musl
	if [[ $STATIC_BUILD -eq 1 ]]; then
		make CC="${PRUDYNT_CROSS}gcc" -j$(nproc) static
		make CC="${PRUDYNT_CROSS}gcc" -j$(nproc)
	else
		make CC="${PRUDYNT_CROSS}gcc" -j$(nproc)
	fi
	cp libmuslshim.* ../install/lib/
	cd $TOP

	echo "import libaudioshim"
	cd 3rdparty

	# Smart libaudioshim handling
	if [[ ! -d libaudioshim ]]; then
		echo "Cloning libaudioshim..."
		git clone --depth=1 https://github.com/gtxaspec/libaudioshim
		cd libaudioshim
		make CC="${PRUDYNT_CROSS}gcc" -j$(nproc)
		cp libaudioshim.* ../install/lib/
	else
		echo "libaudioshim directory exists, using existing version..."
		cd libaudioshim
		make CC="${PRUDYNT_CROSS}gcc" -j$(nproc)
		cp libaudioshim.* ../install/lib/
	fi
	cd $TOP

	echo "Build curl"
	cd 3rdparty

	CURL_VER="8.19.0"
	CURL_TAR="curl-${CURL_VER}.tar.bz2"
	CURL_URL="https://curl.se/download/${CURL_TAR}"

	if [[ ! -d "curl-${CURL_VER}" ]]; then
		echo "Downloading curl ${CURL_VER}..."
		if command -v wget &>/dev/null; then
			wget -q --show-progress "${CURL_URL}" -O "${CURL_TAR}"
		else
			curl -L --progress-bar "${CURL_URL}" -o "${CURL_TAR}"
		fi
		tar -xf "${CURL_TAR}"
		rm -f "${CURL_TAR}"
	fi

	cd "curl-${CURL_VER}"
	mkdir -p build-cross
	cd build-cross

	if [[ $STATIC_BUILD -eq 1 || $HYBRID_BUILD -eq 1 ]]; then
		CURL_ENABLE_SHARED="--disable-shared --enable-static"
	else
		CURL_ENABLE_SHARED="--enable-shared --disable-static"
	fi

	../configure \
		--host=mipsel-linux \
		CC="${PRUDYNT_CROSS}gcc" \
		--prefix="$TOP/3rdparty/install" \
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
	make -j$(nproc)
	make install
	cd $TOP

	echo "Build faac"
	cd 3rdparty

	FAAC_VER="6d9b02edd268bd2f3377a388ed77dde4f34556c8"

	# Smart faac handling
	if [[ ! -d faac ]]; then
		echo "Cloning faac..."
		git clone https://github.com/knik0/faac.git
		cd faac
	else
		echo "faac directory exists, using existing version..."
		cd faac
	fi
		git reset --hard HEAD 2>/dev/null || true
		git clean -fd 2>/dev/null || true
		git fetch origin
		git checkout "$FAAC_VER"
		# Apply local FAAC patches (warnings/portability fixes)
		if ls ../../res/faac/*.patch >/dev/null 2>&1; then
			for p in ../../res/faac/*.patch; do
				patch -p1 < "$p"
			done
		fi

	# faac uses meson; create a cross-file for mipsel
	cat > /tmp/faac-meson-cross.ini <<-CROSSFILE
		[binaries]
		c = '${PRUDYNT_CROSS}gcc'
		cpp = '${PRUDYNT_CROSS}g++'
		ar = '${PRUDYNT_CROSS}ar'
		strip = '${PRUDYNT_CROSS}strip'
		pkg-config = 'pkg-config'

		[host_machine]
		system = 'linux'
		cpu_family = 'mips'
		cpu = 'mipsel'
		endian = 'little'
	CROSSFILE

	if [[ $STATIC_BUILD -eq 1 || $HYBRID_BUILD -eq 1 ]]; then
		FAAC_DEFAULT_LIB=static
	else
		FAAC_DEFAULT_LIB=shared
	fi

	rm -rf builddir
	CFLAGS="-ffast-math" meson setup builddir \
		--cross-file /tmp/faac-meson-cross.ini \
		--prefix="$TOP/3rdparty/install" \
		--default-library="$FAAC_DEFAULT_LIB" \
		-Dfloating-point=single \
		-Dmax-channels=2
	ninja -C builddir -j$(nproc)
	ninja -C builddir install
	cd ../../

}

if [ $# -eq 0 ]; then
	echo "Standalone Prudynt Build"
	echo "Usage: ./build.sh deps <platform> [options]"
	echo "       ./build.sh prudynt <platform> [options]"
	echo "       ./build.sh full <platform> [options]"
	echo ""
	echo "Platforms: T20, T21, T23, T30, T31, C100, T40, T41"
	echo "Options:   -static | -hybrid | -debug"
	echo "  -static:  Static linking (default for -debug)"
	echo "  -hybrid:  Hybrid linking (some static, some dynamic)"
	echo "  -debug:   Debug build (no optimization, debug symbols, debug logging)"
	exit 1
elif [[ "$1" == "deps" ]]; then
	deps "${@:2}"
elif [[ "$1" == "prudynt" ]]; then
	prudynt "${@:2}"
elif [[ "$1" == "full" ]]; then
	deps "${@:2}"
	prudynt "${@:2}"
fi

exit 0
