# Makefile for daynightd
# Moved to src_daynightd/

CC := $(CROSS_COMPILE)gcc
STRIP := $(CROSS_COMPILE)strip
# Use += to allow appending from command line if needed, but define base here
BASE_CFLAGS := -Wall -O2
BASE_LIBS := -lpthread -ldl

# Feature flags (override from command line or compile.sh)
ENABLE_SQLITE ?= 0
ENABLE_MQTT ?= 0
ENABLE_GRAPHITE ?= 0

CFLAGS_FEATURES :=
LIBS_FEATURES :=

ifeq ($(ENABLE_SQLITE),1)
    CFLAGS_FEATURES += -DENABLE_SQLITE
    LIBS_FEATURES += -lsqlite3
endif

ifeq ($(ENABLE_MQTT),1)
    CFLAGS_FEATURES += -DENABLE_MQTT
    LIBS_FEATURES += -lmosquitto
endif

ifeq ($(ENABLE_GRAPHITE),1)
    CFLAGS_FEATURES += -DENABLE_GRAPHITE
endif

# Combine everything
FULL_CFLAGS := $(BASE_CFLAGS) $(CFLAGS_FEATURES) $(CFLAGS)
FULL_LIBS := $(BASE_LIBS) $(LIBS_FEATURES) $(LIBS)

TARGET := ./daynightd
SRC := daynightd.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(FULL_CFLAGS) $(LDFLAGS) $< -o $@ $(FULL_LIBS)
	$(STRIP) $@

clean:
	rm -f $(TARGET)

.PHONY: all clean
