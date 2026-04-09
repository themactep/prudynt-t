#include "RTSP.hpp"
#include "BackchannelServerMediaSubsession.hpp"
#include "H264TimingPatch.hpp"
#include "IMPBackchannel.hpp"
#include "PrudyntRTSPServer.hpp"
#include <stdlib.h>

#undef MODULE
#define MODULE "RTSP"

/**
 * Wait for SPS/PPS (and VPS for H265) to be populated in the parameter cache
 * by VideoWorker, then copy them out.  Returns true on success.
 *
 * This replaces the old approach of consuming frames from msgChannel which
 * silently dropped live frames during RTSP client setup.
 */
static bool wait_for_parameter_sets(int chnNr, bool is_h265,
                                    H264NALUnit &sps_out, H264NALUnit &pps_out,
                                    H264NALUnit *&vps_out) {
  using namespace std::chrono_literals;
  LOG_DEBUG("wait_for_parameter_sets: entering for ch" << chnNr << " h265=" << is_h265);

  auto *vs = global_video[chnNr].get();
  if (!vs) {
    LOG_ERROR("wait_for_parameter_sets: global_video[" << chnNr << "] is null!");
    return false;
  }

  std::unique_lock<std::mutex> lock(vs->parameterCache.mutex);
  LOG_DEBUG("wait_for_parameter_sets: ch" << chnNr << " lock acquired, have_sps="
            << vs->parameterCache.have_sps << " have_pps=" << vs->parameterCache.have_pps);

  const auto ready = [&] {
    if (!vs->parameterCache.have_sps || !vs->parameterCache.have_pps)
      return false;
    if (is_h265 && !vs->parameterCache.have_vps)
      return false;
    return true;
  };

  // Wait up to 10 s; VideoWorker normally delivers parameter sets within one
  // GOP (< 2 s at 25 fps with keyint 50).
  if (!vs->parameterCache.cv.wait_for(lock, 10s, ready)) {
    LOG_ERROR("Timed out waiting for SPS/PPS for stream " << chnNr);
    return false;
  }

  sps_out = vs->parameterCache.sps;
  pps_out = vs->parameterCache.pps;
  LOG_DEBUG("wait_for_parameter_sets: ch" << chnNr << " got sps.size=" << sps_out.data.size()
            << " pps.size=" << pps_out.data.size());
  if (is_h265) {
    if (!vps_out)
      vps_out = new H264NALUnit;
    *vps_out = vs->parameterCache.vps;
    LOG_DEBUG("wait_for_parameter_sets: ch" << chnNr << " vps.size=" << vps_out->data.size());
  }
  LOG_DEBUG("wait_for_parameter_sets: ch" << chnNr << " returning true");
  return true;
}

void RTSP::addSubsession(int chnNr, _stream &stream) {
  LOG_DEBUG("identify stream " << chnNr);

  ServerMediaSession *sms = ServerMediaSession::createNew(*env, stream.rtsp_endpoint, stream.rtsp_info, cfg->rtsp.name);

  // Add video subsession if enabled
  if (stream.video_enabled) {
    H264NALUnit sps;
    H264NALUnit pps;
    H264NALUnit *vps = nullptr;
    bool is_h265 = strcmp(stream.format, "H265") == 0;

    // Wait for the parameter cache to be populated by VideoWorker.
    // Unlike the old msgChannel->wait_read() loop this does NOT consume any
    // live frames — they remain available for RTSP delivery.
    if (!wait_for_parameter_sets(chnNr, is_h265, sps, pps, vps)) {
      LOG_ERROR("Could not obtain SPS/PPS for stream " << chnNr << " — skipping subsession");
      if (vps) { delete vps; vps = nullptr; }
    } else {
      LOG_DEBUG("Got necessary NAL Units from parameter cache.");

      // Patch H264 SPS to include timing_info_present_flag so decoders can
      // determine frame rate without relying on RTSP SDP signaling. This
      // prevents negative DTS/PTS at startup with ffprobe, ffmpeg, and VLC.
      if (!is_h265 && stream.fps > 0) {
        LOG_DEBUG("Applying H264 SPS timing patch for ch" << chnNr << " sps.size=" << sps.data.size() << " fps=" << stream.fps);
        if (!patch_h264_sps_timing(sps.data, stream.fps)) {
          LOG_WARN("H264 SPS timing patch failed for stream " << chnNr << " — using unpatched SPS");
        } else {
          LOG_DEBUG("H264 SPS timing patch applied (fps=" << stream.fps << ") for stream " << chnNr);
        }
      }

      LOG_DEBUG("Creating IMPServerMediaSubsession for ch" << chnNr
                << " sps.size=" << sps.data.size() << " pps.size=" << pps.data.size());
      IMPServerMediaSubsession *sub =
          IMPServerMediaSubsession::createNew(*env, (is_h265 ? vps : nullptr), sps, pps, chnNr);
      sms->addSubsession(sub);
      LOG_INFO("Video stream " << chnNr << " added to session");
    }
  }

  if (cfg->audio.input_enabled && stream.audio_enabled) {
    IMPAudioServerMediaSubsession *audioSub = IMPAudioServerMediaSubsession::createNew(*env, 0);
    sms->addSubsession(audioSub);
    LOG_INFO("Audio stream " << chnNr << " added to session");
  }

  // ONVIF backchannel: add backchannel subsessions to the primary stream (ch0)
  // with a require tag so they only appear in the SDP when the client sends
  // "Require: www.onvif.org/ver20/backchannel" in the DESCRIBE request.
  // Per ONVIF Streaming Spec Section 5.3, backchannel tracks must be part of
  // the main media session, not a separate endpoint.
  if (chnNr == 0 && cfg->audio.output_enabled) {
#define ADD_BC_SUBSESSION_CH0(EnumName, NameString, PayloadType, Frequency, MimeType)                                  \
  {                                                                                                                    \
    BackchannelServerMediaSubsession *bcSub =                                                                          \
        BackchannelServerMediaSubsession::createNew(*env, IMPBackchannelFormat::EnumName);                             \
    bcSub->setRequireTag("www.onvif.org/ver20/backchannel");                                                           \
    sms->addSubsession(bcSub);                                                                                         \
    LOG_INFO("Backchannel (" << NameString << ") added to ch0 (conditional on Require header)");                       \
  }

    X_FOREACH_BACKCHANNEL_FORMAT(ADD_BC_SUBSESSION_CH0)
#undef ADD_BC_SUBSESSION_CH0
  }

  rtspServer->addServerMediaSession(sms);

  char *url = rtspServer->rtspURL(sms);
  LOG_INFO("stream " << chnNr << " available at: " << url);
}

