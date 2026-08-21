#!/bin/bash
set -e

# Capture whether PRUDYNT_CROSS was explicitly set before applying the default
_PRUDYNT_CROSS_EXPLICIT=${PRUDYNT_CROSS+set}
: "${PRUDYNT_CROSS:=ccache mipsel-linux-}"

TOP=$(pwd)
NFS_SHARE="/nfs"

TOOLCHAIN_RELEASE="toolchain-x86_64"

# Libc selection: "musl" (default, uses ingenic-musl shim) or "uclibc" (uses ingenic-uclibc shim)
LIBC_TYPE="uclibc"

parse_libc_flag() {
	for arg in "$@"; do
		case "$arg" in
			--libc-uclibc) LIBC_TYPE="uclibc" ;;
			--libc-musl)   LIBC_TYPE="musl" ;;
		esac
	done
}

# Apply libc-specific compile flags. The thingino uClibc toolchain is built
# with --disable-libssp, so SSP symbols (__stack_chk_*) are unavailable —
# disable stack-protector globally for uclibc builds.
LIBC_EXTRA_CFLAGS=""
apply_libc_env() {
	if [[ "$LIBC_TYPE" == "uclibc" ]]; then
		LIBC_EXTRA_CFLAGS="-fno-stack-protector"
		export CFLAGS="${CFLAGS:-} $LIBC_EXTRA_CFLAGS"
		export CXXFLAGS="${CXXFLAGS:-} $LIBC_EXTRA_CFLAGS"
		# CMake-based dep scripts append ${CMAKE_C_FLAGS}/${CMAKE_CXX_FLAGS}
		# from the env (libwebsockets/opus/libhelix-aac convention).
		export CMAKE_C_FLAGS="${CMAKE_C_FLAGS:-} $LIBC_EXTRA_CFLAGS"
		export CMAKE_CXX_FLAGS="${CMAKE_CXX_FLAGS:-} $LIBC_EXTRA_CFLAGS"
	fi
}

# Map SOC to xburst generation
get_xburst_generation() {
	local soc="$1"
	case "$soc" in
		T10 | T20 | T21 | T23 | T30 | T31 | C100)
			echo "xburst1"
			;;
		T40 | T41)
			echo "xburst2"
			;;
		*)
			echo "xburst1"  # Default to xburst1 for unknown SOCs
			;;
	esac
}

# Set toolchain variables based on SOC and selected libc
set_toolchain_for_soc() {
	local soc="$1"
	local xburst=$(get_xburst_generation "$soc")

	TOOLCHAIN_ARCHIVE="thingino-toolchain-x86_64_${xburst}_${LIBC_TYPE}_gcc16-linux-mipsel.tar.gz"
	TOOLCHAIN_URL="https://github.com/themactep/thingino-firmware/releases/download/${TOOLCHAIN_RELEASE}/${TOOLCHAIN_ARCHIVE}"
	TOOLCHAIN_SDK="${TOP}/toolchain/${xburst}-${LIBC_TYPE}/mipsel-thingino-linux-${LIBC_TYPE}_sdk-buildroot"
}

# Initialize with default (xburst1) for non-SOC commands
set_toolchain_for_soc "T31"

ensure_toolchain() {
	[[ -n "$_PRUDYNT_CROSS_EXPLICIT" ]] && return 0

	if [[ ! -d "${TOOLCHAIN_SDK}/bin" ]]; then
		echo "Thingino ${LIBC_TYPE} toolchain not found, downloading..."
		local xburst_dir=$(dirname "${TOOLCHAIN_SDK}")
		mkdir -p "${xburst_dir}"
		if command -v wget &>/dev/null; then
			wget -q --show-progress "${TOOLCHAIN_URL}" -O "${TOP}/toolchain/${TOOLCHAIN_ARCHIVE}"
		else
			curl -L --progress-bar "${TOOLCHAIN_URL}" -o "${TOP}/toolchain/${TOOLCHAIN_ARCHIVE}"
		fi
		echo "Extracting toolchain to ${xburst_dir}/ ..."
		tar -xf "${TOP}/toolchain/${TOOLCHAIN_ARCHIVE}" -C "${xburst_dir}"
		rm -f "${TOP}/toolchain/${TOOLCHAIN_ARCHIVE}"
		if [[ -x "${TOOLCHAIN_SDK}/relocate-sdk.sh" ]]; then
			echo "Relocating SDK..."
			"${TOOLCHAIN_SDK}/relocate-sdk.sh"
		fi
		echo "Toolchain ready."
	fi

	if command -v ccache &>/dev/null; then
		export PRUDYNT_CROSS="ccache ${TOOLCHAIN_SDK}/bin/mipsel-linux-"
	else
		export PRUDYNT_CROSS="${TOOLCHAIN_SDK}/bin/mipsel-linux-"
	fi
}

