#include "OSDTextFramedSource.hpp"
#include "OSD.hpp"
#include "Logger.hpp"

#undef MODULE
#define MODULE "OSDTextSrc"

OSDTextFramedSource *OSDTextFramedSource::createNew(UsageEnvironment &env,
                                                    OSD *osd) {
  return new OSDTextFramedSource(env, osd);
}

OSDTextFramedSource::OSDTextFramedSource(UsageEnvironment &env, OSD *osd)
    : FramedSource(env), fOSD(osd), fNextTask(nullptr), fIsActive(false) {
}

OSDTextFramedSource::~OSDTextFramedSource() {
  if (fNextTask != nullptr) {
    envir().taskScheduler().unscheduleDelayedTask(fNextTask);
    fNextTask = nullptr;
  }
}

void OSDTextFramedSource::doGetNextFrame() {
  if (!fIsActive) {
    fIsActive = true;
    // First call: deliver immediately so the client sees OSD text right away
    deliverFrame();
    return;
  }

  // Subsequent calls: use timer-based delivery to limit update rate to ~1 Hz
  if (fNextTask == nullptr) {
    fNextTask = envir().taskScheduler().scheduleDelayedTask(
        kFrameIntervalUs, deliverFrame0, this);
  }
}

void OSDTextFramedSource::doStopGettingFrames() {
  fIsActive = false;
  if (fNextTask != nullptr) {
    envir().taskScheduler().unscheduleDelayedTask(fNextTask);
    fNextTask = nullptr;
  }
  FramedSource::doStopGettingFrames();
}

void OSDTextFramedSource::deliverFrame0(void *clientData) {
  ((OSDTextFramedSource *)clientData)->deliverFrame();
}

void OSDTextFramedSource::deliverFrame() {
  fNextTask = nullptr; // task has fired

  if (!isCurrentlyAwaitingData()) {
    // No downstream consumer is waiting; stop. The next doGetNextFrame()
    // call will re-schedule delivery.
    return;
  }

  if (!fOSD) {
    // No OSD object yet — deliver empty frame as keep-alive
    fFrameSize = 0;
    fNumTruncatedBytes = 0;
    gettimeofday(&fPresentationTime, nullptr);
    fDurationInMicroseconds = 0;
    afterGetting(this);
    return;
  }

  // Get the current OSD state as plaintext
  std::string text = fOSD->getPlaintextInfo();

  if (text.empty()) {
    // Empty frame — T140IdleFilter in the sink will enter idle mode
    // and send empty RTP packets per RFC 4103.
    fFrameSize = 0;
  } else {
    size_t len = text.size();
    if (len > fMaxSize) {
      len = fMaxSize;
      fNumTruncatedBytes = (unsigned)(text.size() - len);
    } else {
      fNumTruncatedBytes = 0;
    }
    memcpy(fTo, text.c_str(), len);
    fFrameSize = (unsigned)len;
  }

  gettimeofday(&fPresentationTime, nullptr);
  fDurationInMicroseconds = 0;

  afterGetting(this);
}
