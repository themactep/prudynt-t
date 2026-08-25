// Host test for the H.265 SPS VUI rewrite (issue #1547).
//
// Build and run:
//   g++ -std=c++17 -Wall -Wextra -Isrc
//       tests/test_h265_sps_vui.cpp src/video/H265SpsRewrite.cpp
//       -o /tmp/test_h265_sps_vui && /tmp/test_h265_sps_vui
//
// The real-encoder vectors below were captured from x265 (ffmpeg) and their
// rewritten output was verified with ffprobe: color_range tv -> pc and
// color_space gbr -> bt709. The synthetic minimal-SPS cases exercise the
// "no VUI" and "no video-signal block" insertion paths, which a real encoder
// may or may not produce but the rewriter must still handle.

#include "video/H265SpsRewrite.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

struct BitWriter {
  std::vector<uint8_t> bits;
  void bit(int b) {
    bits.push_back(b & 1);
  }
  void bits_(uint32_t v, int n) {
    for (int i = n - 1; i >= 0; --i)
      bits.push_back((v >> i) & 1);
  }
  void ue(uint32_t v) {
    uint32_t codeNum = v + 1;
    int len = 0;
    for (uint32_t t = codeNum; t > 1; t >>= 1)
      ++len;
    for (int i = 0; i < len; ++i)
      bit(0);
    bits_(codeNum, len + 1);
  }
  void se(int v) {
    uint32_t u = (v > 0) ? static_cast<uint32_t>(2 * v - 1)
                         : static_cast<uint32_t>(-2 * v);
    ue(u);
  }
  void rbspTrailing() {
    bit(1);
    while (bits.size() % 8 != 0)
      bit(0);
  }
  std::vector<uint8_t> toBytes() const {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < bits.size(); i += 8) {
      uint8_t b = 0;
      for (int k = 0; k < 8; ++k) {
        b = static_cast<uint8_t>(b << 1);
        if (i + k < bits.size())
          b = static_cast<uint8_t>(b | bits[i + k]);
      }
      out.push_back(b);
    }
    return out;
  }
};

std::vector<uint8_t> wrapNal(const std::vector<uint8_t> &rbsp,
                             const uint8_t header[2]) {
  std::vector<uint8_t> epb;
  int zeros = 0;
  for (uint8_t b : rbsp) {
    if (zeros >= 2 && b <= 0x03) {
      epb.push_back(0x03);
      zeros = 0;
    }
    epb.push_back(b);
    if (b == 0)
      ++zeros;
    else
      zeros = 0;
  }
  std::vector<uint8_t> nal;
  nal.push_back(header[0]);
  nal.push_back(header[1]);
  nal.insert(nal.end(), epb.begin(), epb.end());
  return nal;
}

struct Reader {
  const std::vector<uint8_t> &bits;
  size_t pos = 0;
  explicit Reader(const std::vector<uint8_t> &b) : bits(b) {
  }
  int bit() {
    return (pos < bits.size()) ? bits[pos++] : 0;
  }
  uint32_t bits_(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i)
      v = (v << 1) | bit();
    return v;
  }
  uint32_t ue() {
    int z = 0;
    while (bit() == 0 && z < 32)
      ++z;
    if (z == 0)
      return 0;
    uint32_t low = 0;
    for (int i = 0; i < z; ++i)
      low = (low << 1) | bit();
    return (1u << z) - 1 + low;
  }
};

// Parse the VUI colour fields out of the MINIMAL SPS shape built by
// buildMinimalSps() below (max_sub_layers=0, no scaling lists, one empty
// short-term ref pic set, no long-term refs). Used only to verify the
// synthetic vectors; the real x265 vectors are checked byte-for-byte.
struct ParsedVui {
  bool ok = false;
  int fullRange = -1;
  int colourDesc = -1;
  int primaries = -1;
  int transfer = -1;
  int matrix = -1;
};

