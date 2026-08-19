# =============================================================================
# Prudynt-T Makefile
# =============================================================================

# Compiler Configuration
# ----------------------
CC                      = ${CROSS_COMPILE}gcc
CXX                     = ${CROSS_COMPILE}g++

# Compiler Flags
# --------------
DEFAULT_WARN_CFLAGS    := -Wall -Wextra -Wno-unused-parameter
DEFAULT_OPTFLAG        ?= -O2

override CFLAGS        += $(DEFAULT_WARN_CFLAGS)
ifeq ($(findstring -O,$(CFLAGS)),)
override CFLAGS        += $(DEFAULT_OPTFLAG)
endif
override CFLAGS        += -DNO_OPENSSL=1

CXXFLAGS               += $(CFLAGS) -std=c++20 -Wall -Wextra -Wno-unused-parameter
LDFLAGS                += -lrt -lpthread

# Allow legacy build systems to keep exporting WEBSOCKET_ENABLED=0/1
ifeq ($(origin USE_WEBSOCKETS), undefined)
ifneq ($(strip $(WEBSOCKET_ENABLED)),)
USE_WEBSOCKETS         := $(WEBSOCKET_ENABLED)
endif
endif
USE_WEBSOCKETS         ?= 1

ifeq ($(USE_WEBSOCKETS),1)
override CFLAGS        += -DWEBSOCKET_ENABLED
endif

# Pre-trigger buffer support
# ---------------------------
USE_PREBUFFER          ?= 1

ifeq ($(USE_PREBUFFER),1)
override CFLAGS        += -DPREBUFFER_ENABLED
endif

# Burned-in OSD timestamp overlay
# -------------------------------
# Draws the camera date/time directly into the encoded video via a hardware
# OSD region (upstream replaced this with SEI metadata). Opt-in, off by
# default; enable with `make USE_OSD_BURNIN=1` or build.sh --osd-burnin.
USE_OSD_BURNIN         ?= 0

ifeq ($(USE_OSD_BURNIN),1)
override CFLAGS        += -DOSD_BURN_TIMESTAMP

# OSD burn-in font: 5x7 (default), 8x8 (full ASCII), or Unifont 8x16
# (full ASCII + Cyrillic).  Select with `make USE_OSD_BURNIN=1
# USE_OSD_FONT8X8=1` or `USE_OSD_FONT_UNIFONT=1`.
# Only meaningful together with the burn-in overlay above.
USE_OSD_FONT8X8         ?= 0
ifeq ($(USE_OSD_FONT8X8),1)
override CFLAGS        += -DUSE_OSD_FONT_8X8
endif
USE_OSD_FONT_UNIFONT    ?= 0
ifeq ($(USE_OSD_FONT_UNIFONT),1)
override CFLAGS        += -DUSE_OSD_FONT_UNIFONT
endif

# Antialiased TrueType rendering via libschrift instead of a fixed bitmap
# font (mutually exclusive with the two flags above). Needs
# /usr/share/fonts/default.ttf present on the camera at runtime.
USE_OSD_FONT_LIBSCHRIFT ?= 0
ifeq ($(USE_OSD_FONT_LIBSCHRIFT),1)
override CFLAGS        += -DUSE_OSD_FONT_LIBSCHRIFT
endif
endif

ifeq ($(USE_OSD_FONT_LIBSCHRIFT),1)
OSD_SCHRIFT_LIB_STATIC_LINE  = -l:libschrift.a
OSD_SCHRIFT_LIB_HYBRID_LINE  = -lschrift
OSD_SCHRIFT_LIB_DYNAMIC_LINE = -lschrift
else
OSD_SCHRIFT_LIB_STATIC_LINE  =
OSD_SCHRIFT_LIB_HYBRID_LINE  =
OSD_SCHRIFT_LIB_DYNAMIC_LINE =
endif

# Optional FLAC support
# ----------------------
USE_FLAC               ?= 1

