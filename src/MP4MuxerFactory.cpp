// MP4MuxerFactory.cpp - factory that returns a custom MP4 writer backend.
#include "MP4MuxerFactory.hpp"
#include "MP4Muxer.hpp"
#include <stdio.h>

// Custom MP4 writer backend.
extern "C" MP4Muxer* CreateSimpleMP4Muxer();
extern "C" void DestroySimpleMP4Muxer(MP4Muxer* m);

MP4Muxer* CreateMP4Muxer() {
    fprintf(stderr, "MP4Muxer: using SimpleMP4Muxer backend (custom MP4 writer)\n");
    return CreateSimpleMP4Muxer();
}

void DestroyMP4Muxer(MP4Muxer* m) {
    DestroySimpleMP4Muxer(m);
}
