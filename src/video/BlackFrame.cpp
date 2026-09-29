#include "video/BlackFrame.hpp"

#include <cstring>

namespace blackframe {

namespace {

class BitReader {
public:
  BitReader(const uint8_t *d, size_t n) : d_(d), n_(n) {}
  uint32_t u(int bits) {
    uint32_t v = 0;
    for (int i = 0; i < bits; ++i) {
      size_t byte = bit_ >> 3;
      if (byte >= n_)
        return v << (bits - i);
      v = (v << 1) | ((d_[byte] >> (7 - (bit_ & 7))) & 1);
      ++bit_;
    }
    return v;
  }
  uint32_t ue() {
    int z = 0;
    while (u(1) == 0 && z < 32)
      ++z;
    if (z == 0)
      return 0;
    return ((1u << z) - 1) + u(z);
  }
  int32_t se() {
    uint32_t k = ue();
    return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
  }
  size_t bit() const { return bit_; }

private:
  const uint8_t *d_;
  size_t n_;
  size_t bit_ = 0;
};

class BitWriter {
public:
  void u(int bits, uint32_t v) {
    for (int i = bits - 1; i >= 0; --i)
      putBit((v >> i) & 1);
  }
  void ue(uint32_t v) {
    uint32_t val = v + 1;
    int n = 0;
    for (uint32_t t = val; t; t >>= 1)
      ++n;
    u(n - 1, 0);
    u(n, val);
  }
  void se(int32_t v) { ue(v > 0 ? (uint32_t)(2 * v - 1) : (uint32_t)(-2 * v)); }
  void align(int bit) {
    while (nbits_ % 8)
      putBit(bit);
  }
  std::vector<uint8_t> bytes() const { return buf_; }

private:
  void putBit(int b) {
    if ((nbits_ & 7) == 0)
      buf_.push_back(0);
    if (b)
      buf_.back() |= (uint8_t)(1u << (7 - (nbits_ & 7)));
    ++nbits_;
  }
  std::vector<uint8_t> buf_;
  size_t nbits_ = 0;
};

std::vector<uint8_t> unescape(const uint8_t *b, size_t n) {
  std::vector<uint8_t> out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    if (i + 2 < n && b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 3) {
      out.push_back(0);
      out.push_back(0);
      i += 2;
    } else {
      out.push_back(b[i]);
    }
  }
  return out;
}

std::vector<uint8_t> escape(const std::vector<uint8_t> &b) {
  std::vector<uint8_t> out;
  out.reserve(b.size() + 8);
  int zeros = 0;
  for (uint8_t x : b) {
    if (zeros >= 2 && x <= 3) {
      out.push_back(3);
      zeros = 0;
    }
    out.push_back(x);
    zeros = (x == 0) ? zeros + 1 : 0;
  }
  return out;
}

void skipScalingList(BitReader &r, int size) {
  int last = 8, next = 8;
  for (int j = 0; j < size; ++j) {
    if (next != 0)
      next = (last + r.se() + 256) % 256;
    last = (next != 0) ? next : last;
  }
}

struct SpsInfo {
  int profile = 0;
  int chroma = 1;
  int log2_fnum = 4;
  int poc_type = 0;
  int log2_poc = 0;
  bool frame_mbs_only = true;
  bool bottom_field_poc = false;
  int width = 0;
  int height = 0;
  bool valid = false;
};

SpsInfo parseSps(const std::vector<uint8_t> &sps) {
  SpsInfo s;
  if (sps.size() < 4)
    return s;
  auto rbsp = unescape(sps.data() + 1, sps.size() - 1);
  BitReader r(rbsp.data(), rbsp.size());
  s.profile = sps[1];
  r.u(24); // profile/constraints/level
  r.ue();  // sps id
  bool high = (s.profile == 100 || s.profile == 110 || s.profile == 122 ||
               s.profile == 244 || s.profile == 44 || s.profile == 83 ||
               s.profile == 86 || s.profile == 118 || s.profile == 128 ||
               s.profile == 138 || s.profile == 139 || s.profile == 134 ||
               s.profile == 135);
  if (high) {
    s.chroma = r.ue();
    if (s.chroma == 3)
      r.u(1);
    r.ue();
    r.ue();
    r.u(1);
    if (r.u(1)) {
      int n = (s.chroma == 3) ? 12 : 8;
      for (int i = 0; i < n; ++i)
        if (r.u(1))
          skipScalingList(r, i < 6 ? 16 : 64);
    }
  }
  s.log2_fnum = (int)r.ue() + 4;
  s.poc_type = (int)r.ue();
  if (s.poc_type == 0) {
    s.log2_poc = (int)r.ue() + 4;
  } else if (s.poc_type == 1) {
    r.u(1);
    r.se();
    r.se();
    int n = (int)r.ue();
    for (int i = 0; i < n; ++i)
      r.se();
  }
  r.ue(); // max num ref frames
  r.u(1); // gaps
  int w_mbs = (int)r.ue() + 1;
  int h_map = (int)r.ue() + 1;
  s.frame_mbs_only = r.u(1) != 0;
  if (!s.frame_mbs_only)
    r.u(1); // mbaff
  r.u(1);   // direct 8x8
  int crop[4] = {0, 0, 0, 0};
  if (r.u(1))
    for (int i = 0; i < 4; ++i)
      crop[i] = (int)r.ue();
  s.width = w_mbs * 16 - (crop[0] + crop[1]) * 2;
  s.height = h_map * 16 * (2 - (s.frame_mbs_only ? 1 : 0)) -
             (crop[2] + crop[3]) * 2;
  s.valid = (s.chroma >= 1 && s.chroma <= 3) && s.width > 0 && s.height > 0;
  return s;
}

struct PpsInfo {
  int pps_id = 0;
  int sps_id = 0;
  bool bottom_field_poc = false;
  int l0 = 0, l1 = 0;
  int wpred = 0, wbipred = 0;
  int qp = 0, qs = 0, cqp = 0;
  bool deblock = false;
  bool cip = false;
  bool redundant = false;
  bool has_tail = false;
  int t8 = 0, sm = 0, scq = 0;
  bool valid = false;
};

PpsInfo parsePps(const std::vector<uint8_t> &pps, int &num_groups) {
  PpsInfo p;
  if (pps.size() < 2)
    return p;
  auto rbsp = unescape(pps.data() + 1, pps.size() - 1);
  BitReader r(rbsp.data(), rbsp.size());
  p.pps_id = (int)r.ue();
  p.sps_id = (int)r.ue();
  r.u(1); // entropy
  p.bottom_field_poc = r.u(1) != 0;
  num_groups = (int)r.ue() + 1;
  if (num_groups != 1)
    return p;
  p.l0 = (int)r.ue();
  p.l1 = (int)r.ue();
  p.wpred = (int)r.u(1);
  p.wbipred = (int)r.u(2);
  p.qp = r.se();
  p.qs = r.se();
  p.cqp = r.se();
  p.deblock = r.u(1) != 0;
  p.cip = r.u(1) != 0;
  p.redundant = r.u(1) != 0;
  size_t remaining = rbsp.size() * 8 - r.bit();
  if (remaining > 8) {
    p.has_tail = true;
    p.t8 = (int)r.u(1);
    if (p.t8)
      p.sm = (int)r.u(1);
    if (p.sm)
      return p; // scaling matrices unsupported
    p.scq = r.se();
  }
  p.valid = true;
  return p;
}

std::vector<uint8_t> buildTwinPps(const PpsInfo &p, int id) {
  BitWriter w;
  w.ue((uint32_t)id);
  w.ue((uint32_t)p.sps_id);
  w.u(1, 0); // entropy_coding_mode_flag = 0 (CAVLC)
  w.u(1, p.bottom_field_poc ? 1 : 0);
  w.ue(0); // num_slice_groups_minus1
  w.ue((uint32_t)p.l0);
  w.ue((uint32_t)p.l1);
  w.u(1, (uint32_t)p.wpred);
  w.u(2, (uint32_t)p.wbipred);
  w.se(p.qp);
  w.se(p.qs);
  w.se(p.cqp);
  w.u(1, p.deblock ? 1 : 0);
  w.u(1, p.cip ? 1 : 0);
  w.u(1, p.redundant ? 1 : 0);
  if (p.has_tail) {
    w.u(1, (uint32_t)p.t8);
    if (p.t8)
      w.u(1, (uint32_t)p.sm);
    w.se(p.scq);
  }
  w.u(1, 1); // rbsp_stop_one_bit
  w.align(0);
  auto body = escape(w.bytes());
  std::vector<uint8_t> nal;
  nal.push_back(0x68);
  nal.insert(nal.end(), body.begin(), body.end());
  return nal;
}

} // namespace

