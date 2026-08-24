#ifndef H264_SPS_REWRITE_HPP
#define H264_SPS_REWRITE_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

// Rewrite an H.264 SPS NAL so its VUI declares full-range luma plus an
// explicit colour matrix. The Ingenic encoder feeds the stream near-full-range
// pixels but the SPS VUI either omits colour signalling or defaults to limited
// range (16-235), so players clip shadows/highlights. See
// https://github.com/themactep/thingino-firmware/issues/1547.
//
// Input is one SPS NAL unit without a start code: sps[0] is the NAL header,
// sps[1..] is the emulation-prevention-processed RBSP. The returned NAL has
// the same layout; the NAL header and level_idc bytes are preserved verbatim
// (the caller normalizes nal_ref_idc / level_idc separately).
//
// Returns a copy of the input unchanged if the SPS cannot be parsed (never
// returns an empty vector for a non-empty input), so a failure degrades to the
// current behaviour instead of corrupting the stream.
std::vector<uint8_t> h264RewriteSpsVui(const uint8_t *sps, size_t len);

#endif // H264_SPS_REWRITE_HPP
