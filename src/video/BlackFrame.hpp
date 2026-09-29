#ifndef THINGINO_BLACKFRAME_HPP
#define THINGINO_BLACKFRAME_HPP

#include <cstdint>
#include <vector>

namespace blackframe {

// Build a self-contained, decodable privacy-cover keyframe (Annex B) from the
// stream's live SPS/PPS: SPS + PPS + CAVLC twin PPS + a uniform Intra16x16
// IDR (DC prediction, zero residual) that decodes to a flat mid-grey frame.
//
// The encoder on the open tx-isp stack cannot be handed a synthetic frame, and
// the driver has no OSD, so the cover cannot be drawn in hardware. This
// synthesises the frame instead; the streamer withholds the real scene while
// privacy is active, so nothing of the scene is transmitted.
//
// Returns an empty vector if the parameter sets are unusable (non-H.264,
// FMO, unsupported profile).
std::vector<uint8_t> buildBlackAccessUnit(const std::vector<uint8_t> &sps,
                                          const std::vector<uint8_t> &pps);

// Build the CAVLC twin of the stream's PPS (same contents, entropy_coding_mode
// forced to CAVLC, pic_parameter_set_id incremented). The black access unit
// references this PPS; fMP4 muxers must carry it in the avcC so decoders have
// it. Returns empty if the PPS cannot be parsed.
std::vector<uint8_t> buildBlackPps(const std::vector<uint8_t> &pps);

} // namespace blackframe

#endif
