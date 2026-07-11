#ifndef SEIWriter_hpp
#define SEIWriter_hpp

#include <cstdint>
#include <string>
#include <vector>

class SEIWriter {
public:
  /// Build a complete SEI NAL unit (with 4-byte start code prefix)
  /// containing a user_data_unregistered SEI message with the Thingino
  /// OSD metadata as a JSON payload.
  ///
  /// @param is_h265  true for H.265 (PREFIX_SEI_NUT, type 39),
  ///                 false for H.264 (SEI, type 6)
  /// @param json     the JSON payload to embed
  /// @return         complete NAL unit bytes ready for insertion
  ///                 into an Annex B byte stream
  static std::vector<uint8_t> buildSEI(bool is_h265,
                                       const std::string &json);

  /// Thingino OSD SEI UUID (16 bytes, v4-compatible).
  /// Clients use this to identify Thingino-specific SEI messages.
  static const uint8_t THINGINO_SEI_UUID[16];

private:
  /// Apply H.264 Annex B emulation prevention (insert 0x03 after
  /// 0x00 0x00 0x00/0x01/0x02/0x03 sequences).
  static std::vector<uint8_t>
  applyEmulationPrevention(const std::vector<uint8_t> &rbsp);
};

#endif // SEIWriter_hpp
