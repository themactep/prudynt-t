#ifndef RTSP_hpp
#define RTSP_hpp

#include "BasicUsageEnvironment.hh"
#include "IMPAudioServerMediaSubsession.hpp"
#include "IMPDeviceSource.hpp"
#include "IMPEncoder.hpp"
#include "IMPServerMediaSubsession.hpp"
#include "Logger.hpp"
#include "liveMedia.hh"

class RTSP {
public:
  RTSP() {};
  void addSubsession(int chnNr, _stream &stream);
  void start();
  static void *run(void *arg);

private:
  UsageEnvironment *env{};
  TaskScheduler *scheduler{};
  RTSPServer *rtspServer{};
  int audioChn = 0;
  IMPDeviceSource<AudioFrame, audio_stream> *audioSource = nullptr;
};

#endif