ifeq ($(USE_FLAC),1)
FLAC_LIB_STATIC_LINE   = -l:libflac-lite.a
FLAC_LIB_HYBRID_LINE   = -lflac-lite
FLAC_LIB_DYNAMIC_LINE  = -lflac-lite
else
FLAC_LIB_STATIC_LINE   =
FLAC_LIB_HYBRID_LINE   =
FLAC_LIB_DYNAMIC_LINE  =
endif

# Optional MP3 support
# ---------------------
USE_MP3                ?= 1

ifeq ($(USE_MP3),1)
MP3_LIB_STATIC_LINE    = -l:libhelix-mp3.a
MP3_LIB_HYBRID_LINE    = -lhelix-mp3
MP3_LIB_DYNAMIC_LINE   = -lhelix-mp3
else
MP3_LIB_STATIC_LINE    =
MP3_LIB_HYBRID_LINE    =
MP3_LIB_DYNAMIC_LINE   =
endif

# Optional Opus support
# ----------------------
USE_OPUS               ?= 1

ifeq ($(USE_OPUS),1)
OPUS_LIB_STATIC_LINE   = -l:libopus.a
OPUS_LIB_HYBRID_LINE   = -lopus
OPUS_LIB_DYNAMIC_LINE  = -lopus
else
OPUS_LIB_STATIC_LINE   =
OPUS_LIB_HYBRID_LINE   =
OPUS_LIB_DYNAMIC_LINE  =
endif

# Optional AAC support
# ---------------------
USE_AAC                ?= 1

ifeq ($(USE_AAC),1)
FAAC_LIB_STATIC_LINE   = -l:libfaac.a
FAAC_LIB_HYBRID_LINE   = -lfaac
FAAC_LIB_DYNAMIC_LINE  = -lfaac
AAC_LIB_STATIC_LINE    = -l:libhelix-aac.a
AAC_LIB_HYBRID_LINE    = -lhelix-aac
AAC_LIB_DYNAMIC_LINE   = -lhelix-aac
else
FAAC_LIB_STATIC_LINE   =
FAAC_LIB_HYBRID_LINE   =
FAAC_LIB_DYNAMIC_LINE  =
AAC_LIB_STATIC_LINE    =
AAC_LIB_HYBRID_LINE    =
AAC_LIB_DYNAMIC_LINE   =
endif

# Export codec feature flags to C/C++
override CFLAGS        += -DUSE_AAC=$(USE_AAC) -DUSE_OPUS=$(USE_OPUS) -DUSE_MP3=$(USE_MP3) -DUSE_FLAC=$(USE_FLAC)

# Optional crash backtrace support (requires libexecinfo)
# -------------------------------------------------------
USE_EXECINFO           ?= 0

ifeq ($(USE_EXECINFO),1)
override CFLAGS        += -DHAS_BACKTRACE=1
EXECINFO_LIB            = -lexecinfo
endif

ifeq ($(USE_WEBSOCKETS),1)
WEBSOCKET_LIB_STATIC_LINE = -l:libwebsockets.a
WEBSOCKET_LIB_HYBRID_LINE = -l:libwebsockets.so
WEBSOCKET_LIB_DYNAMIC_LINE = -lwebsockets
else
WEBSOCKET_LIB_STATIC_LINE =
WEBSOCKET_LIB_HYBRID_LINE =
WEBSOCKET_LIB_DYNAMIC_LINE =
endif

# Kernel Version Support
# ----------------------
ifeq ($(KERNEL_VERSION_4),y)
override CFLAGS        += -DKERNEL_VERSION_4
endif

# Binary Type Configuration
# -------------------------
# Default to dynamic linking unless explicitly specified
ifneq ($(filter -DBINARY_STATIC -DBINARY_HYBRID,$(CFLAGS)),)
# Static or hybrid build explicitly requested
else
override CFLAGS        += -DBINARY_DYNAMIC
endif

# Library Configuration
# =====================
# Check for libc type from CFLAGS, default to musl if not specified
# We add libmuslshim only when using musl (default if no libc type specified)

ifneq ($(MAKECMDGOALS),clean)

