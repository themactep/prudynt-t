#include "video/H264SpsRewrite.hpp"

#include "video/SpsVuiCommon.hpp"

namespace {

using namespace spsvui;

// Walk an H.264 SPS RBSP (NAL header byte already stripped) up to and including
// vui_parameters_present_flag. Returns false if the SPS is malformed or
// truncated. On success, *vuiPresentPos is the bit offset of
// vui_parameters_present_flag.
bool h264SpsToVui(const Bits &bits, size_t payloadEnd, size_t *vuiPresentPos) {
  BitReader br(bits);

  uint32_t profile = br.getBits(8);
  br.getBits(8); // constraint flags + reserved
  br.getBits(8); // level_idc
  br.getUE();    // seq_parameter_set_id

  if (profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
      profile == 44 || profile == 83 || profile == 86 || profile == 118 ||
      profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
      profile == 135) {
    uint32_t chroma = br.getUE();
    if (chroma == 3)
      br.getBit();     // separate_colour_plane_flag
    br.getUE();        // bit_depth_luma_minus8
    br.getUE();        // bit_depth_chroma_minus8
    br.getBit();       // qpprime_y_zero_transform_bypass_flag
    if (br.getBit()) { // seq_scaling_matrix_present_flag
      int count = (chroma != 3) ? 8 : 12;
      for (int i = 0; i < count; ++i) {
        if (br.getBit()) { // seq_scaling_list_present_flag
          int size = (i < 6) ? 16 : 64;
          int last = 8, next = 8;
          for (int j = 0; j < size; ++j) {
            if (next != 0) {
              int delta = br.getSE();
              next = (last + delta + 256) & 0xFF;
            }
            if (next != 0)
              last = next;
          }
        }
      }
    }
  }

  br.getUE(); // log2_max_frame_num_minus4
  uint32_t poc = br.getUE();
  if (poc == 0) {
    br.getUE(); // log2_max_pic_order_cnt_lsb_minus4
  } else if (poc == 1) {
    br.getBit(); // delta_pic_order_always_zero_flag
    br.getSE();  // offset_for_non_ref_pic
    br.getSE();  // offset_for_top_to_bottom_field
    uint32_t n = br.getUE();
    for (uint32_t i = 0; i < n; ++i)
      br.getSE(); // offset_for_ref_frame[i]
  }
  br.getUE();                     // max_num_ref_frames
  br.getBit();                    // gaps_in_frame_num_value_allowed_flag
  br.getUE(); // pic_width_in_mbs_minus1
  br.getUE(); // pic_height_in_map_units_minus1
  uint32_t fmo = br.getBit();     // frame_mbs_only_flag
  if (!fmo)
    br.getBit();     // mb_adaptive_frame_field_flag
  br.getBit();       // direct_8x8_inference_flag
  if (br.getBit()) { // frame_cropping_flag
    br.getUE();
    br.getUE();
    br.getUE();
    br.getUE();
  }

  if (br.pos > payloadEnd)
    return false; // truncated before the VUI

  *vuiPresentPos = br.pos;
  return true;
}

} // namespace

std::vector<uint8_t> h264RewriteSpsVui(const uint8_t *sps, size_t len) {
  if (!sps || len == 0)
    return {};
  std::vector<uint8_t> result(sps, sps + len); // unmodified on any failure
  if (len < 4 || (sps[0] & 0x1F) != 7)         // not an H.264 SPS NAL
    return result;

  std::vector<uint8_t> rbsp = spsvui::stripEpb(sps + 1, len - 1);
  if (rbsp.size() < 3)
    return result;
  Bits bits = spsvui::bytesToBits(rbsp.data(), rbsp.size());
  size_t payloadEnd = bits.size();
  while (payloadEnd > 0 && bits[payloadEnd - 1] == 0)
    --payloadEnd;
  if (payloadEnd > 0)
    --payloadEnd;
  if (payloadEnd < 24)
    return result;

  size_t vuiPresentPos = 0;
  if (!h264SpsToVui(bits, payloadEnd, &vuiPresentPos))
    return result;

  std::vector<uint8_t> out;
  out.reserve(1 + (rbsp.size() + 8));
  out.push_back(sps[0]); // preserve NAL header verbatim
  std::vector<uint8_t> ebsp =
      spsvui::applyVui(bits, vuiPresentPos, /*isHevc=*/false);
  out.insert(out.end(), ebsp.begin(), ebsp.end());
  return out;
}
