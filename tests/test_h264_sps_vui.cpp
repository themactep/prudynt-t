// Host test for the H.264 SPS VUI rewrite (issue #1547).
//
// Build and run:
//   g++ -std=c++17 -Wall -Wextra -Isrc
//       tests/test_h264_sps_vui.cpp src/video/H264SpsRewrite.cpp
//       -o /tmp/test_h264_sps_vui && /tmp/test_h264_sps_vui
//
// Builds synthetic H.264 SPS NALs covering the four VUI shapes the Ingenic
// encoder can emit, rewrites them, and re-parses the result to assert the
// full-range flag and colour matrix landed without disturbing anything else.

#include "video/H264SpsRewrite.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

// Emulation-prevention wrap + NAL header, mirroring the encoder's EBSP form.
std::vector<uint8_t> wrapNal(const std::vector<uint8_t> &rbsp, uint8_t header) {
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
  nal.push_back(header);
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

struct ParsedVui {
  bool ok = false;
  int fullRange = -1;
  int colourDesc = -1;
  int primaries = -1;
  int transfer = -1;
  int matrix = -1;
  int videoFormat = -1;
  bool timingPresent = false;
  uint32_t numUnitsInTick = 0;
  uint32_t timeScale = 0;
  int fixedFrameRate = -1;
};

ParsedVui parseSpsVui(const std::vector<uint8_t> &nal) {
  ParsedVui out;
  if (nal.empty() || (nal[0] & 0x1F) != 7)
    return out;

  std::vector<uint8_t> rbsp;
  for (size_t i = 1; i < nal.size(); ++i) {
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
  uint32_t profile = r.bits_(8);
  r.bits_(8); // constraint flags
  uint32_t level = r.bits_(8);
  r.ue(); // sps_id

  // Baseline only in these tests; reject a profile that would need the
  // high-profile scaling-matrix walk.
  if (profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
      profile == 44 || profile == 83 || profile == 86 || profile == 118 ||
      profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
      profile == 135)
    return out;

  r.ue(); // log2_max_frame_num_minus4
  uint32_t poc = r.ue();
  if (poc == 0)
    r.ue();
  else
    return out; // tests use poc_type 0 only
  r.ue();       // max_num_ref_frames
  r.bit();      // gaps
  r.ue();       // width
  r.ue();       // height
  uint32_t fmo = r.bit();
  if (!fmo)
    r.bit();
  r.bit();       // direct_8x8
  if (r.bit()) { // frame_cropping
    r.ue();
    r.ue();
    r.ue();
    r.ue();
  }

  out.primaries = -1; // keep; set below if present
  int vuiPresent = r.bit();
  if (!vuiPresent) {
    out.ok = true;
    out.fullRange = -1; // no VUI -> flag absent
    return out;
  }

  if (r.bit()) { // aspect_ratio_info_present_flag
    uint32_t idc = r.bits_(8);
    if (idc == 255) {
      r.bits_(16);
      r.bits_(16);
    }
  }
  if (r.bit())
    r.bit(); // overscan_appropriate_flag
  int signal = r.bit();
  if (signal) {
    out.videoFormat = static_cast<int>(r.bits_(3));
    out.fullRange = r.bit();
    out.colourDesc = r.bit();
    if (out.colourDesc) {
      out.primaries = static_cast<int>(r.bits_(8));
      out.transfer = static_cast<int>(r.bits_(8));
      out.matrix = static_cast<int>(r.bits_(8));
    }
  } else {
    out.fullRange = -1;
    out.colourDesc = 0;
  }
  if (r.bit()) { // chroma_loc_info_present_flag
    r.ue();
    r.ue();
  }
  out.timingPresent = r.bit() != 0;
  if (out.timingPresent) {
    out.numUnitsInTick = r.bits_(32);
    out.timeScale = r.bits_(32);
    out.fixedFrameRate = r.bit();
  }
  out.ok = true;
  (void)level;
  return out;
}

// Baseline SPS header up to (but not including) vui_parameters_present_flag.
void writeBaselineHeader(BitWriter &w, uint32_t levelIdc, uint32_t widthMbs,
                         uint32_t heightMbs) {
  w.bits_(66, 8);       // profile_idc = Baseline
  w.bits_(0x00, 8);     // constraint flags + reserved
  w.bits_(levelIdc, 8); // level_idc
  w.ue(0);              // seq_parameter_set_id
  w.ue(0);              // log2_max_frame_num_minus4
  w.ue(0);              // pic_order_cnt_type = 0
  w.ue(0);              // log2_max_pic_order_cnt_lsb_minus4
  w.ue(1);              // max_num_ref_frames
  w.bit(0);             // gaps_in_frame_num_value_allowed_flag
  w.ue(widthMbs - 1);   // pic_width_in_mbs_minus1
  w.ue(heightMbs - 1);  // pic_height_in_map_units_minus1
  w.bit(1);             // frame_mbs_only_flag
  w.bit(1);             // direct_8x8_inference_flag
  w.bit(0);             // frame_cropping_flag
}

std::vector<uint8_t> buildNal(const std::vector<uint8_t> &rbsp) {
  return wrapNal(rbsp, 0x67); // SPS, nal_ref_idc=3
}

} // namespace

int main() {
  const uint8_t kHeader = 0x67;
  const uint32_t kLevelIdc = 0x29; // 4.1

  // Case 1: no VUI at all (SD 720x576 -> 45x36 MBs).
  {
    BitWriter w;
    writeBaselineHeader(w, kLevelIdc, 45, 36);
    w.bit(0); // vui_parameters_present_flag = 0
    w.rbspTrailing();
    std::vector<uint8_t> nal = buildNal(w.toBytes());
    std::vector<uint8_t> out = h264RewriteSpsVui(nal.data(), nal.size());
    CHECK(!out.empty());
    CHECK(out[0] == kHeader);
    CHECK(out[3] == kLevelIdc); // level_idc preserved
    ParsedVui v = parseSpsVui(out);
    CHECK(v.ok);
    CHECK(v.fullRange == 1);
    CHECK(v.colourDesc == 1);
    CHECK(v.matrix == 1); // BT.709 for every stream, regardless of size
    CHECK(v.primaries == 1);
    CHECK(v.transfer == 1);
  }

  // Case 2: VUI present but no video-signal block (HD 1280x720 -> 80x45 MBs).
  {
    BitWriter w;
    writeBaselineHeader(w, kLevelIdc, 80, 45);
    w.bit(1);        // vui_parameters_present_flag
    w.bit(0);        // aspect_ratio_info_present_flag
    w.bit(0);        // overscan_info_present_flag
    w.bit(0);        // video_signal_type_present_flag
    w.bit(0);        // chroma_loc_info_present_flag
    w.bit(1);        // timing_info_present_flag
    w.bits_(1, 32);  // num_units_in_tick
    w.bits_(30, 32); // time_scale
    w.bit(1);        // fixed_frame_rate_flag
    w.bit(0);        // nal_hrd_parameters_present_flag
    w.bit(0);        // vcl_hrd_parameters_present_flag
    w.bit(0);        // pic_struct_present_flag
    w.bit(0);        // bitstream_restriction_flag
    w.rbspTrailing();
    std::vector<uint8_t> nal = buildNal(w.toBytes());
    std::vector<uint8_t> out = h264RewriteSpsVui(nal.data(), nal.size());
    ParsedVui v = parseSpsVui(out);
    CHECK(v.ok);
    CHECK(v.fullRange == 1);
    CHECK(v.colourDesc == 1);
    CHECK(v.matrix == 1); // BT.709
    CHECK(v.primaries == 1);
    CHECK(v.transfer == 1);
    CHECK(v.timingPresent); // tail preserved
    CHECK(v.numUnitsInTick == 1);
    CHECK(v.timeScale == 30);
    CHECK(v.fixedFrameRate == 1);
  }

  // Case 3: video signal present, colour_description absent (SD).
  {
    BitWriter w;
    writeBaselineHeader(w, kLevelIdc, 45, 36);
    w.bit(1);      // vui_parameters_present_flag
    w.bit(0);      // aspect_ratio_info_present_flag
    w.bit(0);      // overscan_info_present_flag
    w.bit(1);      // video_signal_type_present_flag
    w.bits_(5, 3); // video_format = unspecified
    w.bit(0);      // video_full_range_flag = limited
    w.bit(0);      // colour_description_present_flag
    w.bit(0);      // chroma_loc_info_present_flag
    w.bit(0);      // timing_info_present_flag
    w.bit(0);      // nal_hrd
    w.bit(0);      // vcl_hrd
    w.bit(0);      // pic_struct
    w.bit(0);      // bitstream_restriction
    w.rbspTrailing();
    std::vector<uint8_t> nal = buildNal(w.toBytes());
    std::vector<uint8_t> out = h264RewriteSpsVui(nal.data(), nal.size());
    ParsedVui v = parseSpsVui(out);
    CHECK(v.ok);
    CHECK(v.fullRange == 1);
    CHECK(v.colourDesc == 1);
    CHECK(v.matrix == 1); // BT.709
    CHECK(v.videoFormat == 5); // preserved
  }

  // Case 4: full signal block, limited range, colour desc present (HD).
  {
    BitWriter w;
    writeBaselineHeader(w, kLevelIdc, 80, 45);
    w.bit(1);      // vui_parameters_present_flag
    w.bit(0);      // aspect_ratio_info_present_flag
    w.bit(0);      // overscan_info_present_flag
    w.bit(1);      // video_signal_type_present_flag
    w.bits_(5, 3); // video_format = unspecified
    w.bit(0);      // video_full_range_flag = limited
    w.bit(1);      // colour_description_present_flag
    w.bits_(5, 8); // colour_primaries = bt470bg
    w.bits_(5, 8); // transfer_characteristics = bt470bg
    w.bits_(5, 8); // matrix_coefficients = bt470bg
    w.bit(0);      // chroma_loc_info_present_flag
    w.bit(1);      // timing_info_present_flag
    w.bits_(1, 32);
    w.bits_(30, 32);
    w.bit(1);
    w.bit(0); // nal_hrd
    w.bit(0); // vcl_hrd
    w.bit(0); // pic_struct
    w.bit(0); // bitstream_restriction
    w.rbspTrailing();
    std::vector<uint8_t> nal = buildNal(w.toBytes());
    std::vector<uint8_t> out = h264RewriteSpsVui(nal.data(), nal.size());
    ParsedVui v = parseSpsVui(out);
    CHECK(v.ok);
    CHECK(v.fullRange == 1); // the bug: was 0, must become 1
    CHECK(v.colourDesc == 1);
    CHECK(v.matrix == 1); // BT.709
    CHECK(v.primaries == 1);
    CHECK(v.transfer == 1);
    CHECK(v.videoFormat == 5); // preserved
    CHECK(v.timingPresent);
    CHECK(v.numUnitsInTick == 1);

    // Idempotent: a second pass changes nothing.
    std::vector<uint8_t> again = h264RewriteSpsVui(out.data(), out.size());
    CHECK(again == out);
  }

  // Non-SPS NAL (PPS) must be returned byte-for-byte unchanged.
  {
    std::vector<uint8_t> pps = {0x68, 0xce, 0x3c, 0x80};
    std::vector<uint8_t> out = h264RewriteSpsVui(pps.data(), pps.size());
    CHECK(out == pps);
  }

  // Garbage/truncated input must not crash and must return the input.
  {
    std::vector<uint8_t> junk = {0x67, 0x64};
    std::vector<uint8_t> out = h264RewriteSpsVui(junk.data(), junk.size());
    CHECK(out == junk);
    std::vector<uint8_t> empty;
    CHECK(h264RewriteSpsVui(nullptr, 0).empty());
  }

  if (failures == 0) {
    std::printf("PASS: all H.264 SPS VUI rewrite checks passed\n");
    return 0;
  }
  std::printf("%d check(s) FAILED\n", failures);
  return 1;
}