prudynt() {
	local soc="$1"
	shift  # Remove SOC from arguments

	parse_libc_flag "$@"
	set_toolchain_for_soc "$soc"
	ensure_toolchain
	apply_libc_env
	echo "Build prudynt for $soc (libc: $LIBC_TYPE)"

	cd $TOP
	make clean

	# Parse build type flags - default to dynamic linking (ideal for buildroot/firmware)
	BIN_TYPE=""
	DEBUG_BUILD=0
	OSD_BURNIN=0
	OSD_FONT8X8=0
	OSD_FONT_UNIFONT=0
	RTSP_IPV6=0
	OSD_FONT_LIBSCHRIFT=0
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
		elif [ "$arg" = "--osd-burnin" ]; then
			# Restore the burned-in OSD timestamp overlay (off by default)
			OSD_BURNIN=1
		elif [ "$arg" = "--osd-font8x8" ]; then
			# Use the 8x8 full-ASCII font for the burn-in overlay
			OSD_FONT8X8=1
		elif [ "$arg" = "--osd-font-unifont" ]; then
			# Use the Unifont 8x16 font (ASCII + Cyrillic)
			OSD_FONT_UNIFONT=1
		elif [ "$arg" = "--rtsp-ipv6" ]; then
			# Build an IPv6-only RTSP server/SDP generator instead of the
			# default IPv4-only one (no dual-stack mode)
			RTSP_IPV6=1
		elif [ "$arg" = "--osd-font-libschrift" ]; then
			# Use antialiased TrueType rendering via libschrift instead of
			# a fixed bitmap font (needs /usr/share/fonts/default.ttf)
			OSD_FONT_LIBSCHRIFT=1
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

	# libc selection define (Makefile keys on -DLIBC_UCLIBC to pick the uclibc shim)
	LIBC_DEFINE=""
	if [ "$LIBC_TYPE" = "uclibc" ]; then
		LIBC_DEFINE="-DLIBC_UCLIBC"
	fi

	# Ensure locally built third-party pkg-configs are found
	export PKG_CONFIG_PATH="$TOP/3rdparty/install/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

	/usr/bin/make -j$(nproc) \
	ARCH= CROSS_COMPILE="${PRUDYNT_CROSS}" \
	USE_OSD_BURNIN=$OSD_BURNIN \
	USE_OSD_FONT8X8=$OSD_FONT8X8 \
	USE_OSD_FONT_UNIFONT=$OSD_FONT_UNIFONT \
	USE_RTSP_IPV6=$RTSP_IPV6 \
	USE_OSD_FONT_LIBSCHRIFT=$OSD_FONT_LIBSCHRIFT \
	CFLAGS="-DPLATFORM_${soc} $BIN_TYPE $LIBC_DEFINE $LIBC_EXTRA_CFLAGS $OPTIMIZATION $DEBUG_FLAGS -DNO_OPENSSL=1 \
	-isystem ./3rdparty/install/include" \
	LDFLAGS=" -L./3rdparty/install/lib" \
	$STRIP_FLAG \
	-C $PWD all

	if [ -d "$NFS_SHARE" ]; then
		echo "DONE. COPYING BINARY TO $NFS_SHARE"
		SOC_LOWER=$(echo "$soc" | tr '[:upper:]' '[:lower:]')
		cp -vf bin/prudynt "$NFS_SHARE/prudynt-$SOC_LOWER"
		cp -vf res/prudynt.json "$NFS_SHARE/prudynt-$SOC_LOWER.json"
	fi

	exit 0
}

