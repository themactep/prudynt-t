#include "video/H264SpsRewrite.hpp"

#include <algorithm>

namespace {

// Bit vector: one element per bit (0 or 1), MSB first. SPS RBSPs are a few
// dozen bytes, so a byte-per-bit representation is cheap and makes splicing
// simple.
using Bits = std::vector<uint8_t>;

Bits bytesToBits(const uint8_t *data, size_t len) {
  Bits bits;
  bits.reserve(len * 8);
  for (size_t i = 0; i < len; ++i)
    for (int b = 7; b >= 0; --b)
      bits.push_back((data[i] >> b) & 1);
  return bits;
}

std::vector<uint8_t> bitsToBytes(const Bits &bits) {
  std::vector<uint8_t> out;
  out.reserve((bits.size() + 7) / 8);
  for (size_t i = 0; i < bits.size(); i += 8) {
    uint8_t byte = 0;
    for (int b = 0; b < 8; ++b) {
      byte = static_cast<uint8_t>(byte << 1);
      if (i + b < bits.size())
        byte = static_cast<uint8_t>(byte | bits[i + b]);
    }
    out.push_back(byte);
  }
  return out;
}

void appendBits(Bits &dst, uint32_t value, int count) {
  for (int i = count - 1; i >= 0; --i)
    dst.push_back((value >> i) & 1);
}

// Minimal H.264 bit reader over a bit vector; reads past the end return 0,
// which keeps a truncated SPS from crashing (the caller falls back to the
// unmodified input when the parse lands out of range).
struct BitReader {
  const Bits &bits;
  size_t pos = 0;
  explicit BitReader(const Bits &b) : bits(b) {
  }
  int getBit() {
    return (pos < bits.size()) ? bits[pos++] : 0;
  }
  uint32_t getBits(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i)
      v = (v << 1) | getBit();
    return v;
  }
  uint32_t getUE() {
    int z = 0;
    while (getBit() == 0 && z < 32)
      ++z;
    if (z == 0)
      return 0;
    // H.264 9.1.1: codeNum = 2^z - 1 + read_bits(z). The leading '1' is
    // consumed by the loop above; read_bits(z) is the z bits after it.
    uint32_t low = 0;
    for (int i = 0; i < z; ++i)
      low = (low << 1) | getBit();
    return (1u << z) - 1 + low;
  }
  int getSE() {
    uint32_t k = getUE();
    return (k & 1) ? static_cast<int>((k + 1) >> 1) : -static_cast<int>(k >> 1);
  }
};

// Remove emulation prevention bytes (00 00 03 -> 00 00) to recover the RBSP.
std::vector<uint8_t> stripEpb(const uint8_t *data, size_t len) {
  std::vector<uint8_t> rbsp;
  rbsp.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    if (i + 2 < len && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 3) {
      rbsp.push_back(0);
      rbsp.push_back(0);
      i += 2;
      continue;
    }
    rbsp.push_back(data[i]);
  }
  return rbsp;
}

// Re-insert emulation prevention bytes (00 00 00/01/02/03 -> 00 00 03 xx).
std::vector<uint8_t> insertEpb(const std::vector<uint8_t> &rbsp) {
  std::vector<uint8_t> out;
  out.reserve(rbsp.size() + rbsp.size() / 64);
  int zeros = 0;
  for (uint8_t b : rbsp) {
    if (zeros >= 2 && b <= 0x03) {
      out.push_back(0x03);
      zeros = 0;
    }
    out.push_back(b);
    if (b == 0)
      ++zeros;
    else
      zeros = 0;
  }
  return out;
}

struct BitEdit {
  size_t pos;    // bit offset in the RBSP payload
  size_t remove; // number of original bits to drop at pos
  Bits insert;   // replacement bits
};

// Apply non-overlapping bit edits in ascending position order.
Bits spliceBits(const Bits &bits, std::vector<BitEdit> edits) {
  std::sort(edits.begin(), edits.end(),
            [](const BitEdit &a, const BitEdit &b) { return a.pos < b.pos; });
  Bits out;
  out.reserve(bits.size() + 64);
  size_t cursor = 0;
  for (auto &e : edits) {
    if (e.pos < cursor || e.pos > bits.size())
      continue;
    if (e.pos + e.remove > bits.size())
      e.remove = bits.size() - e.pos;
    out.insert(out.end(), bits.begin() + cursor, bits.begin() + e.pos);
    out.insert(out.end(), e.insert.begin(), e.insert.end());
    cursor = e.pos + e.remove;
  }
  out.insert(out.end(), bits.begin() + cursor, bits.end());
  return out;
}

} // namespace

