#include "audio/IMPAudio.hpp"
#if defined(USE_AAC) && USE_AAC
#include "audio/codec/AACEncoder.hpp"
#endif
#include "config/Config.hpp"
#if defined(USE_OPUS) && USE_OPUS
#include "audio/codec/Opus.hpp"
#endif
#include "isp/imp_hal.hpp"
#include <cctype>
#include <cerrno>
#include <thread>

#define MODULE "IMPAUDIO"

static thread_local IMPAudioEncoder *encoder = nullptr;

int IMPAudio::encodeDirect(IMPAudioFrame *frame, unsigned char *outbuf,
                           int *outLen) {
  if (!encoder)
    return -1;
  return encoder->encode(frame, outbuf, outLen);
}

#if defined(USE_AAC) && USE_AAC
bool IMPAudio::isAACEncoder() {
  return dynamic_cast<AACEncoder *>(encoder) != nullptr;
}

int IMPAudio::getAACFrameSamples() {
  auto *aac = dynamic_cast<AACEncoder *>(encoder);
  return aac ? aac->getFrameSamples() : 0;
}

int64_t IMPAudio::getAACLastPtsUs() {
  auto *aac = dynamic_cast<AACEncoder *>(encoder);
  return aac ? aac->getLastFramePtsUs() : 0;
}

const uint8_t *IMPAudio::getAACAsc(uint32_t &len) {
  auto *aac = dynamic_cast<AACEncoder *>(encoder);
  if (aac && aac->getAscLen() > 0) {
    len = aac->getAscLen();
    return aac->getAsc();
  }
  len = 0;
  return nullptr;
}
#else
bool IMPAudio::isAACEncoder() { return false; }
int IMPAudio::getAACFrameSamples() { return 0; }
int64_t IMPAudio::getAACLastPtsUs() { return 0; }
const uint8_t *IMPAudio::getAACAsc(uint32_t &len) { len = 0; return nullptr; }
#endif

IMPAudio *IMPAudio::createNew(int devId, int inChn, int aeChn) {
  return new IMPAudio(devId, inChn, aeChn);
}

