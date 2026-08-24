#ifndef H265_SPS_REWRITE_HPP
#define H265_SPS_REWRITE_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

// Rewrite an H.265 SPS NAL so its VUI declares full-range luma plus an
// explicit colour matrix, mirroring h264RewriteSpsVui() for the HEVC case.
// See https://github.com/themactep/thingino-firmware/issues/1547.
//
// Input is one SPS NAL unit without a start code: sps[0..1] is the two-byte
// NAL header, sps[2..] is the emulation-prevention-processed RBSP. The
// returned NAL has the same layout and preserves the header verbatim.
//
// Returns a copy of the input unchanged if the SPS cannot be parsed.
std::vector<uint8_t> h265RewriteSpsVui(const uint8_t *sps, size_t len);

#endif // H265_SPS_REWRITE_HPP
