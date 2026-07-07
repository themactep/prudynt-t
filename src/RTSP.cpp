#include "RTSP.hpp"
#include "BackchannelServerMediaSubsession.hpp"
#include "IMPBackchannel.hpp"
#include <chrono>

#undef MODULE
#define MODULE "RTSP"

namespace {

bool wait_for_parameter_sets(int chnNr, bool is_h265, H264NALUnit &sps_out,
                             H264NALUnit &pps_out, H264NALUnit *&vps_out) {
  using namespace std::chrono_literals;

  auto *video = global_video[chnNr].get();
  if (!video) {
    LOG_ERROR("wait_for_parameter_sets: missing video state for channel "
              << chnNr);
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + 10s;
  auto next_idr_retry = std::chrono::steady_clock::now();
  unsigned wait_timeout_count = 0;

  while (true) {
    {
      std::lock_guard<std::mutex> lock(video->codec_config_mutex);
      if (video->have_sps && video->have_pps && (!is_h265 || video->have_vps)) {
        sps_out.data = video->latest_sps;
        pps_out.data = video->latest_pps;
        if (is_h265) {
          if (!vps_out) {
            vps_out = new H264NALUnit;
          }
          vps_out->data = video->latest_vps;
        }
        return true;
      }
    }

    auto now = std::chrono::steady_clock::now();
    if (now >= next_idr_retry) {
      IMP_Encoder_RequestIDR(chnNr);
      next_idr_retry = now + 250ms;
    }

    if ((++wait_timeout_count % 4) == 0) {
      LOG_WARN("Still waiting for bootstrap parameter sets on stream "
               << chnNr << ", retrying IDR");
    }

    if (now >= deadline) {
      LOG_ERROR("Timed out waiting for SPS/PPS for stream " << chnNr);
      return false;
    }

    usleep(50 * 1000);
  }
}

} // namespace

void RTSP::addSubsession(int chnNr, _stream &stream) {
  LOG_DEBUG("identify stream " << chnNr);

  ServerMediaSession *sms = ServerMediaSession::createNew(
      *env, stream.rtsp_endpoint, stream.rtsp_info, cfg->rtsp.name);

  // Add video subsession (always on for stream0/stream1)
  {
    H264NALUnit sps;
    H264NALUnit pps;
    H264NALUnit *vps = nullptr;
    bool have_pps = false;
    bool have_sps = false;
    bool have_vps = false;
    bool is_h265 = strcmp(stream.format, "H265") == 0 ? true : false;

    global_video[chnNr]->bootstrap_requested.store(true,
                                                   std::memory_order_relaxed);
    global_video[chnNr]->should_grab_frames.notify_one();
    if (wait_for_parameter_sets(chnNr, is_h265, sps, pps, vps)) {
      have_sps = !sps.data.empty();
      have_pps = !pps.data.empty();
      have_vps = (vps != nullptr && !vps->data.empty());
    }
    global_video[chnNr]->bootstrap_requested.store(false,
                                                   std::memory_order_relaxed);
    global_video[chnNr]->should_grab_frames.notify_one();

    if (!have_sps || !have_pps || (is_h265 && !have_vps)) {
      LOG_ERROR("Could not obtain SPS/PPS for stream "
                << chnNr << " — skipping subsession");
      if (vps) {
        delete vps;
      }
      return;
    }

    LOG_DEBUG("Got necessary NAL Units.");

    IMPServerMediaSubsession *sub = IMPServerMediaSubsession::createNew(
        *env, (is_h265 ? vps : nullptr), sps, pps, chnNr // Conditional VPS
    );
    if (vps) {
      delete vps;
      vps = nullptr;
    }
    sms->addSubsession(sub);
    LOG_INFO("Video stream " << chnNr << " added to session");
  }

  if (cfg->audio.input_enabled && stream.audio_enabled) {
    IMPAudioServerMediaSubsession *audioSub =
        IMPAudioServerMediaSubsession::createNew(*env, 0);
    sms->addSubsession(audioSub);
    LOG_INFO("Audio stream " << chnNr << " added to session");
  }

  // Add OSD subtitle (T.140 text) track when OSD is enabled
  if (stream.osd.enabled &&
      global_video[chnNr] && global_video[chnNr]->imp_encoder &&
      global_video[chnNr]->imp_encoder->osd) {
    IMPTextServerMediaSubsession *textSub =
        IMPTextServerMediaSubsession::createNew(
            *env, global_video[chnNr]->imp_encoder->osd);
    sms->addSubsession(textSub);
    LOG_INFO("OSD subtitle (T.140) track added to stream " << chnNr);
  }

  // ONVIF backchannel: add backchannel subsessions to the primary stream (ch0)
  // with a require tag so they only appear in the SDP when the client sends
  // "Require: www.onvif.org/ver20/backchannel" in the DESCRIBE request.
  // Per ONVIF Streaming Spec Section 5.3, backchannel tracks must be part of
  // the main media session, not a separate endpoint.
  if (chnNr == 0 && cfg->audio.output_enabled) {
#define ADD_BC_SUBSESSION_CH0(EnumName, NameString, PayloadType, Frequency,    \
                              MimeType)                                        \
  {                                                                            \
    BackchannelServerMediaSubsession *bcSub =                                  \
        BackchannelServerMediaSubsession::createNew(                           \
            *env, IMPBackchannelFormat::EnumName);                             \
    bcSub->setRequireTag("www.onvif.org/ver20/backchannel");                   \
    sms->addSubsession(bcSub);                                                 \
    LOG_INFO("Backchannel ("                                                   \
             << NameString                                                     \
             << ") added to ch0 (conditional on Require header)");             \
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
    rtspServer = RTSPServer::createNew(*env, cfg->rtsp.port, auth,
                                       cfg->rtsp.session_reclaim);
  } else {
    rtspServer = RTSPServer::createNew(*env, cfg->rtsp.port, nullptr,
                                       cfg->rtsp.session_reclaim);
  }
  if (rtspServer == NULL) {
    LOG_ERROR("Failed to create RTSP server: " << env->getResultMsg() << "\n");
    return;
  }
  OutPacketBuffer::maxSize = cfg->rtsp.out_buffer_size;

#if defined(USE_AUDIO_STREAM_REPLICATOR)
  if (cfg->audio.input_enabled) {
    audioSource = IMPDeviceSource<AudioFrame, audio_stream>::createNew(
        *env, 0, global_audio[audioChn], "audio");

    if (global_audio[audioChn]->imp_audio->format == IMPAudioFormat::PCM)
      audioSource =
          (IMPDeviceSource<AudioFrame, audio_stream> *)EndianSwap16::createNew(
              *env, audioSource);

    global_audio[audioChn]->streamReplicator =
        StreamReplicator::createNew(*env, audioSource, false);

    // Keep the source object alive for the replicator, but do not start audio
    // capture until an actual RTSP client connects and requests frames.
    global_audio[audioChn]->hasDataCallback = false;
    global_audio[audioChn]->rtsp_client_count.store(0,
                                                    std::memory_order_relaxed);
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
    ServerMediaSession *sms = ServerMediaSession::createNew(
        *env, cfg->rtsp.audio_only_endpoint, cfg->rtsp.audio_only_info,
        cfg->rtsp.name);
    IMPAudioServerMediaSubsession *audioSub =
        IMPAudioServerMediaSubsession::createNew(*env, 0);
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
