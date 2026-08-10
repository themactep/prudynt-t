#include "SEIWriter.hpp"

// Thingino OSD SEI UUID --- version 4 compatible.
// Clients match against this to identify Thingino metadata SEI messages.
const uint8_t SEIWriter::THINGINO_SEI_UUID[16] = {
    0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x47, 0x80,
    0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90,
};

std::vector<uint8_t>
SEIWriter::applyEmulationPrevention(const std::vector<uint8_t> &rbsp) {
  std::vector<uint8_t> result;
  result.reserve(rbsp.size() + rbsp.size() / 128); // rough headroom

  int zero_count = 0;
  for (uint8_t byte : rbsp) {
    if (zero_count == 2 && byte <= 0x03) {
      result.push_back(0x03); // emulation prevention byte
      zero_count = 0;
    }
    result.push_back(byte);
    if (byte == 0x00) {
      zero_count++;
    } else {
      zero_count = 0;
    }
  }

  return result;
}

std::vector<uint8_t> SEIWriter::buildSEI(bool is_h265,
                                         const std::string &json) {
  // -- 1. Build the SEI RBSP --------------------------------------

  std::vector<uint8_t> rbsp;

  // SEI payload type: user_data_unregistered = 5.
  // Fits in one byte (payloadType < 255), no extended encoding needed.
  rbsp.push_back(0x05);

  // SEI payload size = UUID (16) + user data (variable-length encoded).
  size_t payloadSize = 16 + json.size();
  while (payloadSize >= 0xFF) {
    rbsp.push_back(0xFF);
    payloadSize -= 0xFF;
  }
  rbsp.push_back(static_cast<uint8_t>(payloadSize));

  // UUID (16 bytes)
  rbsp.insert(rbsp.end(), THINGINO_SEI_UUID, THINGINO_SEI_UUID + 16);

  // User data payload (JSON)
  rbsp.insert(rbsp.end(), json.begin(), json.end());

  // RBSP trailing bits: rbsp_stop_one_bit + zero-pad to byte.
  rbsp.push_back(0x80);

  // -- 2. Apply emulation prevention -------------------------------

  std::vector<uint8_t> epb = applyEmulationPrevention(rbsp);

  // -- 3. Assemble NAL unit ----------------------------------------

  std::vector<uint8_t> nal;
  nal.reserve(4 + 2 + epb.size()); // start code + header + RBSP

  // 4-byte Annex B start code
  nal.insert(nal.end(), {0x00, 0x00, 0x00, 0x01});

  // NAL unit header
  if (is_h265) {
    // H.265: PREFIX_SEI_NUT (type 39), nuh_layer_id=0, temporal_id+1=1
    // header = (0<<15) | (39<<9) | (0<<3) | 1 = 0x4E01
    uint16_t header = static_cast<uint16_t>((0 << 15) | (39 << 9) |
                                            (0 << 3) | 1);
    nal.push_back(static_cast<uint8_t>((header >> 8) & 0xFF));
    nal.push_back(static_cast<uint8_t>(header & 0xFF));
  } else {
    // H.264: nal_unit_type = 6 (SEI), nal_ref_idc = 0
    nal.push_back(0x06);
  }

  // RBSP (emulation-prevention-processed)
  nal.insert(nal.end(), epb.begin(), epb.end());

  return nal;
}
