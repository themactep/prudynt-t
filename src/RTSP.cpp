#include "RTSP.hpp"
#include "simple-rtsp/RtspServer.hpp"
#include "simple-rtsp/RtspTypes.hpp"
#include "Config.hpp"
#include "Logger.hpp"
#include "globals.hpp"
#include "version.hpp"
#include <cstring>
#include <string>

#undef MODULE
#define MODULE "RTSP"

RTSP::RTSP()
    : server_(std::make_unique<simple_rtsp::RtspServer>()) {
}

RTSP::~RTSP() {
    server_->stop();
}

void RTSP::addSubsession(int chnNr, _stream &stream) {
    // ── Video stream ───────────────────────────────────────────────────
    simple_rtsp::VideoStreamConfig vcfg;
    vcfg.name     = std::to_string(chnNr);
    vcfg.endpoint = stream.rtsp_endpoint;
    vcfg.codec    = stream.format ? stream.format : "H264";
    vcfg.width    = stream.width;
    vcfg.height   = stream.height;
    vcfg.fps      = stream.fps > 0 ? stream.fps : 30;
    vcfg.payloadType = 96;
    vcfg.clockRate   = 90000;

    // Copy current SPS/PPS/VPS from encoder state
    if (chnNr >= 0 && chnNr < NUM_VIDEO_CHANNELS && global_video[chnNr]) {
        auto &vs = global_video[chnNr];
        std::lock_guard<std::mutex> lock(vs->codec_config_mutex);
        if (vs->have_sps) vcfg.sps = vs->latest_sps;
        if (vs->have_pps) vcfg.pps = vs->latest_pps;
        if (vs->have_vps) vcfg.vps = vs->latest_vps;
        vcfg.haveCodecConfig = vs->have_sps && vs->have_pps;
    }

    // Bootstrap SPS/PPS if not yet available
    if (!vcfg.haveCodecConfig && chnNr < NUM_VIDEO_CHANNELS && global_video[chnNr]) {
        // Request IDR to get fresh codec config
        global_video[chnNr]->bootstrap_requested.store(true,
            std::memory_order_relaxed);
        global_video[chnNr]->should_grab_frames.notify_one();
        IMP_Encoder_RequestIDR(chnNr);

        // Wait up to 5 seconds for SPS/PPS
        for (int attempt = 0; attempt < 50; attempt++) {
            usleep(100000); // 100ms
            std::lock_guard<std::mutex> lock(
                global_video[chnNr]->codec_config_mutex);
            if (global_video[chnNr]->have_sps &&
                global_video[chnNr]->have_pps) {
                vcfg.sps = global_video[chnNr]->latest_sps;
                vcfg.pps = global_video[chnNr]->latest_pps;
                if (global_video[chnNr]->have_vps)
                    vcfg.vps = global_video[chnNr]->latest_vps;
                vcfg.haveCodecConfig = true;
                break;
            }
        }
        global_video[chnNr]->bootstrap_requested.store(false,
            std::memory_order_relaxed);
        global_video[chnNr]->should_grab_frames.notify_one();
    }

    server_->addVideoStream(chnNr, vcfg, global_video[chnNr]);

    // ── Audio stream ──────────────────────────────────────────────────
    if (!audioConfigured_ && cfg->audio.input_enabled && stream.audio_enabled
        && global_audio[0] && global_audio[0]->imp_audio) {
        simple_rtsp::AudioStreamConfig acfg;
        int hwRate = global_audio[0]->imp_audio->sample_rate;
        acfg.sampleRate = (hwRate > 0) ? hwRate : 8000;
        acfg.channels = 1;
        LOG_INFO("Audio hw sample_rate=" << hwRate << " -> using " << acfg.sampleRate);
        switch (global_audio[0]->imp_audio->format) {
        case IMPAudioFormat::G711U:
            acfg.codec = "PCMU";
            acfg.payloadType = 0;
            break;
        case IMPAudioFormat::G711A:
            acfg.codec = "PCMA";
            acfg.payloadType = 8;
            break;
        case IMPAudioFormat::AAC:
            acfg.codec = "AAC";
            acfg.payloadType = 97;
            break;
        case IMPAudioFormat::OPUS:
            acfg.codec = "OPUS";
            acfg.payloadType = 97;
            break;
        case IMPAudioFormat::PCM:
        default:
            // 16-bit LE PCM → L16 (network byte order requires swap)
            acfg.codec = "L16";
            acfg.payloadType = 97;
            break;
        }
        server_->addAudioStream(0, acfg, global_audio[0]);
        audioConfigured_ = true;
        LOG_INFO("Audio: " << acfg.codec << " " << acfg.sampleRate << "Hz pt="
                 << static_cast<int>(acfg.payloadType));
    }
}

