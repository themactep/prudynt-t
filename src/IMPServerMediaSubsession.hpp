#ifndef IMPServerMediaSubsession_hpp
#define IMPServerMediaSubsession_hpp

#include "Config.hpp"
#include "OnDemandServerMediaSubsession.hh"
#include "ServerMediaSession.hh"
#include "StreamReplicator.hh"
#include "globals.hpp"

class IMPServerMediaSubsession : public OnDemandServerMediaSubsession {
public:
  static void init() {};

  static IMPServerMediaSubsession *createNew(UsageEnvironment &env,
                                             H264NALUnit *vps, // Change to pointer for optional VPS
                                             H264NALUnit sps, H264NALUnit pps, int encChn);

protected:
  // Constructor with VPS as a pointer for optional usage
  IMPServerMediaSubsession(UsageEnvironment &env,
                           H264NALUnit *vps, // Change to pointer for optional VPS
                           H264NALUnit sps, H264NALUnit pps, int encChn);
  virtual ~IMPServerMediaSubsession();

  virtual FramedSource *createNewStreamSource(unsigned clientSessionId, unsigned &estBitrate);
  virtual RTPSink *createNewRTPSink(Groupsock *rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic,
                                    FramedSource *inputSource);

  // Override sdpLines to refresh SDP when encoder SPS/PPS change
  virtual char const *sdpLines(int addressFamily) override;

  virtual void startStream(unsigned clientSessionId, void *streamToken, TaskFunc *rtcpRRHandler,
                           void *rtcpRRHandlerClientData, unsigned short &rtpSeqNum, unsigned &rtpTimestamp,
                           ServerRequestAlternativeByteHandler *serverRequestAlternativeByteHandler,
                           void *serverRequestAlternativeByteHandlerClientData) override {
    // StreamCore cursors start fresh automatically, no need to clear
    // request idr frame every second for the next x seconds
    global_video[encChn]->idr_fix = 5;
    IMPEncoder::flush(encChn);

    OnDemandServerMediaSubsession::startStream(clientSessionId, streamToken, rtcpRRHandler, rtcpRRHandlerClientData,
                                               rtpSeqNum, rtpTimestamp, serverRequestAlternativeByteHandler,
                                               serverRequestAlternativeByteHandlerClientData);
    global_rtsp_clients.fetch_add(1, std::memory_order_relaxed);
  }

  virtual void deleteStream(unsigned clientSessionId, void *&streamToken) override {
    OnDemandServerMediaSubsession::deleteStream(clientSessionId, streamToken);
    int prev = global_rtsp_clients.load(std::memory_order_relaxed);
    while (prev > 0 &&
           !global_rtsp_clients.compare_exchange_weak(prev, prev - 1, std::memory_order_relaxed)) {
      ;
    }
  }

private:
  H264NALUnit *vps; // Change to pointer for optional VPS
  H264NALUnit sps;
  H264NALUnit pps;
  int encChn;

  // Track whether codec config has changed since last SDP generation
  std::vector<uint8_t> lastKnownSps;
  std::vector<uint8_t> lastKnownPps;
  std::vector<uint8_t> lastKnownVps;
};

#endif
