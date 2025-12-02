#include "AudioOutputWorker.hpp"

#include "IMPAudioOutput.hpp"
#include "Logger.hpp"
#include "Config.hpp"

#define MODULE "AudioOutputWorker"

void *AudioOutputWorker::thread_entry(void * /*arg*/)
{
    AudioOutputWorker worker;
    worker.run();
    return nullptr;
}

bool AudioOutputWorker::enqueuePcm(std::vector<int16_t> &&samples,
                                   bool applyVolume,
                                   int volume,
                                   bool applyGain,
                                   int gain)
{
    if (!global_audio_output || !global_audio_output->jobQueue)
    {
        LOG_ERROR("Audio output queue is not initialized");
        return false;
    }

    AudioPlaybackJob job;
    job.type = AudioPlaybackJobType::PCM;
    job.samples = std::move(samples);
    job.hasVolume = applyVolume;
    job.volume = volume;
    job.hasGain = applyGain;
    job.gain = gain;

    bool enqueued = global_audio_output->jobQueue->write(std::move(job));
    if (!enqueued)
    {
        LOG_WARN("Audio output queue full; dropped oldest PCM chunk to enqueue new data");
    }

    return true;
}

bool AudioOutputWorker::clearQueue()
{
    if (!global_audio_output || !global_audio_output->jobQueue)
    {
        return false;
    }

    global_audio_output->jobQueue->clear();

    AudioPlaybackJob job;
    job.type = AudioPlaybackJobType::CLEAR;

    bool enqueued = global_audio_output->jobQueue->write(std::move(job));
    if (!enqueued)
    {
        LOG_WARN("Audio output queue full while enqueuing clear command; dropped oldest chunk");
    }

    return true;
}

void AudioOutputWorker::signalShutdown()
{
    if (!global_audio_output || !global_audio_output->jobQueue)
    {
        return;
    }

    AudioPlaybackJob job;
    job.type = AudioPlaybackJobType::STOP;

    bool enqueued = global_audio_output->jobQueue->write(std::move(job));
    if (!enqueued)
    {
        LOG_WARN("Audio output queue was full while enqueuing shutdown sentinel");
    }
}

void AudioOutputWorker::run()
{
    if (!global_audio_output)
    {
        LOG_ERROR("Audio output stream not initialized");
        return;
    }

    global_audio_output->running = true;
    global_audio_output->current_volume = cfg->audio.output_vol;
    global_audio_output->current_gain = cfg->audio.output_gain;

    global_audio_output->imp_audio_output = std::make_unique<IMPAudioOutput>(0, 0);
    if (!global_audio_output->imp_audio_output->init())
    {
        LOG_ERROR("Failed to initialize IMP audio output");
        global_audio_output->imp_audio_output.reset();
        global_audio_output->running = false;
        return;
    }

    while (global_audio_output->running)
    {
        AudioPlaybackJob job = global_audio_output->jobQueue->wait_read();
        if (job.type == AudioPlaybackJobType::STOP)
        {
            break;
        }
        if (job.type == AudioPlaybackJobType::CLEAR)
        {
            if (global_audio_output->imp_audio_output)
            {
                global_audio_output->imp_audio_output->flush();
            }
            continue;
        }

        if (job.hasVolume)
        {
            global_audio_output->imp_audio_output->setVolume(job.volume);
            global_audio_output->current_volume = job.volume;
        }
        if (job.hasGain)
        {
            global_audio_output->imp_audio_output->setGain(job.gain);
            global_audio_output->current_gain = job.gain;
        }

        if (!job.samples.empty())
        {
            if (!global_audio_output->imp_audio_output->playSamples(job.samples.data(), job.samples.size()))
            {
                LOG_WARN("Failed to play PCM chunk (" << job.samples.size() << " samples)");
            }
        }
    }

    global_audio_output->imp_audio_output.reset();
    global_audio_output->running = false;
}