# Static Binary Configuration
# ---------------------------
ifneq (,$(findstring -DBINARY_STATIC,$(CFLAGS)))
override LDFLAGS       += -static -static-libgcc -static-libstdc++
LIBS                    = -Wl,--start-group \
                          -l:libimp.a \
                          -l:libalog.a \
                          -l:libsysutils.a \
                          -Wl,--end-group \
                          $(WEBSOCKET_LIB_STATIC_LINE) \
                          $(OSD_SCHRIFT_LIB_STATIC_LINE) \
                          $(OPUS_LIB_STATIC_LINE) \
                          $(FAAC_LIB_STATIC_LINE) \
                          $(AAC_LIB_STATIC_LINE) \
                          $(MP3_LIB_STATIC_LINE) \
                          $(FLAC_LIB_STATIC_LINE) \
                          -l:libcurl.a \
                          -ljct \
                          -latomic

ifneq (,$(findstring -DLIBC_GLIBC,$(CFLAGS)))
	# GLIBC - no additional libraries needed
else ifneq (,$(findstring -DLIBC_UCLIBC,$(CFLAGS)))
	# uClibc - no additional libraries needed
else
	# Default to musl - shim provides glibc compat symbols (__assert, pthread cancel hooks)
LIBS                   += -l:libmuslshim.a
endif

# Hybrid Binary Configuration
# ---------------------------
else ifneq (,$(findstring -DBINARY_HYBRID,$(CFLAGS)))
LIBS                    = -Wl,-Bdynamic \
                          -l:libimp.so \
                          -l:libalog.so \
                          -l:libsysutils.so \
                          -l:libaudioProcess.so \
                          $(WEBSOCKET_LIB_HYBRID_LINE) \
                          -Wl,-Bdynamic \
                          $(OSD_SCHRIFT_LIB_HYBRID_LINE) \
                          $(OPUS_LIB_HYBRID_LINE) \
                          $(FAAC_LIB_HYBRID_LINE) \
                          $(AAC_LIB_HYBRID_LINE) \
                          $(MP3_LIB_HYBRID_LINE) \
                          $(FLAC_LIB_HYBRID_LINE) \
                          -ljct \
                          -lcurl \
                          -latomic

ifneq (,$(findstring -DLIBC_GLIBC,$(CFLAGS)))
	# GLIBC - no additional libraries needed
else ifneq (,$(findstring -DLIBC_UCLIBC,$(CFLAGS)))
	# uClibc - no additional libraries needed
else
	# Default to musl
LIBS                   := $(LIBS:-Wl,-Bdynamic=-Wl,-Bdynamic -l:libmuslshim.so)
endif

# Dynamic Binary Configuration
# ----------------------------
else ifneq (,$(findstring -DBINARY_DYNAMIC,$(CFLAGS)))
override LDFLAGS       += -Wl,-Bdynamic
LIBS                    = -limp \
                          -lalog \
                          -laudioProcess \
                          -lsysutils \
                          $(WEBSOCKET_LIB_DYNAMIC_LINE) \
                          $(OSD_SCHRIFT_LIB_DYNAMIC_LINE) \
                          $(OPUS_LIB_DYNAMIC_LINE) \
                          $(FAAC_LIB_DYNAMIC_LINE) \
                          $(AAC_LIB_DYNAMIC_LINE) \
                          $(MP3_LIB_DYNAMIC_LINE) \
                          $(FLAC_LIB_DYNAMIC_LINE) \
                          -ljct \
                          -latomic \
                          -lcurl

ifneq (,$(findstring -DLIBC_GLIBC,$(CFLAGS)))
	# GLIBC - no additional libraries needed
else ifneq (,$(findstring -DLIBC_UCLIBC,$(CFLAGS)))
	# uClibc - no additional libraries needed
else
LIBS                   += -l:libmuslshim.so
endif

# Error Handling
# --------------
else
$(error No valid binary type defined in CFLAGS. Please specify -DBINARY_STATIC, -DBINARY_HYBRID, or -DBINARY_DYNAMIC)
endif

# Optional execinfo library for backtraces
LIBS                   += $(EXECINFO_LIB)

endif

# Platform-Specific Include Directories
# =====================================
# Prefer the globally selected SDK if provided, otherwise fall back to
# per-platform defaults to keep existing behavior for older setups.
LIBIMP_PLATFORM         :=
LIBIMP_LANG             :=
LIBIMP_DEFAULT_SDK_VERSION :=