int IMPAudio::init() {
  LOG_DEBUG("IMPAudio::init()");
  int ret;

  format = IMPAudioFormat::PCM;
  IMPAudioIOAttr ioattr = {.samplerate = static_cast<IMPAudioSampleRate>(
                               cfg->audio.mic_hq ? AUDIO_SAMPLE_RATE_48000
                                                        : AUDIO_SAMPLE_RATE_16000),
                           .bitwidth = AUDIO_BIT_WIDTH_16,
                           .soundmode = AUDIO_SOUND_MODE_MONO,
                           .frmNum = 30,
                           .numPerFrm = 0,
                           .chnCnt = 1};
  IMPAudioEncChnAttr encattr = {
      .type = IMPAudioPalyloadType::PT_PCM, .bufSize = 20, .value = 0};
  float frameDuration = 0.040;

  // Berechne PCM Bitrate basierend auf outChnCnt
  bitrate = (int)ioattr.bitwidth * (int)ioattr.samplerate * outChnCnt / 1000;

  // output channel count is 2 if stereo is enabled
  outChnCnt = cfg->audio.force_stereo ? 2 : 1;

  if (strcmp(cfg->audio.input_format, "OPUS") == 0) {
#if defined(USE_OPUS) && USE_OPUS
    format = IMPAudioFormat::OPUS;
    bitrate = cfg->audio.mic_bitrate_kbps();
    encoder = Opus::createNew(ioattr.samplerate, outChnCnt);
#else
    LOG_ERROR("OPUS input_format requested but OPUS support is disabled at "
              "build time.");
#endif
  } else if (strcmp(cfg->audio.input_format, "AAC") == 0) {
#if defined(USE_AAC) && USE_AAC
    format = IMPAudioFormat::AAC;
    bitrate = cfg->audio.mic_bitrate_kbps();
    encoder = AACEncoder::createNew(ioattr.samplerate, outChnCnt);
#else
    LOG_ERROR("AAC input_format requested but AAC support is disabled at build "
              "time.");
#endif
  } else if (strcmp(cfg->audio.input_format, "G711A") == 0) {
    outChnCnt = 1; // G711A is mono
    format = IMPAudioFormat::G711A;
    encattr.type = IMPAudioPalyloadType::PT_G711A;
    ioattr.samplerate = AUDIO_SAMPLE_RATE_8000;
    bitrate = ioattr.bitwidth / 2 * ioattr.samplerate / 1000;
  } else if (strcmp(cfg->audio.input_format, "G711U") == 0) {
    outChnCnt = 1; // G711U is mono
    format = IMPAudioFormat::G711U;
    encattr.type = IMPAudioPalyloadType::PT_G711U;
    ioattr.samplerate = AUDIO_SAMPLE_RATE_8000;
    bitrate = ioattr.bitwidth / 2 * ioattr.samplerate / 1000;
  } else if (strcmp(cfg->audio.input_format, "G726") == 0) {
    outChnCnt = 1; // G726 is mono
    format = IMPAudioFormat::G726;
    encattr.type = IMPAudioPalyloadType::PT_G726;
    ioattr.samplerate = AUDIO_SAMPLE_RATE_8000;
    bitrate = 16;
  } else if (strcmp(cfg->audio.input_format, "PCM") == 0) {
    // PCM format - keep the default format = IMPAudioFormat::PCM set above
    LOG_INFO("Using PCM format (no encoding)");
  } else {
    LOG_ERROR("unsupported audio->input_format ("
              << cfg->audio.input_format
              << "). we only support OPUS, AAC, G711A, G711U, G726, and PCM.");
  }

  sample_rate = ioattr.samplerate;

  if (encattr.type > IMPAudioPalyloadType::PT_PCM) {
#if !defined(OPENIMP)
    ret = IMP_AENC_CreateChn(aeChn, &encattr);
    LOG_DEBUG_OR_ERROR(ret, "IMP_AENC_CreateChn(" << aeChn << ", &encattr)");
#else
    LOG_WARN("OpenIMP build: IMP AENC is unavailable; "
             << cfg->audio.input_format
             << " audio input will be captured as raw PCM");
#endif
  }

  // FAAC encodes fixed 1024-sample frames.  The HAL requires numPerFrm to be
  // a multiple of 10 ms of audio, so pick the 10 ms multiple closest to the
  // 1024-sample window: 960 samples (20 ms @ 48 kHz, 60 ms @ 16 kHz) keeps
  // delivery timing regular at whatever rate mic_hq selects.
  if (format == IMPAudioFormat::AAC)
    frameDuration =
        0.010f * static_cast<int>(1024.0f / (0.010f * static_cast<float>(
                                                     ioattr.samplerate)) +
                                  0.5f);

  ioattr.numPerFrm = (int)ioattr.samplerate * frameDuration;
  ret = IMP_AI_SetPubAttr(devId, &ioattr);
  LOG_DEBUG_OR_ERROR(ret, "IMP_AI_SetPubAttr(" << devId << ")");

  memset(&ioattr, 0x0, sizeof(ioattr));
  ret = IMP_AI_GetPubAttr(devId, &ioattr);
  LOG_DEBUG_OR_ERROR(ret, "IMP_AI_GetPubAttr(" << devId << ")");

  // After GetPubAttr, the HAL may have adjusted the sample rate.  Update
  // numPerFrm, the encoder's input rate and our own sample_rate so the SDP
  // clock, AAC ASC, MP4 PTS and tap file all use the actual HAL rate.
  ioattr.numPerFrm = (int)ioattr.samplerate * frameDuration;
  if (ioattr.samplerate > 0)
    sample_rate = ioattr.samplerate;
  if (encoder) {
    int actualRate = static_cast<int>(ioattr.samplerate);
    LOG_DEBUG("Actual HAL sample rate: " << actualRate << " Hz");
    encoder->setInputRate(actualRate);
  }

  if (encoder) {
    // Custom encoders (AAC/OPUS): bypass IMP_AENC entirely.
    // IMP_AENC's internal ring buffer corrupts adjacent heap allocations
    // (including the encoder handle) due to a bug in its buffer management.
    // We open the encoder directly and call it from AudioWorker.
    directEncode = true;
    ret = encoder->open();
    if (ret != 0) {
      LOG_ERROR("Failed to open " << cfg->audio.input_format
                                   << " encoder directly");
      return ret;
    }
    LOG_DEBUG("Opened " << cfg->audio.input_format
                        << " encoder directly (bypassing IMP_AENC)");
  }

  ret = IMP_AI_Enable(devId);
  LOG_DEBUG_OR_ERROR(ret, "IMP_AI_Enable(" << devId << ")");

  IMPAudioIChnParam chnParam{};
  hal::audio::init_ai_channel_param(chnParam);

  ret = IMP_AI_SetChnParam(devId, inChn, &chnParam);
  if (ret != 0) {
    int err = errno;
    LOG_ERROR("IMP_AI_SetChnParam("
              << devId << ", " << inChn << ") failed: ret=" << ret
              << ", errno=" << err << " (" << strerror(err) << ")");
    return ret;
  } else {
    LOG_DEBUG("IMP_AI_SetChnParam(" << devId << ", " << inChn << ")");
  }

  memset(&chnParam, 0x0, sizeof(chnParam));
  ret = IMP_AI_GetChnParam(devId, inChn, &chnParam);
  if (ret != 0) {
    return ret;
  } else {
    LOG_DEBUG("IMP_AI_GetChnParam(" << devId << ", " << inChn << ")");
  }

  ret = IMP_AI_EnableChn(devId, inChn);
  if (ret != 0) {
    int err = errno;
    LOG_ERROR("IMP_AI_EnableChn("
              << devId << ", " << inChn << ") failed: ret=" << ret
              << ", errno=" << err << " (" << strerror(err) << ")");
    return ret;
  } else {
    LOG_DEBUG("IMP_AI_EnableChn(" << devId << ", " << inChn << ")");
  }
  ret = IMP_AI_SetVol(devId, inChn, cfg->audio.input_vol);
  LOG_DEBUG_OR_ERROR(ret, "IMP_AI_SetVol(" << devId << ", " << inChn << ", "
                                           << cfg->audio.input_vol << ")");

  int vol;
  ret = IMP_AI_GetVol(devId, inChn, &vol);
  LOG_DEBUG_OR_ERROR(ret,
                     "IMP_AI_GetVol(" << devId << ", " << inChn << ", &vol)");

  if (cfg->audio.input_gain >= 0) {
    ret = IMP_AI_SetGain(devId, inChn, cfg->audio.input_gain);
    LOG_DEBUG_OR_ERROR(ret, "IMP_AI_SetGain(" << devId << ", " << inChn << ", "
                                              << cfg->audio.input_gain << ")");
  }

  int gain;
  ret = IMP_AI_GetGain(devId, inChn, &gain);
  LOG_DEBUG_OR_ERROR(ret,
                     "IMP_AI_GetGain(" << devId << ", " << inChn << ", &gain)");

  LOG_INFO("Audio In: format:"
           << cfg->audio.input_format << ", vol:" << vol << ", gain:" << gain
           << ", samplerate:" << ioattr.samplerate
           << ", bitwidth:" << ioattr.bitwidth
           << ", soundmode:" << ioattr.soundmode << ", frmNum:" << ioattr.frmNum
           << ", numPerFrm:" << ioattr.numPerFrm << ", chnCnt:" << ioattr.chnCnt
           << ", usrFrmDepth:" << chnParam.usrFrmDepth);

#if defined(LIB_AUDIO_PROCESSING)
  if (cfg->audio.input_noise_suppression) {
    ret = IMP_AI_EnableNs(&ioattr, cfg->audio.input_noise_suppression);
    LOG_DEBUG_OR_ERROR(ret, "IMP_AI_EnableNs(&ioattr, "
                                << cfg->audio.input_noise_suppression << ")");
    enabledNs = true;
  }

  if (cfg->audio.input_high_pass_filter) {
    ret = IMP_AI_EnableHpf(&ioattr);
    LOG_DEBUG_OR_ERROR(ret, "IMP_AI_EnableHpf(&ioattr)");
    enabledHpf = true;
  }

  if (cfg->audio.input_agc_enabled && hal::caps().has_audio_agc) {
    IMPAudioAgcConfig agcConfig = {
        /**< Gain level, with a range of [0, 31]. This represents the target
        volume level, measured in dB (decibels), and is a negative value. The
        smaller the value, the higher the volume. */
        .TargetLevelDbfs = cfg->audio.input_agc_target_level_dbfs,
        /**< Sets the maximum gain value, with a range of [0, 90]. 0 means no
        gain, and the larger the value, the higher the gain. */
        .CompressionGaindB = cfg->audio.input_agc_compression_gain_db,
    };
    /* Enable automatic gain control on platforms that advertise it. */
    ret = IMP_AI_EnableAgc(&ioattr, agcConfig);
    LOG_DEBUG_OR_ERROR(ret, "IMP_AI_EnableAgc({"
                                << agcConfig.TargetLevelDbfs << ", "
                                << agcConfig.CompressionGaindB << "})");
    enabledAgc = true;
  }
#if defined(PLATFORM_T21) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
  if (cfg->audio.input_alc_gain > 0 && hal::caps().has_audio_alc) {
    ret = IMP_AI_SetAlcGain(devId, inChn, cfg->audio.input_alc_gain);
    LOG_DEBUG_OR_ERROR(ret, "IMP_AI_SetAlcGain("
                                << devId << ", " << inChn << ", "
                                << cfg->audio.input_alc_gain << ")");
  }
#endif
#endif // LIB_AUDIO_PROCESSING
  return 0;
}

