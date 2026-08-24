#ifndef SPS_VUI_COMMON_HPP
#define SPS_VUI_COMMON_HPP

#include <algorithm>
#include <cstdint>
#include <vector>

// Shared bit-level machinery for rewriting an SPS VUI to declare full-range
// luma plus an explicit colour matrix. Used by both the H.264 and H.265 SPS
// rewriters: the VUI fields up to matrix_coefficients are laid out identically
// in both specs (H.264 E.1.1 / H.265 E.2.1), so only the SPS header walk that
// locates vui_parameters_present_flag differs between the two.

namespace spsvui {

using Bits = std::vector<uint8_t>; // one element per bit (0 or 1), MSB first

inline Bits bytesToBits(const uint8_t *data, size_t len) {
  Bits bits;
  bits.reserve(len * 8);
  for (size_t i = 0; i < len; ++i)
    for (int b = 7; b >= 0; --b)
      bits.push_back((data[i] >> b) & 1);
  return bits;
}

inline std::vector<uint8_t> bitsToBytes(const Bits &bits) {
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

inline void appendBits(Bits &dst, uint32_t value, int count) {
  for (int i = count - 1; i >= 0; --i)
    dst.push_back((value >> i) & 1);
}

// Minimal bit reader; reads past the end return 0 so a truncated SPS cannot
// crash (callers fall back to the unmodified input on a failed parse).
struct BitReader {
  const Bits &bits;
  size_t pos = 0;
  explicit BitReader(const Bits &b) : bits(b) {
  }
  void seek(size_t p) {
    pos = p;
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
    // H.264 9.1.1 / H.265 9.2: codeNum = 2^z - 1 + read_bits(z).
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
inline std::vector<uint8_t> stripEpb(const uint8_t *data, size_t len) {
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
inline std::vector<uint8_t> insertEpb(const std::vector<uint8_t> &rbsp) {
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
inline Bits spliceBits(const Bits &bits, std::vector<BitEdit> edits) {
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

// Given the full RBSP bits (including trailing bits) and the bit offset of
// vui_parameters_present_flag, splice the VUI to declare full-range luma plus
// an explicit matrix. Returns the new EBSP bytes (no NAL header).
//
// isHevc selects the VUI tail appended when the SPS has no VUI at all: H.265
// VUI carries a few extra single-bit fields between the colour block and the
// timing/bitstream-restriction flags that H.264 does not.
inline std::vector<uint8_t> applyVui(const Bits &bits, size_t vuiPresentPos,
                                     bool isHd, bool isHevc) {
  uint8_t primaries = isHd ? 1 : 6;
  uint8_t transfer = isHd ? 1 : 6;
  uint8_t matrix = isHd ? 1 : 6;

  // Locate the rbsp_stop_one_bit: everything after the last '1' is alignment
  // zero padding, so the last set bit is the stop bit and the payload is the
  // prefix before it.
  size_t payloadEnd = bits.size();
  while (payloadEnd > 0 && bits[payloadEnd - 1] == 0)
    --payloadEnd;
  if (payloadEnd > 0)
    --payloadEnd;

  BitReader br(bits);
  br.seek(vuiPresentPos);
  int vuiPresent = br.getBit();

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
    if (isHevc) {
      e.insert.push_back(0); // neutral_chroma_indication_flag
      e.insert.push_back(0); // field_seq_flag
      e.insert.push_back(0); // frame_field_info_present_flag
      e.insert.push_back(0); // default_display_window_flag
    }
    e.insert.push_back(0); // timing_info_present_flag
    if (!isHevc) {
      e.insert.push_back(0); // nal_hrd_parameters_present_flag
      e.insert.push_back(0); // vcl_hrd_parameters_present_flag
      e.insert.push_back(0); // pic_struct_present_flag
    }
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

  return insertEpb(bitsToBytes(fixed));
}

} // namespace spsvui

#endif // SPS_VUI_COMMON_HPP
