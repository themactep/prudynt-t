#ifndef IMPFramesource_hpp
#define IMPFramesource_hpp

#include "Logger.hpp"
#include <imp/imp_framesource.h>

class IMPFramesource {
public:
  static IMPFramesource *createNew(_stream *stream, _sensor *sensor, int chnNr,
                                   int sourceChn);

  IMPFramesource(_stream *stream, _sensor *sensor, int chnNr, int sourceChn)
      : stream(stream), sensor(sensor), chnNr(chnNr), sourceChn(sourceChn) {
    init();
  }

  ~IMPFramesource() {
    destroy();
  };

  int init();
  int enable();
  int disable();
  int destroy();

private:
  _stream *stream{};
  _sensor *sensor{};
  int chnNr;
  int sourceChn;
};

#endif