std::vector<uint8_t> h264RewriteSpsVui(const uint8_t *sps, size_t len) {
  if (!sps || len == 0)
    return {};
  std::vector<uint8_t> result(sps, sps + len); // unmodified on any failure
  if (len < 4 || (sps[0] & 0x1F) != 7)         // not an SPS NAL
    return result;

  std::vector<uint8_t> rbsp = stripEpb(sps + 1, len - 1);
  if (rbsp.size() < 3)
    return result;
  Bits bits = bytesToBits(rbsp.data(), rbsp.size());

  // Locate the rbsp_stop_one_bit: everything after the last '1' is alignment
  // zero padding, so the last set bit is the stop bit and the payload is the
  // prefix before it.
  size_t payloadEnd = bits.size();
  while (payloadEnd > 0 && bits[payloadEnd - 1] == 0)
    --payloadEnd;
  if (payloadEnd > 0)
    --payloadEnd;
  if (payloadEnd < 24) // no valid SPS is shorter than this before its VUI
    return result;

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
  uint32_t wMbs = br.getUE() + 1; // pic_width_in_mbs_minus1
  br.getUE();                     // pic_height_in_map_units_minus1
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
    return result; // truncated SPS --- nothing sane to splice

  size_t vuiPresentPos = br.pos;
  int vuiPresent = br.getBit();

  // Match the colour matrix to what the ISP actually emits: BT.709 for
  // 720p and up, BT.601 (SMPTE 170M) for SD.
  bool isHd = (wMbs * 16) >= 1280;
  uint8_t primaries = isHd ? 1 : 6;
  uint8_t transfer = isHd ? 1 : 6;
  uint8_t matrix = isHd ? 1 : 6;

  std::vector<BitEdit> edits;

  if (vuiPresent == 0) {
    // No VUI at all: insert a minimal one declaring full range + matrix.
    BitEdit e;
    e.pos = vuiPresentPos;
    e.remove = 1;
    e.insert.push_back(1);      // vui_parameters_present_flag
    e.insert.push_back(0);      // aspect_ratio_info_present_flag
    e.insert.push_back(0);      // overscan_info_present_flag
    e.insert.push_back(1);      // video_signal_type_present_flag
    appendBits(e.insert, 5, 3); // video_format = unspecified
    e.insert.push_back(1);      // video_full_range_flag
    e.insert.push_back(1);      // colour_description_present_flag
    appendBits(e.insert, primaries, 8);
    appendBits(e.insert, transfer, 8);
    appendBits(e.insert, matrix, 8);
    e.insert.push_back(0); // chroma_loc_info_present_flag
    e.insert.push_back(0); // timing_info_present_flag
    e.insert.push_back(0); // nal_hrd_parameters_present_flag
    e.insert.push_back(0); // vcl_hrd_parameters_present_flag
    e.insert.push_back(0); // pic_struct_present_flag
    e.insert.push_back(0); // bitstream_restriction_flag
    edits.push_back(std::move(e));
  } else {
    if (br.getBit()) { // aspect_ratio_info_present_flag
      uint32_t idc = br.getBits(8);
      if (idc == 255) {
        br.getBits(16); // sar_width
        br.getBits(16); // sar_height
      }
    }
    if (br.getBit()) // overscan_info_present_flag
      br.getBit();   // overscan_appropriate_flag

    size_t signalPresentPos = br.pos;
    int signalPresent = br.getBit();

    if (signalPresent == 0) {
      // VUI without a video-signal block: add one declaring full range.
      BitEdit e;
      e.pos = signalPresentPos;
      e.remove = 1;
      e.insert.push_back(1);      // video_signal_type_present_flag
      appendBits(e.insert, 5, 3); // video_format = unspecified
      e.insert.push_back(1);      // video_full_range_flag
      e.insert.push_back(1);      // colour_description_present_flag
      appendBits(e.insert, primaries, 8);
      appendBits(e.insert, transfer, 8);
      appendBits(e.insert, matrix, 8);
      edits.push_back(std::move(e));
    } else {
      br.getBits(3); // video_format (left as-is)
      size_t fullRangePos = br.pos;
      (void)br.getBit(); // video_full_range_flag

      BitEdit fr;
      fr.pos = fullRangePos;
      fr.remove = 1;
      fr.insert.push_back(1); // video_full_range_flag = 1
      edits.push_back(std::move(fr));

      size_t colourDescPos = br.pos;
      int colourDesc = br.getBit();
      if (colourDesc == 0) {
        BitEdit cd;
        cd.pos = colourDescPos;
        cd.remove = 1;
        cd.insert.push_back(1); // colour_description_present_flag
        appendBits(cd.insert, primaries, 8);
        appendBits(cd.insert, transfer, 8);
        appendBits(cd.insert, matrix, 8);
        edits.push_back(std::move(cd));
      } else {
        BitEdit cp;
        cp.pos = br.pos; // colour_primaries
        cp.remove = 24;
        appendBits(cp.insert, primaries, 8);
        appendBits(cp.insert, transfer, 8);
        appendBits(cp.insert, matrix, 8);
        edits.push_back(std::move(cp));
      }
    }
  }

  Bits payload(bits.begin(), bits.begin() + payloadEnd);
  Bits fixed = spliceBits(payload, edits);
  fixed.push_back(1); // rbsp_stop_one_bit
  while ((fixed.size() & 7) != 0)
    fixed.push_back(0); // align to byte boundary

  std::vector<uint8_t> out;
  out.reserve(1 + (fixed.size() / 8) + 8);
  out.push_back(sps[0]); // preserve NAL header verbatim
  std::vector<uint8_t> newEbsp = insertEpb(bitsToBytes(fixed));
  out.insert(out.end(), newEbsp.begin(), newEbsp.end());
  return out;
}
