#include "IMPAudioServerMediaSubsession.hpp"
#if defined(USE_AAC) && USE_AAC
#include "AACSink.hpp"
#endif
#include "GroupsockHelper.hh"
#include "IMPAudio.hpp"
#include "IMPDeviceSource.hpp"
#include "SimpleRTPSink.hh"
#include "globals.hpp"
#include "liveMedia.hh"

IMPAudioServerMediaSubsession *
IMPAudioServerMediaSubsession::createNew(UsageEnvironment &env, int audioChn) {
  return new IMPAudioServerMediaSubsession(env, audioChn);
}

IMPAudioServerMediaSubsession::IMPAudioServerMediaSubsession(
    UsageEnvironment &env, int audioChn)
    : OnDemandServerMediaSubsession(env, false), audioChn(audioChn) {
  LOG_INFO("IMPAudioServerMediaSubsession init");
}

IMPAudioServerMediaSubsession::~IMPAudioServerMediaSubsession() {
}

#if defined(USE_AUDIO_STREAM_REPLICATOR)
FramedSource *
IMPAudioServerMediaSubsession::createNewStreamSource(unsigned clientSessionId,
                                                     unsigned &estBitrate) {
  std::shared_ptr<audio_stream> audioStream = global_audio[audioChn];
  IMPAudio *impAudio = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_main);
    if (audioStream) {
      impAudio = audioStream->imp_audio;
    }
  }
  if (!audioStream || !impAudio) {
    LOG_WARN("Audio stream source requested while audio engine is unavailable");
    estBitrate = 0;
    return nullptr;
  }

  estBitrate = impAudio->bitrate;
  auto *replicator = global_audio[audioChn]->streamReplicator;
  if (!replicator) {
    return nullptr;
  }

  FramedSource *audioSourceReplica = replicator->createStreamReplica();
  if (audioSourceReplica) {
    global_audio[audioChn]->rtsp_client_count.fetch_add(
        1, std::memory_order_relaxed);
    global_audio[audioChn]->hasDataCallback = true;
    global_audio[audioChn]->should_grab_frames.notify_one();
  }
  return audioSourceReplica;
}
#else
FramedSource *
IMPAudioServerMediaSubsession::createNewStreamSource(unsigned clientSessionId,
                                                     unsigned &estBitrate) {
  std::shared_ptr<audio_stream> audioStream = global_audio[audioChn];
  IMPAudio *impAudio = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_main);
    if (audioStream) {
      impAudio = audioStream->imp_audio;
    }
  }
  if (!audioStream || !impAudio) {
    LOG_WARN("Audio stream source requested while audio engine is unavailable");
    estBitrate = 0;
    return nullptr;
  }

  estBitrate = impAudio->bitrate;
  IMPDeviceSource<AudioFrame, audio_stream> *audioSource =
      IMPDeviceSource<AudioFrame, audio_stream>::createNew(
          envir(), audioChn, global_audio[audioChn], "audio", false,
          clientSessionId);

  if (impAudio->format == IMPAudioFormat::PCM)
    return EndianSwap16::createNew(envir(), audioSource);

  return audioSource;
}
#endif

void IMPAudioServerMediaSubsession::closeStreamSource(
    FramedSource *inputSource) {
#if defined(USE_AUDIO_STREAM_REPLICATOR)
  auto audioStream = global_audio[audioChn];
  if (inputSource && audioStream) {
    int previous =
        audioStream->rtsp_client_count.fetch_sub(1, std::memory_order_relaxed);
    if (previous <= 1) {
      audioStream->rtsp_client_count.store(0, std::memory_order_relaxed);
      audioStream->hasDataCallback = false;
    }
  }
#endif
  OnDemandServerMediaSubsession::closeStreamSource(inputSource);
}

RTPSink *IMPAudioServerMediaSubsession::createNewRTPSink(
    Groupsock *rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic,
    FramedSource *inputSource) {
  (void)inputSource;
  IMPAudio *impAudio = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_main);
    if (global_audio[audioChn]) {
      impAudio = global_audio[audioChn]->imp_audio;
    }
  }
  if (!impAudio) {
    LOG_WARN("RTPSink requested while audio engine is unavailable");
    return nullptr;
  }

  unsigned rtpPayloadFormat = rtpPayloadTypeIfDynamic;
  unsigned rtpTimestampFrequency = impAudio->sample_rate;
  const char *rtpPayloadFormatName = "L16";
  bool allowMultipleFramesPerPacket = true;
  int outChnCnt = cfg->audio.force_stereo ? 2 : 1;
  switch (impAudio->format) {
  case IMPAudioFormat::PCM:
    break;
  case IMPAudioFormat::G711A:
    rtpPayloadFormat = 8;
    rtpPayloadFormatName = "PCMA";
    break;
  case IMPAudioFormat::G711U:
    rtpPayloadFormat = 0;
    rtpPayloadFormatName = "PCMU";
    break;
  case IMPAudioFormat::G726:
    rtpPayloadFormatName = "G726-16";
    break;
#if defined(USE_OPUS) && USE_OPUS
  case IMPAudioFormat::OPUS:
    // Opus in RTP MUST advertise 48 kHz clock and 2 channels in SDP (rtpmap)
    rtpTimestampFrequency = 48000;
    rtpPayloadFormatName = "OPUS";
    allowMultipleFramesPerPacket = false;
    outChnCnt = 2; // always advertise stereo in SDP
    break;
#endif
#if defined(USE_AAC) && USE_AAC
  case IMPAudioFormat::AAC: {
    RTPSink *sink = AACSink::createNew(envir(), rtpGroupsock, rtpPayloadFormat,
                                       rtpTimestampFrequency,
                                       /* numChannels */ outChnCnt);
    if (sink != nullptr) {
      sink->enableRTCPReports() = False;
    }
    return sink;
  }
#endif
  }

  LOG_DEBUG("createNewRTPSink: " << rtpPayloadFormatName << ", "
                                 << rtpTimestampFrequency);

  RTPSink *sink = SimpleRTPSink::createNew(
      envir(), rtpGroupsock, rtpPayloadFormat, rtpTimestampFrequency,
      /* sdpMediaTypeString*/ "audio", rtpPayloadFormatName,
      /* numChannels */ outChnCnt, allowMultipleFramesPerPacket);
  if (sink != nullptr) {
    sink->enableRTCPReports() = False;
  }
  return sink;
}
