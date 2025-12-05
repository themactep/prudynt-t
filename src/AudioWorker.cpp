#include "AudioWorker.hpp"

#include "Config.hpp"
#include "Logger.hpp"
#include "WorkerUtils.hpp"
#include "globals.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MODULE "AudioWorker"

#if defined(AUDIO_SUPPORT)

class AudioTap
{
public:
    AudioTap() = default;

    ~AudioTap()
    {
        shutdown();
    }

    void configure(bool enabled,
                   const std::string &path,
                   int sampleRate,
                   int bitwidth,
                   int channels)
    {
        if (fifoPath != path)
        {
            closeWriter();
            fifoReady = false;
        }

        fifoPath = path;
        enabledTap = enabled && !fifoPath.empty();
        sampleRateHz = (sampleRate > 0) ? sampleRate : 16000;
        bitwidthBits = (bitwidth > 0) ? bitwidth : 16;
        channelCount = std::max(1, channels);
        backpressureWarned = false;

        if (!enabledTap)
        {
            shutdown();
            return;
        }

        if (!fifoReady && !createFifo())
        {
            enabledTap = false;
            return;
        }

        LOG_INFO("AudioTap: ready at " << fifoPath << " (" << bitwidthBits << "-bit "
                                         << sampleRateHz << " Hz, " << channelCount << " ch)");
    }

    void publish(const uint8_t *data, size_t length)
    {
        if (!enabledTap || !data || length == 0)
        {
            return;
        }

        if (!fifoReady && !createFifo())
        {
            return;
        }

        if (fd < 0 && !openWriter())
        {
            return;
        }

        size_t offset = 0;
        while (offset < length)
        {
            ssize_t written = ::write(fd, data + offset, length - offset);
            if (written > 0)
            {
                offset += static_cast<size_t>(written);
                backpressureWarned = false;
                continue;
            }

            if (written < 0 && errno == EINTR)
            {
                continue;
            }

            if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                if (!backpressureWarned)
                {
                    LOG_WARN("AudioTap: FIFO full, dropping PCM samples");
                    backpressureWarned = true;
                }
                break;
            }

            if (written < 0 && (errno == EPIPE || errno == ENXIO))
            {
                LOG_INFO("AudioTap: reader disconnected");
            }
            else if (written < 0)
            {
                LOG_WARN("AudioTap: write failed: " << strerror(errno));
            }

            closeWriter();
            break;
        }
    }

    void shutdown()
    {
        closeWriter();
        if (fifoReady && !fifoPath.empty())
        {
            ::unlink(fifoPath.c_str());
        }
        fifoReady = false;
        backpressureWarned = false;
    }

private:
    bool createFifo()
    {
        if (fifoPath.empty())
        {
            return false;
        }

        auto slash = fifoPath.find_last_of('/');
        if (slash != std::string::npos)
        {
            std::string dir = fifoPath.substr(0, slash);
            if (!dir.empty())
            {
                if (::mkdir(dir.c_str(), 0775) < 0 && errno != EEXIST)
                {
                    LOG_ERROR("AudioTap: mkdir failed for " << dir << ": " << strerror(errno));
                    return false;
                }
            }
        }

        if (::unlink(fifoPath.c_str()) < 0 && errno != ENOENT)
        {
            LOG_WARN("AudioTap: unlink failed for " << fifoPath << ": " << strerror(errno));
        }
        if (::mkfifo(fifoPath.c_str(), 0660) < 0)
        {
            if (errno != EEXIST)
            {
                LOG_ERROR("AudioTap: mkfifo failed for " << fifoPath << ": " << strerror(errno));
                return false;
            }
        }

        fifoReady = true;
        return true;
    }

    bool openWriter()
    {
        fd = ::open(fifoPath.c_str(), O_WRONLY | O_NONBLOCK);
        if (fd < 0)
        {
            if (errno != ENXIO)
            {
                LOG_WARN("AudioTap: open failed for " << fifoPath << ": " << strerror(errno));
            }
            return false;
        }
        return true;
    }

    void closeWriter()
    {
        if (fd >= 0)
        {
            ::close(fd);
            fd = -1;
        }
    }

    std::string fifoPath;
    bool enabledTap{false};
    bool fifoReady{false};
    int fd{-1};
    int sampleRateHz{0};
    int bitwidthBits{0};
    int channelCount{0};
    bool backpressureWarned{false};
};

AudioWorker::AudioWorker(int chn)
    : encChn(chn)
    , mp4_audio_samples(NUM_VIDEO_CHANNELS, 0)
    , mp4_audio_sample_rate(0)
    , tap(std::make_unique<AudioTap>())
{
    LOG_DEBUG("AudioWorker created for channel " << encChn);
}