ifneq (,$(findstring -DPLATFORM_C100,$(CFLAGS)))
    LIBIMP_PLATFORM        := C100
    LIBIMP_LANG            := en
    LIBIMP_DEFAULT_SDK_VERSION := 2.1.0
else ifneq (,$(or $(findstring -DPLATFORM_T20,$(CFLAGS)), $(findstring -DPLATFORM_T10,$(CFLAGS))))
    LIBIMP_PLATFORM        := T20
    LIBIMP_LANG            := zh
    LIBIMP_DEFAULT_SDK_VERSION := 3.12.0
else ifneq (,$(findstring -DPLATFORM_T21,$(CFLAGS)))
    LIBIMP_PLATFORM        := T21
    LIBIMP_LANG            := zh
    LIBIMP_DEFAULT_SDK_VERSION := 1.0.33
else ifneq (,$(findstring -DPLATFORM_T23,$(CFLAGS)))
    LIBIMP_PLATFORM        := T23
    LIBIMP_LANG            := zh
    LIBIMP_DEFAULT_SDK_VERSION := 1.3.0
else ifneq (,$(findstring -DPLATFORM_T30,$(CFLAGS)))
    LIBIMP_PLATFORM        := T30
    LIBIMP_LANG            := zh
    LIBIMP_DEFAULT_SDK_VERSION := 1.0.5
else ifneq (,$(findstring -DPLATFORM_T31,$(CFLAGS)))
    LIBIMP_PLATFORM        := T31
    LIBIMP_LANG            := en
    LIBIMP_DEFAULT_SDK_VERSION := 1.1.6
else ifneq (,$(findstring -DPLATFORM_T40,$(CFLAGS)))
    LIBIMP_PLATFORM        := T40
    LIBIMP_LANG            := zh
    LIBIMP_DEFAULT_SDK_VERSION := 1.2.0
else ifneq (,$(findstring -DPLATFORM_T41,$(CFLAGS)))
    LIBIMP_PLATFORM        := T41
    LIBIMP_LANG            := zh
    LIBIMP_DEFAULT_SDK_VERSION := 1.2.5
else
    LIBIMP_PLATFORM        := T31
    LIBIMP_LANG            := en
    LIBIMP_DEFAULT_SDK_VERSION := 1.1.6
endif

LIBIMP_SDK_VERSION      := $(strip $(SDK_VERSION))
ifeq ($(LIBIMP_SDK_VERSION),)
    LIBIMP_SDK_VERSION     := $(LIBIMP_DEFAULT_SDK_VERSION)
endif

LIBIMP_INC_DIR          = ./include/$(LIBIMP_PLATFORM)/$(LIBIMP_SDK_VERSION)/$(LIBIMP_LANG)

# Directory Structure
# ===================
SRC_DIR                 = ./src
OBJ_DIR                 = ./obj
BIN_DIR                 = ./bin

# Source subdirectories
SRC_SUBDIRS := core config stream video audio audio/playback audio/codec \
               isp network network/rtsp recording util

