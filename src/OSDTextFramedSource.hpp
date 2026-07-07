#ifndef OSDTextFramedSource_hpp
#define OSDTextFramedSource_hpp

#include "FramedSource.hh"

class OSD;

/// A live555 FramedSource that periodically emits the current OSD state
/// as a UTF-8 plaintext frame suitable for T.140 RTP transmission.
///
/// Frames are delivered at ~1 Hz via a scheduled delayed task.
/// During idle periods (no OSD text), empty frames are delivered so
/// that the T140IdleFilter in T140TextRTPSink can send idle RTP
/// keep-alive packets per RFC 4103.
class OSDTextFramedSource : public FramedSource {
public:
  static OSDTextFramedSource *createNew(UsageEnvironment &env,
                                        OSD *osd);

protected:
  OSDTextFramedSource(UsageEnvironment &env, OSD *osd);
  virtual ~OSDTextFramedSource();

  virtual void doGetNextFrame() override;
  virtual void doStopGettingFrames() override;

private:
  static void deliverFrame0(void *clientData);
  void deliverFrame();

  OSD *fOSD;
  TaskToken fNextTask;
  bool fIsActive;

  static constexpr int kFrameIntervalUs = 1000000; // 1 second
};

#endif // OSDTextFramedSource_hpp
