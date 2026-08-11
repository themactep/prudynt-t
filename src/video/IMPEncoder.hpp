#ifndef IMPEncoder_hpp
#define IMPEncoder_hpp

#include "config/Config.hpp"
#include "util/Logger.hpp"
#include "video/OSD.hpp"
#include "isp/imp_hal.hpp"
#include <array>
#include <imp/imp_common.h>
#include <imp/imp_system.h>

static const std::array<int, 64> jpeg_chroma_quantizer = {
    {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
     24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
     99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
     99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99}};

static const std::array<int, 64> jpeg_luma_quantizer = {
    {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
     14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
     18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
     49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99}};

class IMPEncoder {
public:
  static IMPEncoder *createNew(_stream *stream, int encChn, int encGrp,
                               int fsChn, const char *name);

  IMPEncoder(_stream *stream, int encChn, int encGrp, int fsChn,
             const char *name)
      : stream(stream), encChn(encChn), encGrp(encGrp), fsChn(fsChn),
        name(name) {
  }

  ~IMPEncoder() {
    deinit();
    destroy();
  };

  int init();
  int deinit(bool skipStopRecvPic = false);
  int destroy();
  static void flush(int encChn);

  OSD *osd = nullptr;

private:
  IMPEncoderCHNAttr chnAttr{};
  void initProfile();
  bool ownsGroupResources() const {
    return encGrp == encChn;
  }

  IMPCell fs{};
  IMPCell enc{};

  bool is_jpeg_stream{false};
  bool group_created{false};
  bool chn_created{false};
  bool chn_registered{false};
  bool fs_to_enc_bound{false};

  _stream *stream{};
  int encChn{};
  int encGrp{};
  int fsChn{};
  const char *name{};
};

#endif
