#ifndef IMPAudio_hpp
#define IMPAudio_hpp

#include "config/Config.hpp"
#include "util/Logger.hpp"
#include <imp/imp_audio.h>

enum IMPAudioFormat {
  PCM,
  G711A,
  G711U,
  G726,
  OPUS,
  AAC,
};

class IMPAudioEncoder {
public:
  virtual int open() = 0;
  virtual int encode(IMPAudioFrame *data, unsigned char *outbuf,
                     int *outLen) = 0;
  virtual int close() = 0;
  virtual void setInputRate(int /*rate*/) {}
  virtual ~IMPAudioEncoder() = default;

  // RTTI-free accessors (dynamic_cast removed for -fno-rtti)
  virtual bool isAAC() const { return false; }
  virtual int getFrameSamples() const { return 0; }
  virtual int64_t getLastFramePtsUs() const { return 0; }
  virtual const uint8_t *getAsc(uint32_t &len) const {
    len = 0;
    return nullptr;
  }
};

class IMPAudio {
public:
  static IMPAudio *createNew(int devId, int inChn, int aeChn);
  static int encodeDirect(IMPAudioFrame *frame, unsigned char *outbuf,
                           int *outLen);
  // T10/T20 AAC: access encoder internals
  static int getAACFrameSamples();
  static int64_t getAACLastPtsUs();
  static bool isAACEncoder();
  static const uint8_t *getAACAsc(uint32_t &len);

  IMPAudio(int devId, int inChn, int aeChn)
      : devId(devId), inChn(inChn), aeChn(aeChn) {
    if (init() != 0) {
      init_failed = true;
    }
  };

  ~IMPAudio() {
    deinit();
  };

  int init();
  int deinit();
  int bitrate; // computed during setup, in Kbps
  int sample_rate;
  IMPAudioFormat format;
  bool directEncode = false;

  int devId{};
  int inChn{};
  int aeChn{};
  int outChnCnt = 1;

private:
  bool init_failed = false;
  bool enabledAgc = false;
  bool enabledHpf = false;
  bool enabledNs = false;
  int handle = 0;
  const char *name{};
  _stream *stream{};
};

#endif
