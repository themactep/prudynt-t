// MP4Muxer.hpp - Abstract interface for MP4/fMP4 muxer backends
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class MP4Muxer {
public:
  struct InitParams {
    int width = 0;
    int height = 0;
    int fps = 0;
    int sampleRate = 48000;
    int channels = 1;
    // avcC (AVCDecoderConfigurationRecord) and AAC AudioSpecificConfig
    std::vector<uint8_t> avcC;
    std::vector<uint8_t> aacConfig;
  };

  virtual bool init(const InitParams &params) = 0;

  // Return the init segment (ftyp+moov or equivalent) to send immediately to
  // clients
  virtual std::vector<uint8_t> getInitSegment() = 0;

  // Feed one video sample (raw NALs for H.264) and return any generated
  // fragment bytes pts is in milliseconds.
  virtual std::vector<uint8_t> muxVideo(const uint8_t *data, size_t size, int64_t pts_ms, bool isKey) = 0;

  // Feed one audio sample (raw AAC frame payload) and return any generated
  // fragment bytes
  virtual std::vector<uint8_t> muxAudio(const uint8_t *data, size_t size, int64_t pts_ms) = 0;

  virtual void close() = 0;

  virtual ~MP4Muxer() {
  }
};
