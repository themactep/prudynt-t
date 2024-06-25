#!/bin/bash

DEST_DIR=$HOME/nfs/
CAMERA=cinnado_d1_t31l
SOC=T31

HOST=$HOME/output/$CAMERA/per-package/prudynt-t/host
INC=$HOST/mipsel-buildroot-linux-musl/sysroot/usr/include

make clean

/usr/bin/make \
    ARCH=mips \
    CROSS_COMPILE=$HOST/bin/mipsel-linux- \
    CFLAGS="-DPLATFORM_$SOC -Os -DALLOW_RTSP_SERVER_PORT_REUSE=1 -DNO_OPENSSL=1 -I$INC -I$INC/freetype2 -I$INC/liveMedia -I$INC/groupsock -I$INC/UsageEnvironment -I$INC/BasicUsageEnvironment" \
    LDFLAGS=" -L$HOST/mipsel-buildroot-linux-musl/sysroot/usr/lib -L$HOME/output/$CAMERA/per-package/prudynt-t/target/usr/lib" \
    -C $PWD all

[ -n $DEST_DIR ] && cp -vf bin/prudynt $DEST_DIR
