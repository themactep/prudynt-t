#ifndef IMPTextServerMediaSubsession_hpp
#define IMPTextServerMediaSubsession_hpp

#include "OnDemandServerMediaSubsession.hh"

class OSD;

/// OnDemandServerMediaSubsession that serves a T.140 text track
/// carrying OSD information (time, usertext, uptime, brightness)
/// as a subtitle channel alongside the video and audio streams.
class IMPTextServerMediaSubsession : public OnDemandServerMediaSubsession {
public:
  static IMPTextServerMediaSubsession *createNew(UsageEnvironment &env,
                                                 OSD *osd);

protected:
  IMPTextServerMediaSubsession(UsageEnvironment &env, OSD *osd);
  virtual ~IMPTextServerMediaSubsession();

  virtual FramedSource *createNewStreamSource(unsigned clientSessionId,
                                              unsigned &estBitrate) override;
  virtual RTPSink *createNewRTPSink(Groupsock *rtpGroupsock,
                                    unsigned char rtpPayloadTypeIfDynamic,
                                    FramedSource *inputSource) override;

private:
  OSD *fOSD;
};

#endif // IMPTextServerMediaSubsession_hpp
