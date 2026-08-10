#pragma once

#include "network/rtsp/RtspTypes.hpp"
#include <string>

namespace simple_rtsp {

// Generate SDP for a video+audio+subtitle session.
// Returns the SDP text, or empty string on failure.
std::string generateSdp(const VideoStreamConfig &video,
                        const AudioStreamConfig *audio,  // nullptr if no audio
                        const char *serverIp,
                        const char *streamName,
                        const std::vector<BackchannelConfig> *backchannel = nullptr,
                        const SubtitleStreamConfig *subtitle = nullptr);

// Generate SDP for an audio-only session (no video track).
std::string generateAudioOnlySdp(const AudioStreamConfig &audio,
                                 const char *serverIp,
                                 const char *streamName);

// Generate SDP describing available backchannel (receive) codecs.
std::string generateBackchannelSdp(const std::vector<BackchannelConfig> &formats,
                                   const char *serverIp,
                                   const char *streamName);

} // namespace simple_rtsp
