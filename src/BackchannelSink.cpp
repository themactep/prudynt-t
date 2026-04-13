#include "BackchannelSink.hpp"

#include "Logger.hpp"
#include "globals.hpp"

#define MODULE "BackchannelSink"

namespace {
constexpr unsigned BASE_TIMEOUT_US = 500000; // 0.5s default
constexpr unsigned MIN_TIMEOUT_US = 500000;  // Never below 0.5s
constexpr unsigned MAX_TIMEOUT_US = 4000000; // Cap at 4s
constexpr double EMA_ALPHA = 0.2;            // Weight for latest interval
constexpr double TIMEOUT_MULTIPLIER = 3.0;   // Wait ~3× observed interval
} // namespace

BackchannelSink *BackchannelSink::createNew(UsageEnvironment &env, unsigned clientSessionId,
                                            IMPBackchannelFormat format) {
  return new BackchannelSink(env, clientSessionId, format);
}

BackchannelSink::BackchannelSink(UsageEnvironment &env, unsigned clientSessionId, IMPBackchannelFormat format)
#if defined(USE_AAC) && USE_AAC
    : MediaSink(env), fRTPSource(nullptr), fReceiveBufferSize((format == IMPBackchannelFormat::AAC) ? 2048 : 1024),
#else
    : MediaSink(env), fRTPSource(nullptr), fReceiveBufferSize(1024),
#endif
      fIsActive(false), fAfterFunc(nullptr), fAfterClientData(nullptr), fClientSessionId(clientSessionId),
      fTimeoutTask(nullptr), fIsSending(false), fFormat(format), fCurrentTimeoutUs(BASE_TIMEOUT_US),
      fAvgInterFrameIntervalUs(static_cast<double>(BASE_TIMEOUT_US)), fHasLastPresentationTime(false) {
  fLastPresentationTime.tv_sec = 0;
  fLastPresentationTime.tv_usec = 0;
  fReceiveBuffer = new u_int8_t[fReceiveBufferSize];
  if (fReceiveBuffer == nullptr) {
    LOG_ERROR("Failed to allocate receive buffer (Session: " << static_cast<unsigned>(fClientSessionId) << ")");
  }
  resetAdaptiveTimeout();
}

BackchannelSink::~BackchannelSink() {
  stopPlaying();
  delete[] fReceiveBuffer;
}

Boolean BackchannelSink::startPlaying(FramedSource &source, MediaSink::afterPlayingFunc *afterFunc,
                                      void *afterClientData) {
  if (fIsActive) {
    LOG_WARN("startPlaying called while already active for session " << fClientSessionId);
    return False;
  }

  fRTPSource = &source;
  fAfterFunc = afterFunc;
  fAfterClientData = afterClientData;
  fIsActive = True;

  LOG_DEBUG("Sink starting consumption for session " << fClientSessionId);
  resetAdaptiveTimeout();

  return continuePlaying();
}

void BackchannelSink::stopPlaying() {
  if (!fIsActive) {
    return;
  }

  LOG_DEBUG("Sink stopping consumption for session " << fClientSessionId);

  // Set inactive *first* to prevent re-entrancy
  fIsActive = False;

  sendBackchannelStopFrame();

  envir().taskScheduler().unscheduleDelayedTask(fTimeoutTask);
  fTimeoutTask = nullptr;

  if (fRTPSource != nullptr) {
    fRTPSource->stopGettingFrames();
  }

  if (fAfterFunc != nullptr) {
    (*fAfterFunc)(fAfterClientData);
  }

  fRTPSource = nullptr;
  fAfterFunc = nullptr;
  fAfterClientData = nullptr;

  resetAdaptiveTimeout();
}

Boolean BackchannelSink::continuePlaying() {
  if (!fIsActive || fRTPSource == nullptr) {
    return False;
  }

  fRTPSource->getNextFrame(fReceiveBuffer, fReceiveBufferSize, afterGettingFrame, this, nullptr, this);

  return True;
}

void BackchannelSink::afterGettingFrame(void *clientData, unsigned frameSize, unsigned numTruncatedBytes,
                                        struct timeval presentationTime, unsigned /*durationInMicroseconds*/) {
  BackchannelSink *sink = static_cast<BackchannelSink *>(clientData);
  if (sink != nullptr) {
    sink->afterGettingFrame1(frameSize, numTruncatedBytes, presentationTime);
  } else {
    LOG_ERROR("afterGettingFrame called with invalid clientData");
  }
}

void BackchannelSink::afterGettingFrame1(unsigned frameSize, unsigned numTruncatedBytes,
                                         struct timeval presentationTime) {
  if (!fIsActive) {
    return;
  }

  if (numTruncatedBytes > 0) {
    LOG_WARN("Received truncated frame (" << frameSize << " bytes, " << numTruncatedBytes << " truncated) for session "
                                          << fClientSessionId << ". Discarding.");
  } else if (frameSize > 0) {
    sendBackchannelFrame(fReceiveBuffer, frameSize);
    updateAdaptiveTimeout(presentationTime);
  }

  // Reschedule the timeout check after receiving any frame (even size 0 or truncated)
  // This resets the timer as long as *something* is coming from the source.
  envir().taskScheduler().unscheduleDelayedTask(fTimeoutTask);
  scheduleTimeoutCheck();

  if (fIsActive) {
    continuePlaying();
  }
}