AudioWorker::~AudioWorker()
{
    LOG_DEBUG("AudioWorker destroyed for channel " << encChn);
}

void AudioWorker::process_audio_frame(IMPAudioFrame &frame)
{
    int64_t audio_ts = frame.timeStamp;
    struct timeval encoder_time;
    encoder_time.tv_sec = audio_ts / 1000000;
    encoder_time.tv_usec = audio_ts % 1000000;

    AudioFrame af;
    af.time = encoder_time;

    uint8_t *start = (uint8_t *) frame.virAddr;
    uint8_t *end = start + frame.len;
    int sample_size_bytes = frame.bitwidth / 8;
    int channels = (frame.soundmode == AUDIO_SOUND_MODE_MONO) ? 1 : 2;
    if (channels <= 0)
    {
        channels = global_audio[encChn]->imp_audio ? global_audio[encChn]->imp_audio->outChnCnt : 1;
    }
    if (channels <= 0)
    {
        channels = 1;
    }
    int64_t frame_samples = 0;
    if (sample_size_bytes > 0)
    {
        frame_samples = frame.len / (sample_size_bytes * channels);
    }

    IMPAudioStream stream;
    if (global_audio[encChn]->imp_audio->format != IMPAudioFormat::PCM)
    {
        if (IMP_AENC_SendFrame(global_audio[encChn]->aeChn, &frame) != 0)
        {
            LOG_ERROR("IMP_AENC_SendFrame(" << global_audio[encChn]->devId << ", "
                                            << global_audio[encChn]->aeChn << ") failed");
        }
        else if (IMP_AENC_PollingStream(global_audio[encChn]->aeChn,
                                        cfg->general.imp_polling_timeout)
                 != 0)
        {
            LOG_ERROR("IMP_AENC_PollingStream(" << global_audio[encChn]->devId << ", "
                                                << global_audio[encChn]->aeChn << ") failed");
        }
        else if (IMP_AENC_GetStream(global_audio[encChn]->aeChn, &stream, IMPBlock::BLOCK) != 0)
        {
            LOG_ERROR("IMP_AENC_GetStream(" << global_audio[encChn]->devId << ", "
                                            << global_audio[encChn]->aeChn << ") failed");
        }
        else
        {
            start = (uint8_t *) stream.stream;
            end = start + stream.len;
        }
    }

    if (end > start)
    {
        af.data.insert(af.data.end(), start, end);
    }

    size_t frame_len = (end > start) ? static_cast<size_t>(end - start) : 0;
    if (frame_samples <= 0)
    {
        frame_samples = 1024;
    }

    bool any_recorder_active = false;
    for (int ch = 0; ch < NUM_VIDEO_CHANNELS; ++ch)
    {
        if (static_cast<size_t>(ch) >= mp4_audio_samples.size())
        {
            break;
        }
        auto &recorder = global_mp4_recorders[ch];
        bool recorder_active = recorder.isActive();
        if (!recorder_active)
        {
            mp4_audio_samples[ch] = 0;
            continue;
        }

        any_recorder_active = true;
        if (frame_len == 0)
        {
            continue;
        }

        if (mp4_audio_sample_rate <= 0)
        {
            if (global_audio[encChn]->imp_audio)
            {
                mp4_audio_sample_rate = global_audio[encChn]->imp_audio->sample_rate;
            }
            if (mp4_audio_sample_rate <= 0)
            {
                mp4_audio_sample_rate = cfg->audio.input_sample_rate;
            }
        }

        int64_t pts_ms = 0;
        if (mp4_audio_sample_rate > 0)
        {
            pts_ms = (mp4_audio_samples[ch] * 1000) / mp4_audio_sample_rate;
        }
        recorder.writeAudio(start, frame_len, pts_ms);
        mp4_audio_samples[ch] += frame_samples;
    }

    if (!any_recorder_active)
    {
        mp4_audio_sample_rate = 0;
    }

    if (!af.data.empty() && global_audio[encChn]->hasDataCallback
        && (global_video[0]->hasDataCallback || global_video[1]->hasDataCallback))
    {
        if (!global_audio[encChn]->msgChannel->write(af))
        {
#if defined(USE_AUDIO_STREAM_REPLICATOR)
            LOG_DDEBUG("audio encChn:" << encChn << ", size:" << af.data.size() << " clogged!");
#else
            LOG_ERROR("audio encChn:" << encChn << ", size:" << af.data.size() << " clogged!");
#endif
        }
        else
        {
            std::unique_lock<std::mutex> lock_stream{global_audio[encChn]->onDataCallbackLock};
            if (global_audio[encChn]->onDataCallback)
                global_audio[encChn]->onDataCallback();
        }
    }

    if (global_audio[encChn]->imp_audio->format != IMPAudioFormat::PCM
        && IMP_AENC_ReleaseStream(global_audio[encChn]->aeChn, &stream) < 0)
    {
        LOG_ERROR("IMP_AENC_ReleaseStream(" << global_audio[encChn]->devId << ", "
                                            << global_audio[encChn]->aeChn << ", &stream) failed");
    }
}