# Collect all .cpp sources from all subdirectories (except prudyntctl)
MAIN_SOURCES_CPP        := $(filter-out %/prudyntctl.cpp,$(foreach d,$(SRC_SUBDIRS),$(wildcard $(SRC_DIR)/$(d)/*.cpp)))
SOURCES_C               := $(foreach d,$(SRC_SUBDIRS),$(wildcard $(SRC_DIR)/$(d)/*.c))

SOURCES                 := $(MAIN_SOURCES_CPP) $(SOURCES_C)

# Object files mirror source tree under obj/
OBJECTS                 := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(MAIN_SOURCES_CPP)) \
                           $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SOURCES_C))

# prudyntctl is a separate binary
PRUDYNTCTL_SOURCE       := $(SRC_DIR)/prudyntctl.cpp
PRUDYNTCTL_OBJECTS      := $(OBJ_DIR)/prudyntctl.o

$(info Sources: $(SOURCES))
$(info Objects: $(OBJECTS))

# Target Configuration
# ====================
TARGET                  = $(BIN_DIR)/prudynt
PRUDYNTCTL_TARGET       = $(BIN_DIR)/prudyntctl

# Version Management
# ==================
ifndef commit_tag
# Always use timestamp for development builds to track local changes
current_timestamp       = $(shell date +%s)
git_hash                = $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")
commit_tag              = $(git_hash)-dev$(current_timestamp)
endif

VERSION_FILE            = $(LIBIMP_INC_DIR)/version.hpp
THIRDPARTY_INC_DIR      = ./3rdparty/install/include
CXXFLAGS_FILE           = $(OBJ_DIR)/.cxxflags

# Build Options
# =============
# Static binaries are used for development — keep symbols so crash
# addresses (e.g. /tmp/prudynt_crash.log) can be symbolized with addr2line.
ifneq (,$(findstring -DBINARY_STATIC,$(CFLAGS)))
STRIP_FLAG              :=
else
STRIP_FLAG              := $(if $(filter 0,$(DEBUG_STRIP)),,"-s")
endif

# =============================================================================
# Build Rules
# =============================================================================

# Version File Generation
# -----------------------
$(VERSION_FILE): $(SRC_DIR)/util/version.tpl.hpp FORCE
	@mkdir -p $(dir $(VERSION_FILE))
	@new_ver=$$(sed 's/COMMIT_TAG/"$(commit_tag)"/g' $(SRC_DIR)/util/version.tpl.hpp); \
	if [ ! -f $(VERSION_FILE) ] || ! echo "$$new_ver" | cmp -s $(VERSION_FILE) -; then \
		echo "Updating $(VERSION_FILE) to $(commit_tag)"; \
		echo "$$new_ver" > $(VERSION_FILE); \
	fi

# Compilation flags tracking - recompile all objects when CXXFLAGS changes
# -------------------------------------------------------------------------
$(CXXFLAGS_FILE): FORCE
	@mkdir -p $(@D)
	@[ -f "$@" ] && [ "$$(cat '$@')" = "$(CXXFLAGS)" ] || printf '%s' "$(CXXFLAGS)" > '$@'

# Include paths common to all compilations
INCLUDES := -I$(SRC_DIR) \
            -I$(LIBIMP_INC_DIR) \
            -I$(LIBIMP_INC_DIR)/imp \
            -I$(LIBIMP_INC_DIR)/sysutils \
            -isystem $(THIRDPARTY_INC_DIR)

# C++ Object Compilation (handles all subdirectories via pattern rule)
# ----------------------
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp $(VERSION_FILE) $(CXXFLAGS_FILE)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# C Object Compilation
# --------------------
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(VERSION_FILE) $(CXXFLAGS_FILE)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Final Binary Linking
# --------------------
$(TARGET): $(OBJECTS) $(VERSION_FILE)
	@mkdir -p $(@D)
	$(CCACHE) $(CXX) -o $@ $(OBJECTS) $(LDFLAGS) $(LIBS) $(STRIP_FLAG)

# prudyntctl compilation (separate binary, no subdirectory)
$(OBJ_DIR)/prudyntctl.o: $(SRC_DIR)/prudyntctl.cpp $(VERSION_FILE) $(CXXFLAGS_FILE)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(PRUDYNTCTL_TARGET): $(PRUDYNTCTL_OBJECTS)
	@mkdir -p $(@D)
	$(CCACHE) $(CXX) -o $@ $(PRUDYNTCTL_OBJECTS) $(LDFLAGS) $(STRIP_FLAG)

# =============================================================================
# Phony Targets
# =============================================================================

.PHONY: all clean distclean FORCE
FORCE:

# Default Target
# --------------
all: $(TARGET) $(PRUDYNTCTL_TARGET)

# Clean Build Artifacts
# ---------------------
clean:
	@echo "Cleaning build artifacts..."
	rm -rf $(OBJ_DIR)
	rm -f $(LIBIMP_INC_DIR)/version.hpp

# Complete Clean
# --------------
distclean: clean
	@echo "Cleaning all generated files..."
	rm -rf $(BIN_DIR)