ParsedVui parseMinimalVui(const std::vector<uint8_t> &nal) {
  ParsedVui out;
  if (nal.size() < 4 || ((nal[0] >> 1) & 0x3F) != 33)
    return out;

  std::vector<uint8_t> rbsp;
  for (size_t i = 2; i < nal.size(); ++i) {
    if (i + 2 < nal.size() && nal[i] == 0 && nal[i + 1] == 0 &&
        nal[i + 2] == 3) {
      rbsp.push_back(0);
      rbsp.push_back(0);
      i += 2;
      continue;
    }
    rbsp.push_back(nal[i]);
  }

  std::vector<uint8_t> bits;
  for (uint8_t b : rbsp)
    for (int k = 7; k >= 0; --k)
      bits.push_back((b >> k) & 1);

  Reader r(bits);
  r.bits_(4); // sps_video_parameter_set_id
  uint32_t maxSubLayers = r.bits_(3);
  r.bit(); // sps_temporal_id_nesting_flag
  // profile_tier_level(1, maxSubLayers)
  r.bits_(2);
  r.bit();
  r.bits_(5);
  r.bits_(32);
  r.bits_(4);
  r.bits_(44);
  r.bits_(8); // general_level_idc
  for (uint32_t i = 0; i < maxSubLayers; ++i) {
    r.bit();
    r.bit();
  }
  if (maxSubLayers > 0)
    for (uint32_t i = maxSubLayers; i < 8; ++i)
      r.bits_(2);
  for (uint32_t i = 0; i < maxSubLayers; ++i) {
    // minimal shape never sets these, so skip nothing here
  }
  r.ue(); // sps_seq_parameter_set_id
  uint32_t chroma = r.ue();
  if (chroma == 3)
    r.bit();
  r.ue();        // width
  r.ue();        // height
  if (r.bit()) { // conformance_window_flag
    r.ue();
    r.ue();
    r.ue();
    r.ue();
  }
  r.ue(); // bit_depth_luma_minus8
  r.ue(); // bit_depth_chroma_minus8
  r.ue(); // log2_max_pic_order_cnt_lsb_minus4
  uint32_t subLayerOrdering = r.bit();
  for (uint32_t i = subLayerOrdering ? 0 : maxSubLayers; i <= maxSubLayers;
       ++i) {
    r.ue();
    r.ue();
    r.ue();
  }
  r.ue();         // log2_min_luma_coding_block_size_minus3
  r.ue();         // log2_diff_max_min_luma_coding_block_size
  r.ue();         // log2_min_luma_transform_block_size_minus2
  r.ue();         // log2_diff_max_min_luma_transform_block_size
  r.ue();         // max_transform_hierarchy_depth_inter
  r.ue();         // max_transform_hierarchy_depth_intra
  if (r.bit()) {  // scaling_list_enabled_flag
    if (r.bit())  // sps_scaling_list_data_present_flag
      return out; // minimal shape never uses scaling lists
  }
  r.bit();      // amp_enabled_flag
  r.bit();      // sample_adaptive_offset_enabled_flag
  if (r.bit())  // pcm_enabled_flag
    return out; // minimal shape never uses PCM
  uint32_t numStRps = r.ue();
  for (uint32_t i = 0; i < numStRps; ++i) {
    if (i != 0 && r.bit()) // inter_ref_pic_set_prediction_flag
      return out;          // minimal shape uses the non-predicted path
    uint32_t numNeg = r.ue();
    uint32_t numPos = r.ue();
    for (uint32_t j = 0; j < numNeg; ++j) {
      r.ue();
      r.bit();
    }
    for (uint32_t j = 0; j < numPos; ++j) {
      r.ue();
      r.bit();
    }
  }
  if (r.bit())  // long_term_ref_pics_present_flag
    return out; // minimal shape has none
  r.bit();      // sps_temporal_mvp_enabled_flag
  r.bit();      // strong_intra_smoothing_enabled_flag

  if (!r.bit()) { // vui_parameters_present_flag
    out.ok = true;
    return out; // no VUI
  }
  if (r.bit()) { // aspect_ratio_info_present_flag
    uint32_t idc = r.bits_(8);
    if (idc == 255) {
      r.bits_(16);
      r.bits_(16);
    }
  }
  if (r.bit())
    r.bit();     // overscan_appropriate_flag
  if (r.bit()) { // video_signal_type_present_flag
    r.bits_(3);  // video_format
    out.fullRange = r.bit();
    out.colourDesc = r.bit();
    if (out.colourDesc) {
      out.primaries = static_cast<int>(r.bits_(8));
      out.transfer = static_cast<int>(r.bits_(8));
      out.matrix = static_cast<int>(r.bits_(8));
    }
  }
  out.ok = true;
  return out;
}

