#pragma once
// Timestamp policy shared by the encoder worker, RTP writer and native tests.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace access_unit_timing {

struct Pack {
  int64_t timestamp_us = 0;
  bool picture = false;
  bool first_slice = false;
  bool prefix = false;
  bool valid = true;
};

// Only the first bytes are needed: H264 first_mb_in_slice==0 starts with a
// one-bit Exp-Golomb code; H265 carries first_slice_segment_in_pic_flag.
inline Pack describe(const uint8_t *data, size_t size, bool hevc,
                     int64_t timestamp_us) {
  Pack out;
  out.timestamp_us = timestamp_us;
  if (!data || size == 0) {
    out.valid = false;
    return out;
  }
  size_t start = 0;
  if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1)
    start = 4;
  else if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)
    start = 3;
  if (!data || size <= start) {
    out.valid = false;
    return out;
  }
  const unsigned type = hevc ? ((data[start] >> 1) & 63) : (data[start] & 31);
  out.picture = hevc ? type <= 31 : (type == 1 || type == 5);
  out.prefix = hevc ? (type >= 32 && type <= 35) || type == 39
                    : type == 6 || type == 7 || type == 8 || type == 9;
  if (out.picture) {
    const size_t payload = start + (hevc ? 2 : 1);
    if (size <= payload)
      out.valid = false;
    else
      out.first_slice = (data[payload] & 0x80) != 0;
  } else if (!hevc && (type == 0 || type == 2 || type == 3 || type == 4)) {
    // Data partitions need a full H264 picture parser; do not guess.
    out.valid = false;
  }
  return out;
}

struct Unit {
  size_t begin = 0;
  size_t end = 0; // inclusive pack indices
  int64_t timestamp_us = 0;
  bool picture = false;
  bool valid = true;
};

inline std::vector<Unit> group(const std::vector<Pack> &packs) {
  std::vector<Unit> units;
  if (packs.empty())
    return units;
  Unit current;
  for (size_t i = 0; i < packs.size(); ++i) {
    const Pack &p = packs[i];
    // Config before another picture belongs to the next access unit.
    // frameEnd is deliberately absent: SDKs set it on config NALs or
    // leave it unset entirely, neither of which creates a picture.
    if (current.picture && (p.prefix || (p.picture && p.first_slice))) {
      current.end = i - 1;
      units.push_back(current);
      current = Unit{};
      current.begin = i;
    }
    current.valid = current.valid && p.valid;
    if (p.picture) {
      if (!current.picture && !p.first_slice)
        current.valid = false;
      current.picture = true;
      if (p.timestamp_us > 0) {
        if (current.timestamp_us > 0 && current.timestamp_us != p.timestamp_us)
          current.valid = false;
        else
          current.timestamp_us = p.timestamp_us;
      }
    }
  }
  current.end = packs.size() - 1;
  units.push_back(current);
  return units;
}

enum class Result { Accepted, Invalid, Repeated, Backward };
class PictureClock {
  int64_t last_us_ = 0;

public:
  void reset() {
    last_us_ = 0;
  }
  int64_t last() const {
    return last_us_;
  }
  Result accept(int64_t capture_us) {
    if (capture_us <= 0)
      return Result::Invalid;
    if (capture_us == last_us_)
      return Result::Repeated;
    if (last_us_ > 0 && capture_us < last_us_)
      return Result::Backward;
    last_us_ = capture_us;
    return Result::Accepted;
  }
};

inline uint32_t rtp90k(int64_t capture_us, int64_t anchor_us) {
  if (capture_us < anchor_us)
    return 0;
  const uint64_t elapsed = static_cast<uint64_t>(capture_us - anchor_us);
  // Split the quotient so long captures cannot overflow elapsed*9.
  return static_cast<uint32_t>((elapsed / 100) * 9 + (elapsed % 100) * 9 / 100);
}

} // namespace access_unit_timing
