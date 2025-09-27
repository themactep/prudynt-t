#!/bin/bash
set -e
: "${PRUDYNT_CROSS:=ccache mipsel-linux-}"
:
TOP=$(pwd)
NFS_SHARE="/nfs/"

prudynt() {
	echo "Build prudynt"

	cd $TOP
	make clean

	# Rebuild live555 to ensure latest changes are included
	echo "Rebuilding live555 with latest changes..."
	cd 3rdparty/live
	if [[ -f Makefile ]]; then
		make clean
		PRUDYNT_ROOT="${TOP}" PRUDYNT_CROSS="${PRUDYNT_CROSS}" make -j$(nproc)
		PRUDYNT_ROOT="${TOP}" PRUDYNT_CROSS="${PRUDYNT_CROSS}" make install
		echo "live555 rebuilt successfully"
	else
		echo "Warning: live555 Makefile not found, skipping live555 rebuild"
	fi
	cd $TOP

	# Parse build type flags - default to dynamic linking (ideal for buildroot/firmware)
	BIN_TYPE=""
	for arg in "$@"; do
		if [ "$arg" = "-static" ]; then
			BIN_TYPE="-DBINARY_STATIC"
		elif [ "$arg" = "-hybrid" ]; then
			BIN_TYPE="-DBINARY_HYBRID"
		fi
	done
	# If no explicit flag provided, default to dynamic (no flag needed in Makefile)

	# Detect optional -ffmpeg flag in args to enable USE_FFMPEG in Makefile
	FFMPEG_FLAG=""
	for arg in "$@"; do
		if [ "$arg" = "-ffmpeg" ]; then
			FFMPEG_FLAG="USE_FFMPEG=1"
		fi
	done

	# Ensure cross-built FFmpeg pkg-configs are found
	export PKG_CONFIG_PATH="$TOP/3rdparty/install/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

	/usr/bin/make -j$(nproc) \
	ARCH= CROSS_COMPILE="${PRUDYNT_CROSS}" \
	CFLAGS="-DPLATFORM_$1 $BIN_TYPE -O2 -DALLOW_RTSP_SERVER_PORT_REUSE=1 -DNO_OPENSSL=1 \
	-isystem ./3rdparty/install/include \
	-isystem ./3rdparty/install/include/liveMedia \
	-isystem ./3rdparty/install/include/groupsock \
	-isystem ./3rdparty/install/include/UsageEnvironment \
	-isystem ./3rdparty/install/include/BasicUsageEnvironment" \
	LDFLAGS=" -L./3rdparty/install/lib" \
	-C $PWD all

	if [ -d "$NFS_SHARE" ]; then
		echo "DONE. COPYING BINARY TO $NFS_SHARE"
		cp -vf bin/prudynt "$NFS_SHARE"
		cp -vf res/prudynt.json "$NFS_SHARE"
	fi

	exit 0
}

