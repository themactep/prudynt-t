#include "video/H265CodecConfig.hpp"

// Profile/level live in the SPS RBSP, after the 2-byte HEVC NAL header:
//   byte[2]: sps_vps_id(4)|max_sub_layers(3)|temporal_nested(1)
//   byte[3]: profile_space(2)|tier_flag(1)|profile_idc(5)
//   byte[4..7]: profile_compatibility_flags
//   byte[8..13]: constraint_indicator_flags
//   byte[14]: level_idc
// A too-short SPS falls back to zeroed fields, which every decoder accepts.
bool buildHvcC(const std::vector<uint8_t> &vps,
               const std::vector<uint8_t> &sps,
               const std::vector<uint8_t> &pps, std::vector<uint8_t> &hvcC) {
  if (sps.size() < 4 || pps.empty()) {
    return false;
  }

  uint8_t general_profile_space = 0;
  uint8_t general_tier_flag = 0;
  uint8_t general_profile_idc = 0;
  uint8_t general_profile_compat[4] = {0, 0, 0, 0};
  uint8_t general_constraint[6] = {0, 0, 0, 0, 0, 0};
  uint8_t general_level_idc = 0;
  uint8_t num_temporal_layers = 1;
  uint8_t temporal_id_nested = 0;

  if (sps.size() >= 3) {
    num_temporal_layers = (((sps[2] >> 1) & 0x07) + 1);
    temporal_id_nested = sps[2] & 0x01;
  }
  if (sps.size() >= 4) {
    general_profile_space = (sps[3] >> 6) & 0x03;
    general_tier_flag = (sps[3] >> 5) & 0x01;
    general_profile_idc = sps[3] & 0x1F;
  }
  if (sps.size() >= 8) {
    for (int i = 0; i < 4; ++i)
      general_profile_compat[i] = sps[4 + i];
  }
  if (sps.size() >= 14) {
    for (int i = 0; i < 6; ++i)
      general_constraint[i] = sps[8 + i];
  }
  if (sps.size() >= 15) {
    general_level_idc = sps[14];
  }

  hvcC.clear();

  // configurationVersion
  hvcC.push_back(0x01);
  // general_profile_space | general_tier_flag | general_profile_idc
  hvcC.push_back(static_cast<uint8_t>((general_profile_space << 6) |
                                      (general_tier_flag << 5) |
                                      general_profile_idc));
  // general_profile_compatibility_flags (32 bits)
  for (int i = 0; i < 4; ++i)
    hvcC.push_back(general_profile_compat[i]);
  // general_constraint_indicator_flags (48 bits)
  for (int i = 0; i < 6; ++i)
    hvcC.push_back(general_constraint[i]);
  // general_level_idc
  hvcC.push_back(general_level_idc);
  // reserved(4 bits=0xF) | min_spatial_segmentation_idc(12 bits=0)
  hvcC.push_back(0xF0);
  hvcC.push_back(0x00);
  // reserved(6 bits=0x3F) | parallelismType(2 bits=0)
  hvcC.push_back(0xFC);
  // reserved(6 bits=0x3F) | chromaFormat(2 bits=1 -> 4:2:0)
  hvcC.push_back(0xFD);
  // reserved(5 bits=0x1F) | bitDepthLumaMinus8(3 bits=0)
  hvcC.push_back(0xF8);
  // reserved(5 bits=0x1F) | bitDepthChromaMinus8(3 bits=0)
  hvcC.push_back(0xF8);
  // avgFrameRate (0 = unspecified)
  hvcC.push_back(0x00);
  hvcC.push_back(0x00);
  // constantFrameRate(2)|numTemporalLayers(3)|temporalIdNested(1)|lengthSizeMinusOne(2=3,
  // i.e. 4-byte lengths)
  hvcC.push_back(static_cast<uint8_t>((0 << 6) |
                                      ((num_temporal_layers & 0x07) << 3) |
                                      (temporal_id_nested << 2) | 0x03));

  // NAL unit arrays: VPS (32), SPS (33), PPS (34). Empty ones are omitted.
  struct NalEntry {
    uint8_t nal_type;
    const std::vector<uint8_t> *data;
  };
  NalEntry entries[3] = {
      {32, &vps},
      {33, &sps},
      {34, &pps},
  };

  uint8_t num_arrays = 0;
  for (auto &e : entries) {
    if (!e.data->empty())
      ++num_arrays;
  }
  hvcC.push_back(num_arrays);

  for (auto &e : entries) {
    if (e.data->empty())
      continue;
    // array_completeness(1)=1 | reserved(1)=0 | NAL_unit_type(6)
    hvcC.push_back(static_cast<uint8_t>(0x80 | (e.nal_type & 0x3F)));
    // numNalus = 1
    hvcC.push_back(0x00);
    hvcC.push_back(0x01);
    // nalUnitLength
    uint16_t nal_len = static_cast<uint16_t>(e.data->size());
    hvcC.push_back(static_cast<uint8_t>(nal_len >> 8));
    hvcC.push_back(static_cast<uint8_t>(nal_len & 0xFF));
    // nalUnit
    hvcC.insert(hvcC.end(), e.data->begin(), e.data->end());
  }

  return true;
}