deps() {
	local soc="$1"
	shift  # Remove SOC from arguments

	parse_libc_flag "$@"
	set_toolchain_for_soc "$soc"
	ensure_toolchain
	apply_libc_env
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
	cd 3rdparty
	PRUDYNT_CROSS=$PRUDYNT_CROSS LIBC_EXTRA_CFLAGS="$LIBC_EXTRA_CFLAGS" ../scripts/make_libhelixmp3_deps.sh
	cd ../

	echo "Build libflac-lite"
	cd 3rdparty
	PRUDYNT_CROSS=$PRUDYNT_CROSS LIBC_EXTRA_CFLAGS="$LIBC_EXTRA_CFLAGS" ../scripts/make_libflaclite_deps.sh
	cd ../

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

	# libschrift is no longer linked by prudynt (burned-in OSD removed in b718ed5);
	# scripts/make_libschrift_deps.sh is kept dormant for manual use.

	echo "Build JCT (JSON Configuration Tool)"
	cd 3rdparty
	if [[ $STATIC_BUILD -eq 1 || $HYBRID_BUILD -eq 1 ]]; then
		PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_jct_deps.sh -static
	else
		PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_jct_deps.sh
	fi
	cd ../

	echo "import libimp"
	cd 3rdparty
	if [[ $CLEAN_ALL -eq 1 ]]; then rm -rf ingenic-lib; fi
	if [[ ! -d ingenic-lib ]]; then
		git clone --depth=1 https://github.com/gtxaspec/ingenic-lib
	fi

	INGENIC_LIB_SRC=""
	case "$soc" in
		T10|T20)
			echo "use T20 libs"
			INGENIC_LIB_SRC="ingenic-lib/T20/lib/3.12.0/uclibc/4.7.2"
			;;
		T21)
			echo "use $soc libs"
			INGENIC_LIB_SRC="ingenic-lib/$soc/lib/1.0.33/uclibc/5.4.0"
			;;
		T23)
			echo "use $soc libs"
			INGENIC_LIB_SRC="ingenic-lib/$soc/lib/1.1.0/uclibc/5.4.0"
			;;
		T30)
			echo "use $soc libs"
			INGENIC_LIB_SRC="ingenic-lib/$soc/lib/1.0.5/uclibc/5.4.0"
			;;
		T31)
			echo "use $soc libs"
			INGENIC_LIB_SRC="ingenic-lib/$soc/lib/1.1.6/uclibc/5.4.0"
			;;
		C100)
			echo "use $soc libs"
			INGENIC_LIB_SRC="ingenic-lib/$soc/lib/2.1.0/uclibc/5.4.0"
			;;
		T40)
			echo "use $soc libs"
			INGENIC_LIB_SRC="ingenic-lib/$soc/lib/1.2.0/uclibc/7.2.0"
			;;
		T41)
			echo "use $soc libs"
			INGENIC_LIB_SRC="ingenic-lib/$soc/lib/1.2.5/uclibc/7.2.0"
			;;
		*)
			echo "Unsupported or unspecified SoC model."
			;;
	esac

	if [[ -n "$INGENIC_LIB_SRC" ]]; then
		cp -Pf "$INGENIC_LIB_SRC"/* "$TOP/3rdparty/install/lib/"
	fi

	cd ../

	if [[ "$LIBC_TYPE" == "uclibc" ]]; then
		echo "Build libuclibcshim"
		cd 3rdparty
		if [[ $STATIC_BUILD -eq 1 || $HYBRID_BUILD -eq 1 ]]; then
			PRUDYNT_CROSS=$PRUDYNT_CROSS LIBC_EXTRA_CFLAGS="$LIBC_EXTRA_CFLAGS" ../scripts/make_uclibcshim_deps.sh -static
		else
			PRUDYNT_CROSS=$PRUDYNT_CROSS LIBC_EXTRA_CFLAGS="$LIBC_EXTRA_CFLAGS" ../scripts/make_uclibcshim_deps.sh
		fi
		cd $TOP
	else
		echo "Build libmuslshim"
		cd 3rdparty
		if [[ $STATIC_BUILD -eq 1 ]]; then
			PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_muslshim_deps.sh -static
		else
			PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_muslshim_deps.sh
		fi
		cd $TOP
	fi

if false; then
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
fi

	echo "Build curl"
	cd 3rdparty
	if [[ $STATIC_BUILD -eq 1 || $HYBRID_BUILD -eq 1 ]]; then
		PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_curl_deps.sh -static
	else
		PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_curl_deps.sh
	fi
	cd $TOP

	echo "Build faac"
	cd 3rdparty
	if [[ $STATIC_BUILD -eq 1 || $HYBRID_BUILD -eq 1 ]]; then
		PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_faac_deps.sh -static
	else
		PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_faac_deps.sh
	fi
	cd ../

}

if [ $# -eq 0 ]; then
	echo "Standalone Prudynt Build"
	echo "Usage: ./build.sh deps <platform> [options]"
	echo "       ./build.sh prudynt <platform> [options]"
	echo "       ./build.sh full <platform> [options]"
	echo ""
	echo "Platforms: T20, T21, T23, T30, T31, C100, T40, T41"
	echo "Options:   -static | -hybrid | -debug | --libc-musl | --libc-uclibc"
	echo "  -static:        Static linking (default for -debug)"
	echo "  -hybrid:        Hybrid linking (some static, some dynamic)"
	echo "  -debug:         Debug build (no optimization, debug symbols, debug logging)"
	echo "  --libc-musl:    Use ingenic-musl shim (default)"
	echo "  --libc-uclibc:  Use thingino uClibc toolchain + ingenic-uclibc shim"
	exit 1
elif [[ "$1" == "deps" ]]; then
	deps "${@:2}"
elif [[ "$1" == "prudynt" ]]; then
	prudynt "${@:2}"
elif [[ "$1" == "full" ]]; then
	echo "Removing 3rdparty for a fresh full build..."
	rm -rf "${TOP}/3rdparty"
	deps "${@:2}"
	prudynt "${@:2}"
fi

exit 0