// Build a minimal HEVC SPS NAL (2-byte header 0x42 0x01 = SPS, type 33).
// vuiKind: 0 = no VUI, 1 = VUI without a video-signal block,
//          2 = VUI with a full limited-range signal block.
std::vector<uint8_t> buildMinimalSps(uint32_t width, uint32_t height,
                                     int vuiKind) {
  BitWriter w;
  w.bits_(0, 4); // sps_video_parameter_set_id
  w.bits_(0, 3); // sps_max_sub_layers_minus1
  w.bit(1);      // sps_temporal_id_nesting_flag
  // profile_tier_level(1, 0): Main profile, level 4.0
  w.bits_(0, 2);   // general_profile_space
  w.bit(0);        // general_tier_flag
  w.bits_(1, 5);   // general_profile_idc = Main
  w.bits_(0, 32);  // compatibility flags
  w.bits_(0x1, 4); // frame_only constraint flag
  w.bits_(0, 44);  // reserved
  w.bits_(120, 8); // general_level_idc = 4.0
  w.ue(0);         // sps_seq_parameter_set_id
  w.ue(1);         // chroma_format_idc = 4:2:0
  w.ue(width);     // pic_width_in_luma_samples
  w.ue(height);    // pic_height_in_luma_samples
  w.bit(0);        // conformance_window_flag
  w.ue(0);         // bit_depth_luma_minus8
  w.ue(0);         // bit_depth_chroma_minus8
  w.ue(0);         // log2_max_pic_order_cnt_lsb_minus4
  w.bit(1);        // sps_sub_layer_ordering_info_present_flag
  w.ue(3);         // sps_max_dec_pic_buffering_minus1[0]
  w.ue(2);         // sps_max_num_reorder_pics[0]
  w.ue(0);         // sps_max_latency_increase_plus1[0]
  w.ue(0);         // log2_min_luma_coding_block_size_minus3
  w.ue(3);         // log2_diff_max_min_luma_coding_block_size
  w.ue(0);         // log2_min_luma_transform_block_size_minus2
  w.ue(3);         // log2_diff_max_min_luma_transform_block_size
  w.ue(1);         // max_transform_hierarchy_depth_inter
  w.ue(1);         // max_transform_hierarchy_depth_intra
  w.bit(0);        // scaling_list_enabled_flag
  w.bit(0);        // amp_enabled_flag
  w.bit(1);        // sample_adaptive_offset_enabled_flag
  w.bit(0);        // pcm_enabled_flag
  w.ue(1);         // num_short_term_ref_pic_sets
  w.ue(0);         // st_ref_pic_set(0): num_negative_pics
  w.ue(0);         // num_positive_pics
  w.bit(0);        // long_term_ref_pics_present_flag
  w.bit(0);        // sps_temporal_mvp_enabled_flag
  w.bit(0);        // strong_intra_smoothing_enabled_flag

  w.bit(vuiKind != 0); // vui_parameters_present_flag
  if (vuiKind != 0) {
    w.bit(0);            // aspect_ratio_info_present_flag
    w.bit(0);            // overscan_info_present_flag
    w.bit(vuiKind == 2); // video_signal_type_present_flag
    if (vuiKind == 2) {
      w.bits_(5, 3); // video_format = unspecified
      w.bit(0);      // video_full_range_flag = limited
      w.bit(1);      // colour_description_present_flag
      w.bits_(5, 8); // colour_primaries = bt470bg
      w.bits_(5, 8); // transfer_characteristics = bt470bg
      w.bits_(5, 8); // matrix_coefficients = bt470bg
    }
  }
  w.bit(0); // sps_extension_present_flag
  w.rbspTrailing();

  const uint8_t header[2] = {0x42, 0x01};
  return wrapNal(w.toBytes(), header);
}

} // namespace