void RTSP::start() {
    // ── Configure server ───────────────────────────────────────────────
    std::string streamName = std::string(cfg->rtsp.name) + " (" + BUILD_COMMIT + ")";
    server_->setStreamName(streamName);
    server_->setStreamInfo("stream0");
    server_->setSendBufferSize(cfg->rtsp.send_buffer_size);
    server_->setSendTimeout(cfg->rtsp.send_timeout_s);

    if (cfg->rtsp.auth_required) {
        server_->setAuthCredentials(cfg->rtsp.username, cfg->rtsp.password);
    }

    // Register streams (must happen before server starts)
    if (cfg->stream0.enabled) {
        LOG_INFO("Registering stream 0: " << cfg->stream0.rtsp_endpoint);
        addSubsession(0, cfg->stream0);
    }
    if (cfg->stream1.enabled) {
        LOG_INFO("Registering stream 1: " << cfg->stream1.rtsp_endpoint);
        addSubsession(1, cfg->stream1);
    }

    // ── Audio-only endpoint (e.g. /mic) ──────────────────────────────
    if (cfg->rtsp.audio_only_enabled && cfg->audio.input_enabled
        && global_audio[0] && global_audio[0]->imp_audio) {
        simple_rtsp::AudioStreamConfig acfg;
        acfg.endpoint = cfg->rtsp.audio_only_endpoint;
        int hwRate = global_audio[0]->imp_audio->sample_rate;
        acfg.sampleRate = (hwRate > 0) ? hwRate : 8000;
        acfg.channels = 1;
        switch (global_audio[0]->imp_audio->format) {
        case IMPAudioFormat::G711U:
            acfg.codec = "PCMU";
            acfg.payloadType = 0;
            break;
        case IMPAudioFormat::G711A:
            acfg.codec = "PCMA";
            acfg.payloadType = 8;
            break;
        case IMPAudioFormat::AAC:
            acfg.codec = "AAC";
            acfg.payloadType = 97;
            break;
        case IMPAudioFormat::OPUS:
            acfg.codec = "OPUS";
            acfg.payloadType = 97;
            break;
        case IMPAudioFormat::PCM:
        default:
            acfg.codec = "L16";
            acfg.payloadType = 97;
            break;
        }
        server_->addAudioOnlyStream(acfg, global_audio[0]);
        LOG_INFO("Audio-only endpoint: /" << acfg.endpoint
                 << " (" << acfg.codec << " " << acfg.sampleRate << "Hz)");
    }

    // ── Set up the signal so main.cpp can stop us ──────────────────────
    global_rtsp_thread_signal = 0; // signal running

    // ── Run the event loop (blocks until stop()) ───────────────────────
    if (!server_->start(cfg->rtsp.port)) {
        LOG_ERROR("Failed to start SimpleRTSP server");
        global_rtsp_thread_signal = 1;
        return;
    }

    // Wait until signaled to stop (poll the signal flag)
    while (global_rtsp_thread_signal == 0 &&
           !global_shutdown_requested.load(std::memory_order_relaxed)) {
        usleep(100000); // 100ms poll
    }

    server_->stop();
    global_rtsp_thread_signal = 1;
}

void *RTSP::run(void *arg) {
    auto *self = static_cast<RTSP *>(arg);
    self->start();
    return nullptr;
}