void AudioWorker::publishTapFrame(const IMPAudioFrame &frame)
{
    if (!tap || frame.virAddr == nullptr || frame.len <= 0)
    {
        return;
    }

    tap->publish(reinterpret_cast<uint8_t *>(frame.virAddr),
                 static_cast<size_t>(frame.len));
}

void AudioWorker::process_frame(IMPAudioFrame &frame)
{
    if (global_audio[encChn]->imp_audio->outChnCnt == 2 && frame.soundmode == AUDIO_SOUND_MODE_MONO)
    {
        size_t sample_size = frame.bitwidth / 8;
        size_t num_samples = frame.len / sample_size;
        size_t stereo_size = frame.len * 2;
        uint8_t *stereo_buffer = new uint8_t[stereo_size];

        for (size_t i = 0; i < num_samples; i++)
        {
            uint8_t *mono_sample = ((uint8_t *) frame.virAddr) + (i * sample_size);
            uint8_t *stereo_left = stereo_buffer + (i * sample_size * 2);
            uint8_t *stereo_right = stereo_left + sample_size;
            memcpy(stereo_left, mono_sample, sample_size);
            memcpy(stereo_right, mono_sample, sample_size);
        }

        IMPAudioFrame stereo_frame = frame;
        stereo_frame.virAddr = (uint32_t *) stereo_buffer;
        stereo_frame.len = stereo_size;
        stereo_frame.soundmode = AUDIO_SOUND_MODE_STEREO;

        publishTapFrame(stereo_frame);
        process_audio_frame(stereo_frame);
        delete[] stereo_buffer;
    }
    else
    {
        publishTapFrame(frame);
        process_audio_frame(frame);
    }
}

