#pragma once

#include <cstdint>
#include <vector>

// Build an HEVCDecoderConfigurationRecord (hvcC) from start-code-free VPS, SPS
// and PPS NAL units (the 2-byte HEVC NAL header included). The SPS RBSP must
// carry the profile/level fields; when it is too short, safe defaults are used.
// Returns false when the SPS or PPS are missing.
bool buildHvcC(const std::vector<uint8_t> &vps,
               const std::vector<uint8_t> &sps,
               const std::vector<uint8_t> &pps, std::vector<uint8_t> &hvcC);