void BackchannelSink::scheduleTimeoutCheck() {
  fTimeoutTask = envir().taskScheduler().scheduleDelayedTask(fCurrentTimeoutUs, (TaskFunc *)timeoutCheck, this);
}

void BackchannelSink::timeoutCheck(void *clientData) {
  BackchannelSink *sink = static_cast<BackchannelSink *>(clientData);
  if (sink) {
    sink->timeoutCheck1();
  }
}

void BackchannelSink::timeoutCheck1() {
  fTimeoutTask = nullptr;

  if (!fIsActive) {
    return;
  }

  LOG_INFO("Audio data timeout detected for session " << fClientSessionId << ". Sending stop frame.");
  sendBackchannelStopFrame();
  resetAdaptiveTimeout();
}

void BackchannelSink::sendBackchannelFrame(const uint8_t *payload, unsigned payloadSize) {
  if (!global_backchannel) {
    LOG_ERROR("global_backchannel is null, cannot queue BackchannelFrame! (Session: "
              << static_cast<unsigned>(fClientSessionId) << ")");
    return;
  }

  if (!fIsSending) {
    fIsSending = true;
    global_backchannel->is_sending.fetch_add(1, std::memory_order_relaxed);
  }

  BackchannelFrame bcFrame;
  bcFrame.format = fFormat;
  bcFrame.clientSessionId = fClientSessionId;
  bcFrame.payload.assign(payload, payload + payloadSize);

  bool enqueued = global_backchannel->inputQueue->write(std::move(bcFrame));
  if (!enqueued) {
    LOG_WARN("Input queue full for session " << static_cast<unsigned>(fClientSessionId) << ". Frame dropped.");
  } else {
    global_backchannel->should_grab_frames.notify_one();
  }
}

void BackchannelSink::sendBackchannelStopFrame() {
  if (!fIsSending) {
    return;
  }

  if (global_backchannel) {
    BackchannelFrame stopFrame;
    stopFrame.format = fFormat;
    stopFrame.clientSessionId = fClientSessionId;
    stopFrame.payload.clear(); // Zero-size payload indicates stop/timeout
    bool enqueued = global_backchannel->inputQueue->write(std::move(stopFrame));
    if (!enqueued) {
      LOG_WARN("Input queue full when trying to send stop frame for session "
               << static_cast<unsigned>(fClientSessionId));
    } else {
      global_backchannel->should_grab_frames.notify_one();
      fIsSending = false;
      global_backchannel->is_sending.fetch_sub(1, std::memory_order_relaxed);
      LOG_INFO("Sent stop frame (zero-payload frame) for session " << static_cast<unsigned>(fClientSessionId));
    }
  } else {
    LOG_ERROR("global_backchannel is null, cannot send stop frame for session " << fClientSessionId);
  }
}

void BackchannelSink::resetAdaptiveTimeout() {
  fCurrentTimeoutUs = BASE_TIMEOUT_US;
  fAvgInterFrameIntervalUs = static_cast<double>(BASE_TIMEOUT_US);
  fHasLastPresentationTime = false;
  fLastPresentationTime.tv_sec = 0;
  fLastPresentationTime.tv_usec = 0;
}

uint64_t BackchannelSink::toMicroseconds(const struct timeval &tv) {
  return static_cast<uint64_t>(tv.tv_sec) * 1000000ULL + static_cast<uint64_t>(tv.tv_usec);
}

void BackchannelSink::updateAdaptiveTimeout(const struct timeval &presentationTime) {
  if (presentationTime.tv_sec == 0 && presentationTime.tv_usec == 0) {
    return;
  }

  if (fHasLastPresentationTime) {
    uint64_t previousUs = toMicroseconds(fLastPresentationTime);
    uint64_t currentUs = toMicroseconds(presentationTime);
    if (currentUs > previousUs) {
      double interval = static_cast<double>(currentUs - previousUs);
      // Exponential moving average of observed inter-frame interval
      fAvgInterFrameIntervalUs = (1.0 - EMA_ALPHA) * fAvgInterFrameIntervalUs + EMA_ALPHA * interval;
      double desiredTimeout = fAvgInterFrameIntervalUs * TIMEOUT_MULTIPLIER;
      if (desiredTimeout < static_cast<double>(MIN_TIMEOUT_US)) {
        desiredTimeout = static_cast<double>(MIN_TIMEOUT_US);
      } else if (desiredTimeout > static_cast<double>(MAX_TIMEOUT_US)) {
        desiredTimeout = static_cast<double>(MAX_TIMEOUT_US);
      }
      fCurrentTimeoutUs = static_cast<unsigned>(desiredTimeout);
    }
  } else {
    fHasLastPresentationTime = true;
  }

  fLastPresentationTime = presentationTime;
}