int IMPAudio::deinit() {
  LOG_DEBUG("IMPAudio::deinit()");
  int ret;

  if (enabledNs) {
    ret = IMP_AI_DisableNs();
    LOG_DEBUG_OR_ERROR(ret, "IMP_AI_DisableNs()");
    enabledNs = false;
  }

  if (enabledHpf) {
    ret = IMP_AI_DisableHpf();
    LOG_DEBUG_OR_ERROR(ret, "IMP_AI_DisableHpf()");
    enabledHpf = false;
  }

  if (enabledAgc) {
    ret = IMP_AI_DisableAgc();
    LOG_DEBUG_OR_ERROR(ret, "IMP_AI_DisableAgc()");
    enabledAgc = false;
  }

  if (encoder) {
    if (directEncode) {
      encoder->close();
    } else {
#if !defined(OPENIMP)
      ret = IMP_AENC_UnRegisterEncoder(&handle);
      LOG_DEBUG_OR_ERROR(ret, "IMP_AENC_UnRegisterEncoder(&handle)");
#endif
    }

    delete encoder;
    encoder = nullptr;
    handle = 0;
  }

  if (!directEncode && format != IMPAudioFormat::PCM) {
#if !defined(OPENIMP)
    ret = IMP_AENC_DestroyChn(aeChn);
    LOG_DEBUG_OR_ERROR(ret, "IMP_AENC_DestroyChn(" << aeChn << ")");
#endif
  }

  ret = IMP_AI_DisableChn(devId, inChn);
  LOG_DEBUG_OR_ERROR(ret,
                     "IMP_AI_DisableChn(" << devId << ", " << inChn << ")");

  ret = IMP_AI_Disable(devId);
  LOG_DEBUG_OR_ERROR(ret, "IMP_AI_Disable(" << devId << ")");

  return 0;
}
