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

IMPAudioServerMediaSubsession *IMPAudioServerMediaSubsession::createNew(UsageEnvironment &env, int audioChn) {
  return new IMPAudioServerMediaSubsession(env, audioChn);
}

IMPAudioServerMediaSubsession::IMPAudioServerMediaSubsession(UsageEnvironment &env, int audioChn)
    : OnDemandServerMediaSubsession(env, true), audioChn(audioChn) {
  LOG_INFO("IMPAudioServerMediaSubsession init");
}

IMPAudioServerMediaSubsession::~IMPAudioServerMediaSubsession() {
}

#if defined(USE_AUDIO_STREAM_REPLICATOR)
FramedSource *IMPAudioServerMediaSubsession::createNewStreamSource(unsigned clientSessionId, unsigned &estBitrate) {
  estBitrate = global_audio[audioChn]->imp_audio->bitrate;
  auto *replicator = global_audio[audioChn]->streamReplicator;
  if (!replicator) {
    return nullptr;
  }

  FramedSource *audioSourceReplica = replicator->createStreamReplica();
  if (audioSourceReplica) {
    global_audio[audioChn]->rtsp_client_count.fetch_add(1, std::memory_order_relaxed);
    global_audio[audioChn]->hasDataCallback = true;
    global_audio[audioChn]->should_grab_frames.notify_one();
  }
  return audioSourceReplica;
}
#else
FramedSource *IMPAudioServerMediaSubsession::createNewStreamSource(unsigned clientSessionId, unsigned &estBitrate) {
  estBitrate = global_audio[audioChn]->imp_audio->bitrate;
  IMPDeviceSource<AudioFrame, audio_stream> *audioSource =
      IMPDeviceSource<AudioFrame, audio_stream>::createNew(envir(), audioChn, global_audio[audioChn], "audio");

  if (global_audio[audioChn]->imp_audio->format == IMPAudioFormat::PCM)
    return EndianSwap16::createNew(envir(), audioSource);

  return audioSource;
}
#endif

void IMPAudioServerMediaSubsession::closeStreamSource(FramedSource *inputSource) {
#if defined(USE_AUDIO_STREAM_REPLICATOR)
  if (inputSource) {
    int previous = global_audio[audioChn]->rtsp_client_count.fetch_sub(1, std::memory_order_relaxed);
    if (previous <= 1) {
      global_audio[audioChn]->rtsp_client_count.store(0, std::memory_order_relaxed);
      global_audio[audioChn]->hasDataCallback = false;
    }
  }
#endif
  OnDemandServerMediaSubsession::closeStreamSource(inputSource);
}

RTPSink *IMPAudioServerMediaSubsession::createNewRTPSink(Groupsock *rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic,
                                                         FramedSource *inputSource) {
  unsigned rtpPayloadFormat = rtpPayloadTypeIfDynamic;
  unsigned rtpTimestampFrequency = global_audio[audioChn]->imp_audio->sample_rate;
  const char *rtpPayloadFormatName = "L16";
  bool allowMultipleFramesPerPacket = true;
  int outChnCnt = cfg->audio.force_stereo ? 2 : 1;
  switch (global_audio[audioChn]->imp_audio->format) {
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
  case IMPAudioFormat::AAC:
    return AACSink::createNew(envir(), rtpGroupsock, rtpPayloadFormat, rtpTimestampFrequency,
                              /* numChannels */ outChnCnt);
#endif
  }

  LOG_DEBUG("createNewRTPSink: " << rtpPayloadFormatName << ", " << rtpTimestampFrequency);

  return SimpleRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadFormat, rtpTimestampFrequency,
                                  /* sdpMediaTypeString*/ "audio", rtpPayloadFormatName,
                                  /* numChannels */ outChnCnt, allowMultipleFramesPerPacket);
}
