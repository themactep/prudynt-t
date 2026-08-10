// MP4MuxerFactory.hpp - factory for creating MP4Muxer instances
#pragma once

#include "recording/MP4Muxer.hpp"

// Create an MP4Muxer instance. Returns nullptr if no backend is available.
MP4Muxer *CreateMP4Muxer();

// Destroy an MP4Muxer instance created by CreateMP4Muxer
void DestroyMP4Muxer(MP4Muxer *m);
