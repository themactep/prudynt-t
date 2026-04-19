#!/bin/bash
# Configurable compilation wrapper for daynightd with dependency management

set -e

# --- Configuration ---
# Source global thingino config if present
if [ -f "$HOME/.thingino_config" ]; then
    source "$HOME/.thingino_config"
fi

# Default cross-compiler prefix
CROSS_COMPILE="${CROSS_COMPILE:-mipsel-linux-}"
CC="${CROSS_COMPILE}gcc"
AR="${CROSS_COMPILE}ar"
STRIP="${CROSS_COMPILE}strip"

# Default feature flags (set to 1 to enable, 0 to disable)
ENABLE_SQLITE=${ENABLE_SQLITE:-0}
ENABLE_MQTT=${ENABLE_MQTT:-0}
ENABLE_GRAPHITE=${ENABLE_GRAPHITE:-0}

# Parse command line arguments for overrides
for arg in "$@"; do
    case $arg in
        --enable-sqlite)   ENABLE_SQLITE=1 ;;
        --enable-mqtt)     ENABLE_MQTT=1 ;;
        --enable-graphite) ENABLE_GRAPHITE=1 ;;
        --disable-sqlite)  ENABLE_SQLITE=0 ;;
        --disable-mqtt)    ENABLE_MQTT=0 ;;
        --disable-graphite) ENABLE_GRAPHITE=0 ;;
        --all)             ENABLE_SQLITE=1; ENABLE_MQTT=1; ENABLE_GRAPHITE=1 ;;
        --minimal)         ENABLE_SQLITE=0; ENABLE_MQTT=0; ENABLE_GRAPHITE=0 ;;
        *)
            if [[ $arg != mipsel-* ]]; then
                echo "Unknown option: $arg"
                echo "Usage: $0 [CROSS_COMPILE_PREFIX] [--enable-sqlite] [--enable-mqtt] [--enable-graphite] [--all] [--minimal]"
            else
                CROSS_COMPILE="$arg"
                CC="${CROSS_COMPILE}gcc"
                AR="${CROSS_COMPILE}ar"
                STRIP="${CROSS_COMPILE}strip"
            fi
            ;;
    esac
done

# Directories
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
THIRDPARTY_DIR="$PROJECT_DIR/3rdparty"
INSTALL_DIR="$THIRDPARTY_DIR/install"

mkdir -p "$INSTALL_DIR/include" "$INSTALL_DIR/lib"

# --- SQLite3 ---
if [ "$ENABLE_SQLITE" -eq 1 ]; then
    SQLITE_VER="3450300" # 3.45.3
    SQLITE_URL="https://www.sqlite.org/2024/sqlite-amalgamation-$SQLITE_VER.zip"

    if [ ! -f "$INSTALL_DIR/lib/libsqlite3.a" ]; then
        echo "--- Fetching and Building SQLite3 ---"
        mkdir -p "$THIRDPARTY_DIR"
        TEMP_DIR=$(mktemp -d /tmp/sqlite_XXXX)
        wget -q "$SQLITE_URL" -O "$TEMP_DIR/sqlite.zip"
        unzip -q "$TEMP_DIR/sqlite.zip" -d "$TEMP_DIR"
        cd "$TEMP_DIR/sqlite-amalgamation-$SQLITE_VER"
        
        # Build a static library
        $CC -O2 -fPIC -c sqlite3.c -o sqlite3.o
        $AR rcs "$INSTALL_DIR/lib/libsqlite3.a" sqlite3.o
        cp sqlite3.h sqlite3ext.h "$INSTALL_DIR/include/"
        
        cd "$PROJECT_DIR"
        rm -rf "$TEMP_DIR"
        echo "    SQLite3 built and installed to $INSTALL_DIR"
    fi
fi

# --- Mosquitto ---
if [ "$ENABLE_MQTT" -eq 1 ]; then
    MOSQ_VER="2.0.18"
    MOSQ_URL="https://mosquitto.org/files/source/mosquitto-$MOSQ_VER.tar.gz"

    if [ ! -f "$INSTALL_DIR/lib/libmosquitto.a" ]; then
        echo "--- Fetching and Building Mosquitto (client lib) ---"
        mkdir -p "$THIRDPARTY_DIR"
        TEMP_DIR=$(mktemp -d /tmp/mosq_XXXX)
        wget -q "$MOSQ_URL" -O "$TEMP_DIR/mosq.tar.gz"
        tar -xf "$TEMP_DIR/mosq.tar.gz" -C "$TEMP_DIR"
        cd "$TEMP_DIR/mosquitto-$MOSQ_VER"
        
        # Build only libmosquitto manually to avoid build system complexity and dependencies
        # Disable features in config.h that require extra libraries (TLS, etc)
        sed -i 's/^#define WITH_TLS$/\/* #undef WITH_TLS *\//' config.h
        sed -i 's/^#define WITH_TLS_PSK$/\/* #undef WITH_TLS_PSK *\//' config.h
        sed -i 's/^#define WITH_EC$/\/* #undef WITH_EC *\//' config.h
        
        cd lib
        # Include both lib/, include/, root/ and deps/ for internal and external headers
        $CC -O2 -fPIC -c *.c -I. -I.. -I../include -I../deps
        $AR rcs "$INSTALL_DIR/lib/libmosquitto.a" *.o
        cp ../include/mosquitto.h "$INSTALL_DIR/include/"
        
        cd "$PROJECT_DIR"
        rm -rf "$TEMP_DIR"
        echo "    Libmosquitto built and installed to $INSTALL_DIR"
    fi
fi

# --- Main Compilation ---
echo "--- Compiling daynightd (SQLite: $ENABLE_SQLITE, MQTT: $ENABLE_MQTT, Graphite: $ENABLE_GRAPHITE) ---"

# Build flags
CFLAGS="-I$INSTALL_DIR/include"
LDFLAGS="-L$INSTALL_DIR/lib"

make -C "$PROJECT_DIR" \
    CROSS_COMPILE="$CROSS_COMPILE" \
    CFLAGS="$CFLAGS" \
    LDFLAGS="$LDFLAGS" \
    ENABLE_SQLITE="$ENABLE_SQLITE" \
    ENABLE_MQTT="$ENABLE_MQTT" \
    ENABLE_GRAPHITE="$ENABLE_GRAPHITE"

if [ $? -eq 0 ]; then
    echo "--- Build successful: ../daynightd ---"
    ls -lh ../daynightd
else
    echo "--- Build failed ---"
    exit 1
fi
