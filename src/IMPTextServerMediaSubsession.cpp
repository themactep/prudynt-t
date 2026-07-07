#include "IMPTextServerMediaSubsession.hpp"
#include "OSDTextFramedSource.hpp"
#include "OSD.hpp"
#include "Logger.hpp"
#include "GroupsockHelper.hh"
#include "T140TextRTPSink.hh"

#undef MODULE
#define MODULE "TextSub"

IMPTextServerMediaSubsession *
IMPTextServerMediaSubsession::createNew(UsageEnvironment &env, OSD *osd) {
  return new IMPTextServerMediaSubsession(env, osd);
}

IMPTextServerMediaSubsession::IMPTextServerMediaSubsession(
    UsageEnvironment &env, OSD *osd)
    : OnDemandServerMediaSubsession(env, False /* reuseFirstSource */),
      fOSD(osd) {
  LOG_DEBUG("T.140 text subsession created for OSD");
}

IMPTextServerMediaSubsession::~IMPTextServerMediaSubsession() {
}

FramedSource *
IMPTextServerMediaSubsession::createNewStreamSource(unsigned /*clientSessionId*/,
                                                    unsigned &estBitrate) {
  estBitrate = 4; // ~4 kbps for OSD text (very low overhead)
  return OSDTextFramedSource::createNew(envir(), fOSD);
}

RTPSink *IMPTextServerMediaSubsession::createNewRTPSink(
    Groupsock *rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic,
    FramedSource * /*inputSource*/) {
  // Increase send buffer for the groupsock
  increaseSendBufferTo(envir(), rtpGroupsock->socketNum(), 65536);

  // T.140 text uses dynamic payload type (RFC 4103).
  // The timestamp frequency is 1000 Hz and payload format is "t140".
  return T140TextRTPSink::createNew(envir(), rtpGroupsock,
                                    rtpPayloadTypeIfDynamic);
}