deps() {
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

	# Smart libschrift handling
	if [[ ! -d libschrift ]]; then
		echo "Cloning libschrift..."
		git clone --depth=1 https://github.com/tomolt/libschrift/
		cd libschrift
		git apply ../../res/libschrift.patch
	else
		echo "libschrift directory exists, using existing version..."
		cd libschrift
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

	echo "Build cJSON"
	cd 3rdparty
	rm -rf cJSON
	git clone --depth=1 https://github.com/DaveGamble/cJSON.git cJSON
	cd cJSON

	# Apply patch to use spaces instead of tabs for formatting
	echo "Applying cJSON spaces patch..."
	patch -p1 < ../../res/cjson-spaces.patch

	mkdir -p build
	cd build
	if [[ $STATIC_BUILD -eq 1 ]]; then
		SHARED=OFF
	else
		SHARED=ON
	fi

	# Extract compiler path without ccache for cmake
	COMPILER_PATH=$(echo "${PRUDYNT_CROSS}" | sed 's/ccache //')

	cmake -DCMAKE_SYSTEM_NAME=Linux \
		-DCMAKE_C_COMPILER_LAUNCHER=$(which ccache) \
		-DCMAKE_C_COMPILER="${COMPILER_PATH}gcc" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$TOP/3rdparty/install" \
		-DBUILD_SHARED_LIBS=${SHARED} \
		-DENABLE_CJSON_TEST=OFF \
		..
	make -j$(nproc)
	make install
	cd ../../../

	echo "Build live555"
	cd 3rdparty

	# Smart live555 handling - only clone if directory doesn't exist
	if [[ ! -d live ]]; then
		echo "Cloning live555..."
		git clone https://github.com/themactep/thingino-live555.git live
		cd live
	else
		echo "live555 directory exists, checking for updates..."
		cd live
		# Reset to clean state and pull latest changes
		git reset --hard HEAD
		git clean -fd
		git pull origin master
	fi

	if [[ -f Makefile ]]; then
		make distclean
	fi

	if [[ "$2" == "-static" || "$2" == "-hybrid" ]]; then
		echo "STATIC LIVE555"
		cp ../../res/live555-config.prudynt-static ./config.prudynt-static
		./genMakefiles prudynt-static
	else
		echo "SHARED LIVE555"
		patch config.linux-with-shared-libraries ../../res/live555-prudynt.patch --output=./config.prudynt
		./genMakefiles prudynt
	fi

	PRUDYNT_ROOT="${TOP}" PRUDYNT_CROSS="${PRUDYNT_CROSS}" make -j$(nproc)
	PRUDYNT_ROOT="${TOP}" PRUDYNT_CROSS="${PRUDYNT_CROSS}" make install
	cd ../../

	echo "import libimp"
	cd 3rdparty
	if [[ $CLEAN_ALL -eq 1 ]]; then rm -rf ingenic-lib; fi
	if [[ ! -d ingenic-lib ]]; then
	git clone --depth=1 https://github.com/gtxaspec/ingenic-lib

	case "$1" in
		T10|T20)
			echo "use T20 libs"
			cp ingenic-lib/T20/lib/3.12.0/uclibc/4.7.2/* $TOP/3rdparty/install/lib
			;;
		T21)
			echo "use $1 libs"
			cp ingenic-lib/$1/lib/1.0.33/uclibc/5.4.0/* $TOP/3rdparty/install/lib
			;;
		T23)
			echo "use $1 libs"
			cp ingenic-lib/$1/lib/1.1.0/uclibc/5.4.0/* $TOP/3rdparty/install/lib
			;;
		T30)
			echo "use $1 libs"
			cp ingenic-lib/$1/lib/1.0.5/uclibc/5.4.0/* $TOP/3rdparty/install/lib
			;;
		T31)
			echo "use $1 libs"
			cp ingenic-lib/$1/lib/1.1.6/uclibc/5.4.0/* $TOP/3rdparty/install/lib
			;;
		C100)
			echo "use $1 libs"
			cp ingenic-lib/$1/lib/2.1.0/uclibc/5.4.0/* $TOP/3rdparty/install/lib
			;;
		T40)
			echo "use $1 libs"
			cp ingenic-lib/$1/lib/1.2.0/uclibc/7.2.0/* $TOP/3rdparty/install/lib
			;;
		T41)
			echo "use $1 libs"
			cp ingenic-lib/$1/lib/1.2.0/uclibc/7.2.0/* $TOP/3rdparty/install/lib
			;;
		*)
			echo "Unsupported or unspecified SoC model."
			;;
	esac
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

	echo "Build faac"
	cd 3rdparty

	# Smart faac handling
	if [[ ! -d faac ]]; then
		echo "Cloning faac..."
		git clone --depth=1 https://github.com/knik0/faac.git
		cd faac
		sed -i 's/^#define MAX_CHANNELS 64/#define MAX_CHANNELS 2/' libfaac/coder.h
	else
		echo "faac directory exists, using existing version..."
		cd faac
	fi
	./bootstrap
	if [[ $STATIC_BUILD -eq 1 || $HYBRID_BUILD -eq 1 ]]; then
		CC="${PRUDYNT_CROSS}gcc" ./configure --host mipsel-linux-gnu --prefix="$TOP/3rdparty/install" --enable-static --disable-shared
	else
		CC="${PRUDYNT_CROSS}gcc" ./configure --host mipsel-linux-gnu --prefix="$TOP/3rdparty/install" --disable-static --enable-shared
	fi
	make -j$(nproc)
	make install
	cd ../../

	# Optional: Build FFmpeg when -ffmpeg is requested
	if [[ "$3" == "-ffmpeg" || "$4" == "-ffmpeg" || "$5" == "-ffmpeg" ]]; then
		echo "Build FFmpeg minimal (parsers + BSFs)"
		PRUDYNT_CROSS=$PRUDYNT_CROSS ../scripts/make_ffmpeg_deps.sh "$2"
	fi
}

if [ $# -eq 0 ]; then
	echo "Standalone Prudynt Build"
	echo "Usage: ./build.sh deps <platform> [options]"
	echo "       ./build.sh prudynt <platform> [options]"
	echo "       ./build.sh full <platform> [options]"
	echo ""
	echo "Platforms: T20, T21, T23, T30, T31, C100, T40, T41"
	echo "Options:   -static | -hybrid | -ffmpeg (enable USE_FFMPEG)"
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