void RTSP::start() {
  scheduler = BasicTaskScheduler::createNew();
  env = BasicUsageEnvironment::createNew(*scheduler);

  if (cfg->rtsp.auth_required) {
    UserAuthenticationDatabase *auth = new UserAuthenticationDatabase;
    auth->addUserRecord(cfg->rtsp.username, cfg->rtsp.password);
    rtspServer = PrudyntRTSPServer::createNew(*env, cfg->rtsp.port, auth, cfg->rtsp.session_reclaim);
  } else {
    rtspServer = PrudyntRTSPServer::createNew(*env, cfg->rtsp.port, nullptr, cfg->rtsp.session_reclaim);
  }
  if (rtspServer == NULL) {
    LOG_ERROR("Failed to create RTSP server: " << env->getResultMsg() << "\n");
    return;
  }
  OutPacketBuffer::maxSize = cfg->rtsp.out_buffer_size;

  // Let each RTP sink keep its own timestamp base. Sharing one numeric base
  // across different RTP clock domains (for example 90 kHz video and 16 kHz audio)
  // is not required for A/V sync and can confuse downstream demuxers.
  unsetenv("PRUDYNT_SHARED_TIMESTAMP_BASE");

#if defined(USE_AUDIO_STREAM_REPLICATOR)
  if (cfg->audio.input_enabled) {
    audioSource = IMPDeviceSource<AudioFrame, audio_stream>::createNew(*env, 0, global_audio[audioChn], "audio");

    if (global_audio[audioChn]->imp_audio->format == IMPAudioFormat::PCM)
      audioSource = (IMPDeviceSource<AudioFrame, audio_stream> *)EndianSwap16::createNew(*env, audioSource);

    global_audio[audioChn]->streamReplicator = StreamReplicator::createNew(*env, audioSource, false);

    // The replicator stays alive even when no RTSP clients are connected, so
    // keep the audio capture active. When RTSP clients connect via
    // IMPAudioServerMediaSubsession, hasDataCallback will be managed by the
    // sessionsubsession's createNewStreamSource/closeStreamSource.
    global_audio[audioChn]->rtsp_client_count.store(0, std::memory_order_relaxed);
  }
#endif

  if (cfg->stream0.enabled) {
    addSubsession(0, cfg->stream0);
  }

  if (cfg->stream1.enabled) {
    addSubsession(1, cfg->stream1);
  }

  // Optional audio-only RTSP session (microphone only, no video/backchannel)
  if (cfg->audio.input_enabled && cfg->rtsp.audio_only_enabled) {
    ServerMediaSession *sms =
        ServerMediaSession::createNew(*env, cfg->rtsp.audio_only_endpoint, cfg->rtsp.audio_only_info, cfg->rtsp.name);
    IMPAudioServerMediaSubsession *audioSub = IMPAudioServerMediaSubsession::createNew(*env, 0);
    sms->addSubsession(audioSub);
    rtspServer->addServerMediaSession(sms);
    char *url = rtspServer->rtspURL(sms);
    LOG_INFO("Audio-only stream available at: " << url);
  }

  global_rtsp_thread_signal = 0;
  env->taskScheduler().doEventLoop(&global_rtsp_thread_signal);

#if defined(USE_AUDIO_STREAM_REPLICATOR)
  if (cfg->audio.input_enabled) {
    if (global_audio[audioChn]->streamReplicator != nullptr) {
      global_audio[audioChn]->streamReplicator->detachInputSource();
    }
    if (audioSource != nullptr) {
      delete audioSource;
      audioSource = nullptr;
    }
  }
#endif

  // Clean up VPS if it was allocated
  /*
  if (vps) {
      delete vps;
      vps = nullptr;
  }
  */

  LOG_DEBUG("Stop RTSP Server.");

  // Cleanup RTSP server and environment
  Medium::close(rtspServer);
  env->reclaim();
  delete scheduler;
}

void *RTSP::run(void *arg) {
  ((RTSP *)arg)->start();
  return nullptr;
}