std::vector<uint8_t> buildBlackPps(const std::vector<uint8_t> &pps) {
  std::vector<uint8_t> empty;
  if (pps.size() < 2 || (pps[0] & 0x1F) != 8)
    return empty;
  int num_groups = 1;
  PpsInfo p = parsePps(pps, num_groups);
  if (!p.valid || num_groups != 1)
    return empty;
  return buildTwinPps(p, (p.pps_id + 1) & 0xFF);
}

std::vector<uint8_t> buildBlackAccessUnit(const std::vector<uint8_t> &sps,
                                          const std::vector<uint8_t> &pps) {
  std::vector<uint8_t> empty;
  if (sps.size() < 4 || pps.size() < 2)
    return empty;
  if ((sps[0] & 0x1F) != 7 || (pps[0] & 0x1F) != 8)
    return empty; // not an H.264 SPS/PPS

  SpsInfo s = parseSps(sps);
  int num_groups = 1;
  PpsInfo p = parsePps(pps, num_groups);
  if (!s.valid || !p.valid || num_groups != 1)
    return empty;
  if (!s.frame_mbs_only)
    return empty; // interlaced not expected on these sensors

  const int twin_pps_id = (p.pps_id + 1) & 0xFF;
  auto twin_pps = buildTwinPps(p, twin_pps_id);

  const int mbs = (s.width / 16) * (s.height / 16);
  if (mbs <= 0)
    return empty;

  BitWriter w;
  w.ue(0);                       // first_mb_in_slice
  w.ue(7);                       // slice_type = I (all slices in frame)
  w.ue((uint32_t)twin_pps_id);
  w.u(s.log2_fnum, 0);           // frame_num
  w.ue(0);                       // idr_pic_id
  if (s.poc_type == 0)
    w.u(s.log2_poc, 0);          // pic_order_cnt_lsb
  if (p.redundant)
    w.ue(0);                     // redundant_pic_cnt
  w.u(1, 0);                     // no_output_of_prior_pics_flag
  w.u(1, 0);                     // long_term_reference_flag
  w.se(0);                       // slice_qp_delta
  if (p.deblock)
    w.ue(1);                     // disable_deblocking_filter_idc

  // Uniform Intra16x16 cover: DC prediction with zero residual decodes to a
  // flat mid-grey frame, about 1.4 KB for 768x432 and 8 KB for 1080p. There is
  // no hardware OSD on the open tx-isp stack, so this software keyframe is what
  // hides the scene while privacy is active.
  //   mb_type 3 = Intra_16x16, Intra16x16PredMode = DC, CodedBlockPattern = 0
  //   coeff_token for TotalCoeff = 0 (luma DC) is the single bit '1'
  for (int i = 0; i < mbs; ++i) {
    w.ue(3);                     // mb_type
    w.ue(0);                     // intra_chroma_pred_mode = DC
    w.se(0);                     // mb_qp_delta
    w.u(1, 1);                   // luma DC coeff_token, TotalCoeff = 0
  }
  w.u(1, 1);                     // rbsp_stop_one_bit
  w.align(0);

  auto idr_body = escape(w.bytes());
  std::vector<uint8_t> idr;
  idr.push_back(0x65);           // nal_ref_idc=3, nal_unit_type=5
  idr.insert(idr.end(), idr_body.begin(), idr_body.end());

  const uint8_t sc[4] = {0x00, 0x00, 0x00, 0x01};
  std::vector<uint8_t> au;
  au.reserve(sps.size() + pps.size() + twin_pps.size() + idr.size() + 16);
  au.insert(au.end(), sc, sc + 4);
  au.insert(au.end(), sps.begin(), sps.end());
  au.insert(au.end(), sc, sc + 4);
  au.insert(au.end(), pps.begin(), pps.end());
  au.insert(au.end(), sc, sc + 4);
  au.insert(au.end(), twin_pps.begin(), twin_pps.end());
  au.insert(au.end(), sc, sc + 4);
  au.insert(au.end(), idr.begin(), idr.end());
  return au;
}

} // namespace blackframe
