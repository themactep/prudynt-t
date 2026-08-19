#ifndef IMPFramesource_hpp
#define IMPFramesource_hpp

#include "util/Logger.hpp"
#include <imp/imp_framesource.h>

class IMPFramesource {
public:
  static IMPFramesource *createNew(_stream *stream, _sensor *sensor, int chnNr);

  IMPFramesource(_stream *stream, _sensor *sensor, int chnNr)
      : stream(stream), sensor(sensor), chnNr(chnNr) {
    init();
  }

  ~IMPFramesource() {
    destroy();
  };

  int init();
  int enable();
  int disable();
  int destroy();

  // True when this channel is eligible for the runtime single-buffer
  // fallback: non-scaled T31 channel with auto-configured buffers.  Used
  // by the VideoWorker watchdog to recover from old tx-isp drivers that
  // reject 2+ buffers on the physical channel (silently, no error).
  bool canFallbackToSingleBuffer() const;

  // Disable, re-attribute with nrVBs=1 and re-enable the channel.  Returns
  // 0 on success.  Only meaningful after the channel was created.
  int fallbackToSingleBuffer();

private:
  _stream *stream{};
  _sensor *sensor{};
  int chnNr;
  bool fs_scale{false};
  bool nrVBs_was_auto{false};
};

#endif