void AudioWorker::run()
{
    LOG_DEBUG("Start audio processing run loop for channel " << encChn);

    if (tap && cfg)
    {
        const char *path = cfg->audio.tap_path ? cfg->audio.tap_path : "";
        int sampleRate = cfg->audio.input_sample_rate;
        int bitwidth = 16; // Prudynt captures 16-bit PCM frames
        int channels = 1;
        if (global_audio[encChn]->imp_audio)
        {
            if (global_audio[encChn]->imp_audio->sample_rate > 0)
            {
                sampleRate = global_audio[encChn]->imp_audio->sample_rate;
            }
            if (global_audio[encChn]->imp_audio->outChnCnt > 0)
            {
                channels = global_audio[encChn]->imp_audio->outChnCnt;
            }
        }
        tap->configure(cfg->audio.tap_enabled && cfg->audio.input_enabled,
                       path,
                       sampleRate,
                       bitwidth,
                       channels);
    }

    // Initialize AudioReframer only if needed, store in member variable
    if (global_audio[encChn]->imp_audio->format == IMPAudioFormat::AAC)
    {
        reframer = std::make_unique<AudioReframer>(
            global_audio[encChn]->imp_audio->sample_rate,
            /* inputSamplesPerFrame */ global_audio[encChn]->imp_audio->sample_rate * 0.040,
            /* outputSamplesPerFrame */ 1024);
        LOG_DEBUG("AudioReframer created for channel " << encChn);
    }
    else
    {
        LOG_DEBUG("AudioReframer not needed or imp_audio not ready for channel " << encChn);
    }

    while (global_audio[encChn]->running)
    {
        bool recorder_needs_audio =
            (global_mp4_active_recorders.load(std::memory_order_relaxed) > 0);
        bool video_clients_active = global_video[0]->hasDataCallback || global_video[1]->hasDataCallback
            || global_force_video_active.load(std::memory_order_relaxed)
            || recorder_needs_audio;
        bool should_capture_audio = cfg->audio.input_enabled
            && (global_audio[encChn]->hasDataCallback || recorder_needs_audio)
            && (video_clients_active || recorder_needs_audio);

        if (should_capture_audio)
        {
            if (IMP_AI_PollingFrame(global_audio[encChn]->devId,
                                    global_audio[encChn]->aiChn,
                                    cfg->general.imp_polling_timeout)
                == 0)
            {
                IMPAudioFrame frame;
                if (IMP_AI_GetFrame(global_audio[encChn]->devId,
                                    global_audio[encChn]->aiChn,
                                    &frame,
                                    IMPBlock::BLOCK)
                    != 0)
                {
                    LOG_ERROR("IMP_AI_GetFrame(" << global_audio[encChn]->devId << ", "
                                                 << global_audio[encChn]->aiChn << ") failed");
                    continue; // avoid using an uninitialized frame
                }

                if (reframer)
                {
                    reframer->addFrame(reinterpret_cast<uint8_t *>(frame.virAddr), frame.timeStamp);
                    while (reframer->hasMoreFrames())
                    {
                        size_t frameLen = 1024 * sizeof(uint16_t)
                                          * global_audio[encChn]->imp_audio->outChnCnt;
                        std::vector<uint8_t> frameData(frameLen, 0);
                        int64_t audio_ts;
                        reframer->getReframedFrame(frameData.data(), audio_ts);
                        IMPAudioFrame reframed = {.bitwidth = frame.bitwidth,
                                                  .soundmode = frame.soundmode,
                                                  .virAddr = reinterpret_cast<uint32_t *>(
                                                      frameData.data()),
                                                  .phyAddr = frame.phyAddr,
                                                  .timeStamp = audio_ts,
                                                  .seq = frame.seq,
                                                  .len = static_cast<int>(frameLen)};
                        process_frame(reframed);
                    }
                }
                else
                {
                    process_frame(frame);
                }

                if (IMP_AI_ReleaseFrame(global_audio[encChn]->devId,
                                        global_audio[encChn]->aiChn,
                                        &frame)
                    < 0)
                {
                    LOG_ERROR("IMP_AI_ReleaseFrame(" << global_audio[encChn]->devId << ", "
                                                     << global_audio[encChn]->aiChn
                                                     << ", &frame) failed");
                }
            }
            else
            {
                LOG_DEBUG(global_audio[encChn]->devId << ", " << global_audio[encChn]->aiChn
                                                      << " POLLING TIMEOUT");
            }
        }
        else if (cfg->audio.input_enabled && !global_restart)
        {
            std::unique_lock<std::mutex> lock_stream{mutex_main};
            global_audio[encChn]->active = false;
            LOG_DDEBUG("AUDIO LOCK");

            /* Since the audio stream is permanently in use by the stream replicator,
             * we send the audio grabber and encoder to standby when no video is requested.
            */
            while (!global_restart_audio)
            {
                bool recorder_needed_now =
                    (global_mp4_active_recorders.load(std::memory_order_relaxed) > 0);
                bool video_clients_active_now = global_video[0]->hasDataCallback
                    || global_video[1]->hasDataCallback
                    || global_force_video_active.load(std::memory_order_relaxed)
                    || recorder_needed_now;
                if (global_audio[encChn]->onDataCallback != nullptr
                    && (video_clients_active_now || recorder_needed_now))
                {
                    break;
                }
                if (recorder_needed_now)
                {
                    break;
                }
                global_audio[encChn]->should_grab_frames.wait(lock_stream);
            }
            global_audio[encChn]->active = true;
            LOG_DDEBUG("AUDIO UNLOCK");
        }
        else
        {
            /* to prevent clogging on startup or while restarting the threads
             * we wait for 250ms
            */
            usleep(250 * 1000);
        }
    }
}

void *AudioWorker::thread_entry(void *arg)
{
    StartHelper *sh = static_cast<StartHelper *>(arg);
    int encChn = sh->encChn;

    LOG_DEBUG("Start audio_grabber thread for device "
              << global_audio[encChn]->devId << " and channel " << global_audio[encChn]->aiChn
              << " and encoder " << global_audio[encChn]->aeChn);

    global_audio[encChn]->imp_audio = IMPAudio::createNew(global_audio[encChn]->devId,
                                                          global_audio[encChn]->aiChn,
                                                          global_audio[encChn]->aeChn);

    // inform main that initialization is complete
    sh->has_started.release();

    /* 'active' indicates, the thread is activly polling and grabbing images
     * 'running' describes the runlevel of the thread, if this value is set to false
     *           the thread exits and cleanup all ressources
     */
    global_audio[encChn]->active = true;
    global_audio[encChn]->running = true;

    AudioWorker worker(encChn);
    worker.run();

    if (global_audio[encChn]->imp_audio)
    {
        delete global_audio[encChn]->imp_audio;
    }

    return 0;
}

#endif // AUDIO_SUPPORT