int main() {
  // Real x265 SPS (720p), limited range, matrix=gbr. Rewrite must flip the
  // full-range flag and set primaries/transfer/matrix to BT.709. Expected
  // bytes verified with ffprobe (color_range=pc, color_space=bt709).
  {
    const std::vector<uint8_t> limited = {
        0x42, 0x01, 0x01, 0x04, 0x08, 0x00, 0x00, 0x03, 0x00, 0x9e, 0x08,
        0x00, 0x00, 0x03, 0x00, 0x00, 0x5d, 0x90, 0x00, 0x50, 0x10, 0x05,
        0xa2, 0xcb, 0x2b, 0x34, 0x92, 0x65, 0x78, 0x0b, 0x50, 0x20, 0x20,
        0x00, 0x40, 0x00, 0x00, 0x03, 0x00, 0x40, 0x00, 0x00, 0x06, 0x42};
    const std::vector<uint8_t> expected = {
        0x42, 0x01, 0x01, 0x04, 0x08, 0x00, 0x00, 0x03, 0x00, 0x9e, 0x08,
        0x00, 0x00, 0x03, 0x00, 0x00, 0x5d, 0x90, 0x00, 0x50, 0x10, 0x05,
        0xa2, 0xcb, 0x2b, 0x34, 0x92, 0x65, 0x78, 0x0b, 0x70, 0x10, 0x10,
        0x10, 0x40, 0x00, 0x00, 0x03, 0x00, 0x40, 0x00, 0x00, 0x06, 0x42};
    std::vector<uint8_t> out =
        h265RewriteSpsVui(limited.data(), limited.size());
    CHECK(out == expected);

    // Idempotent: rewriting the result changes nothing.
    std::vector<uint8_t> again = h265RewriteSpsVui(out.data(), out.size());
    CHECK(again == out);
  }

  // Synthetic minimal SPS, no VUI (SD 720x576 -> BT.709).
  {
    std::vector<uint8_t> nal = buildMinimalSps(720, 576, 0);
    std::vector<uint8_t> out = h265RewriteSpsVui(nal.data(), nal.size());
    ParsedVui v = parseMinimalVui(out);
    CHECK(v.ok);
    CHECK(v.fullRange == 1);
    CHECK(v.colourDesc == 1);
    CHECK(v.matrix == 1); // BT.709, same as HD
    CHECK(v.primaries == 1);
    CHECK(v.transfer == 1);
  }

  // Synthetic minimal SPS, VUI without a video-signal block (HD 1280x720).
  {
    std::vector<uint8_t> nal = buildMinimalSps(1280, 720, 1);
    std::vector<uint8_t> out = h265RewriteSpsVui(nal.data(), nal.size());
    ParsedVui v = parseMinimalVui(out);
    CHECK(v.ok);
    CHECK(v.fullRange == 1);
    CHECK(v.colourDesc == 1);
    CHECK(v.matrix == 1); // BT.709
    CHECK(v.primaries == 1);
    CHECK(v.transfer == 1);
  }

  // Synthetic minimal SPS, full limited-range signal block (HD).
  {
    std::vector<uint8_t> nal = buildMinimalSps(1280, 720, 2);
    std::vector<uint8_t> out = h265RewriteSpsVui(nal.data(), nal.size());
    ParsedVui v = parseMinimalVui(out);
    CHECK(v.ok);
    CHECK(v.fullRange == 1);
    CHECK(v.colourDesc == 1);
    CHECK(v.matrix == 1);
    CHECK(v.primaries == 1);
    CHECK(v.transfer == 1);
  }

  // Non-SPS NAL (PPS) must be returned byte-for-byte unchanged.
  {
    std::vector<uint8_t> pps = {0x44, 0x01, 0xc1, 0x73, 0xd8, 0x90};
    std::vector<uint8_t> out = h265RewriteSpsVui(pps.data(), pps.size());
    CHECK(out == pps);
  }

  // Garbage/truncated input must not crash and must return the input.
  {
    std::vector<uint8_t> junk = {0x42, 0x01, 0x01, 0x04};
    std::vector<uint8_t> out = h265RewriteSpsVui(junk.data(), junk.size());
    CHECK(out == junk);
    CHECK(h265RewriteSpsVui(nullptr, 0).empty());
  }

  if (failures == 0) {
    std::printf("PASS: all H.265 SPS VUI rewrite checks passed\n");
    return 0;
  }
  std::printf("%d check(s) FAILED\n", failures);
  return 1;
}
