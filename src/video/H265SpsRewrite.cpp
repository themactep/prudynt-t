#include "video/H265SpsRewrite.hpp"

#include "video/SpsVuiCommon.hpp"

namespace {

using namespace spsvui;

// H.265 7.3.4: skip scaling_list_data(). Only the bit lengths matter here.
void skipScalingListData(BitReader &br) {
  for (int sizeId = 0; sizeId < 4; ++sizeId) {
    int numMatrices = (sizeId == 3) ? 2 : 6;
    for (int matrixId = 0; matrixId < numMatrices; ++matrixId) {
      if (!br.getBit()) { // scaling_list_pred_mode_flag
        br.getUE();       // scaling_list_pred_matrix_id_delta
        continue;
      }
      int coefNum = (sizeId == 0) ? 16 : 64;
      if (sizeId > 1)
        br.getSE(); // scaling_list_dc_coef_minus8
      for (int i = 0; i < coefNum; ++i)
        br.getSE(); // scaling_list_delta_coef
    }
  }
}

// H.265 7.3.7: skip one st_ref_pic_set(). Track NumDeltaPocs per set so a
// later predicted set can size its flag loop. In the SPS path the
// delta_idx_minus1 element is never present (stRpsIdx <
// num_short_term_ref_pic_sets), so RefRpsIdx is always stRpsIdx - 1.
void skipStRefPicSet(BitReader &br, uint32_t stRpsIdx,
                     std::vector<uint32_t> &numDeltaPocs) {
  int interPred = 0;
  if (stRpsIdx != 0)
    interPred = br.getBit();

  if (interPred) {
    br.getBit(); // delta_rps_sign
    br.getUE();  // abs_delta_rps_minus1
    uint32_t refCount = numDeltaPocs[stRpsIdx - 1];
    uint32_t keep = 0;
    for (uint32_t j = 0; j <= refCount; ++j) {
      if (br.getBit()) { // used_by_curr_pic_flag[j]
        ++keep;
      } else if (br.getBit()) { // use_delta_flag[j]
        ++keep;
      }
    }
    numDeltaPocs.push_back(keep);
  } else {
    uint32_t numNeg = br.getUE();
    uint32_t numPos = br.getUE();
    for (uint32_t i = 0; i < numNeg; ++i) {
      br.getUE();  // delta_poc_s0_minus1[i]
      br.getBit(); // used_by_curr_pic_s0_flag[i]
    }
    for (uint32_t i = 0; i < numPos; ++i) {
      br.getUE();  // delta_poc_s1_minus1[i]
      br.getBit(); // used_by_curr_pic_s1_flag[i]
    }
    numDeltaPocs.push_back(numNeg + numPos);
  }
}

// Walk an H.265 SPS RBSP (two-byte NAL header already stripped) up to and
// including vui_parameters_present_flag. Returns false if the SPS is
// malformed or truncated. On success, *vuiPresentPos is the bit offset of
// vui_parameters_present_flag and *width is pic_width_in_luma_samples.
bool h265SpsToVui(const Bits &bits, size_t payloadEnd, size_t *vuiPresentPos,
                  uint32_t *width) {
  BitReader br(bits);

  br.getBits(4);                         // sps_video_parameter_set_id
  uint32_t maxSubLayers = br.getBits(3); // sps_max_sub_layers_minus1
  br.getBit();                           // sps_temporal_id_nesting_flag

  // H.265 7.3.3: profile_tier_level(1, maxSubLayers).
  br.getBits(2);  // general_profile_space
  br.getBit();    // general_tier_flag
  br.getBits(5);  // general_profile_idc
  br.getBits(32); // general_profile_compatibility_flag[32]
  br.getBits(4);  // progressive/interlaced/non_packed/frame_only constraint
  br.getBits(44); // general_reserved_zero_44bits
  br.getBits(8);  // general_level_idc

  std::vector<uint32_t> profPresent(maxSubLayers), levelPresent(maxSubLayers);
  for (uint32_t i = 0; i < maxSubLayers; ++i) {
    profPresent[i] = br.getBit();  // sub_layer_profile_present_flag[i]
    levelPresent[i] = br.getBit(); // sub_layer_level_present_flag[i]
  }
  if (maxSubLayers > 0)
    for (uint32_t i = maxSubLayers; i < 8; ++i)
      br.getBits(2); // reserved_zero_2bits[i]
  for (uint32_t i = 0; i < maxSubLayers; ++i) {
    if (profPresent[i]) {
      br.getBits(2);  // sub_layer_profile_space
      br.getBit();    // sub_layer_tier_flag
      br.getBits(5);  // sub_layer_profile_idc
      br.getBits(32); // sub_layer_profile_compatibility_flag[32]
      br.getBits(4);  // sub_layer constraint flags
      br.getBits(44); // sub_layer_reserved_zero_44bits
    }
    if (levelPresent[i])
      br.getBits(8); // sub_layer_level_idc[i]
  }

  br.getUE(); // sps_seq_parameter_set_id
  uint32_t chroma = br.getUE();
  if (chroma == 3)
    br.getBit();           // separate_colour_plane_flag
  uint32_t w = br.getUE(); // pic_width_in_luma_samples
  br.getUE();              // pic_height_in_luma_samples
  if (br.getBit()) {       // conformance_window_flag
    br.getUE();
    br.getUE();
    br.getUE();
    br.getUE();
  }
  br.getUE();                          // bit_depth_luma_minus8
  br.getUE();                          // bit_depth_chroma_minus8
  uint32_t log2MaxPocLsb = br.getUE(); // log2_max_pic_order_cnt_lsb_minus4

  uint32_t subLayerOrdering = br.getBit();
  for (uint32_t i = subLayerOrdering ? 0 : maxSubLayers; i <= maxSubLayers;
       ++i) {
    br.getUE(); // sps_max_dec_pic_buffering_minus1[i]
    br.getUE(); // sps_max_num_reorder_pics[i]
    br.getUE(); // sps_max_latency_increase_plus1[i]
  }

  br.getUE(); // log2_min_luma_coding_block_size_minus3
  br.getUE(); // log2_diff_max_min_luma_coding_block_size
  br.getUE(); // log2_min_luma_transform_block_size_minus2
  br.getUE(); // log2_diff_max_min_luma_transform_block_size
  br.getUE(); // max_transform_hierarchy_depth_inter
  br.getUE(); // max_transform_hierarchy_depth_intra

  if (br.getBit()) { // scaling_list_enabled_flag
    if (br.getBit()) // sps_scaling_list_data_present_flag
      skipScalingListData(br);
  }

  br.getBit();       // amp_enabled_flag
  br.getBit();       // sample_adaptive_offset_enabled_flag
  if (br.getBit()) { // pcm_enabled_flag
    br.getBits(4);   // pcm_sample_bit_depth_luma_minus1
    br.getBits(4);   // pcm_sample_bit_depth_chroma_minus1
    br.getUE();      // log2_min_pcm_luma_coding_block_size_minus3
    br.getUE();      // log2_diff_max_min_pcm_luma_coding_block_size
    br.getBit();     // pcm_loop_filter_disabled_flag
  }

  uint32_t numStRps = br.getUE();
  std::vector<uint32_t> numDeltaPocs;
  numDeltaPocs.reserve(numStRps);
  for (uint32_t i = 0; i < numStRps; ++i)
    skipStRefPicSet(br, i, numDeltaPocs);

  if (br.getBit()) { // long_term_ref_pics_present_flag
    uint32_t numLt = br.getUE();
    for (uint32_t i = 0; i < numLt; ++i) {
      br.getBits(log2MaxPocLsb + 4); // lt_ref_pic_poc_lsb_sps[i]
      br.getBit();                   // used_by_curr_pic_lt_sps_flag[i]
    }
  }

  br.getBit(); // sps_temporal_mvp_enabled_flag
  br.getBit(); // strong_intra_smoothing_enabled_flag

  if (br.pos > payloadEnd)
    return false; // truncated before the VUI

  *vuiPresentPos = br.pos;
  *width = w;
  return true;
}

} // namespace

std::vector<uint8_t> h265RewriteSpsVui(const uint8_t *sps, size_t len) {
  if (!sps || len == 0)
    return {};
  std::vector<uint8_t> result(sps, sps + len); // unmodified on any failure
  if (len < 5 || ((sps[0] >> 1) & 0x3F) != 33) // not an H.265 SPS NAL
    return result;

  std::vector<uint8_t> rbsp = spsvui::stripEpb(sps + 2, len - 2);
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
  uint32_t width = 0;
  if (!h265SpsToVui(bits, payloadEnd, &vuiPresentPos, &width))
    return result;

  std::vector<uint8_t> out;
  out.reserve(2 + (rbsp.size() + 8));
  out.push_back(sps[0]); // preserve two-byte NAL header verbatim
  out.push_back(sps[1]);
  std::vector<uint8_t> ebsp =
      spsvui::applyVui(bits, vuiPresentPos, width >= 1280, /*isHevc=*/true);
  out.insert(out.end(), ebsp.begin(), ebsp.end());
  return out;
}
