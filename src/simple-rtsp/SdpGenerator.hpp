#pragma once

#include "RtspTypes.hpp"
#include <string>

namespace simple_rtsp {

// Generate SDP for a video+audio session.
// Returns the SDP text, or empty string on failure.
std::string generateSdp(const VideoStreamConfig &video,
                        const AudioStreamConfig *audio,  // nullptr if no audio
                        const char *serverIp,
                        const char *streamName,
                        const std::vector<BackchannelConfig> *backchannel = nullptr);

// Generate SDP for an audio-only session (no video track).
std::string generateAudioOnlySdp(const AudioStreamConfig &audio,
                                 const char *serverIp,
                                 const char *streamName);

} // namespace simple_rtsp
