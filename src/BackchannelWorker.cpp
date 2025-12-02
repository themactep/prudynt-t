#include "BackchannelWorker.hpp"

#include "AudioOutputWorker.hpp"
#include "IMPBackchannel.hpp"
#include "Logger.hpp"

#include <cassert>
#include <cmath>
#include <vector>

#include <imp/imp_audio.h>

#define MODULE "BackchannelWorker"

BackchannelWorker::BackchannelWorker()
    : currentSessionId(0)
{}

BackchannelWorker::~BackchannelWorker() = default;

std::vector<int16_t> BackchannelWorker::resampleLinear(const std::vector<int16_t> &input_pcm,
                                                       int input_rate,
                                                       int output_rate)
{
    assert(input_rate != output_rate);

    double ratio = static_cast<double>(output_rate) / input_rate;
    size_t output_size = static_cast<size_t>(
        std::max(1.0, std::round(static_cast<double>(input_pcm.size()) * ratio)));

    std::vector<int16_t> output_pcm(output_size);
    size_t input_size = input_pcm.size();

    for (size_t i = 0; i < output_size; ++i)
    {
        double input_pos = static_cast<double>(i) / ratio;
        size_t index1 = static_cast<size_t>(input_pos);

        if (index1 >= input_size)
        {
            index1 = input_size - 1;
        }

        int16_t sample1 = input_pcm[index1];
        int16_t sample2 = (index1 + 1 < input_size) ? input_pcm[index1 + 1] : sample1;

        double factor = input_pos - static_cast<double>(index1);

        double interpolated_sample = static_cast<double>(sample1) * (1.0 - factor)
                                     + static_cast<double>(sample2) * factor;

        if (interpolated_sample > INT16_MAX)
            interpolated_sample = INT16_MAX;
        if (interpolated_sample < INT16_MIN)
            interpolated_sample = INT16_MIN;

        output_pcm[i] = static_cast<int16_t>(interpolated_sample);
    }

    return output_pcm;
}

bool BackchannelWorker::decodeFrame(const uint8_t *payload,
                                    size_t payloadSize,
                                    IMPBackchannelFormat format,
                                    std::vector<int16_t> &outPcmBuffer)
{
    IMPAudioStream stream_in;
    stream_in.stream = const_cast<uint8_t *>(payload);
    stream_in.len = static_cast<int>(payloadSize);

    int adChn = (int) format;
    int ret = IMP_ADEC_SendStream(adChn, &stream_in, BLOCK);
    if (ret != 0)
    {
        LOG_ERROR("IMP_ADEC_SendStream failed for channel " << adChn << ": " << ret);
        return false;
    }

    IMPAudioStream stream_out;
    ret = IMP_ADEC_GetStream(adChn, &stream_out, BLOCK);
    if (ret == 0 && stream_out.len > 0 && stream_out.stream != nullptr)
    {
        size_t num_samples = stream_out.len / sizeof(int16_t);
        if (stream_out.len % sizeof(int16_t) != 0)
        {
            LOG_WARN("Decoded stream length (" << stream_out.len
                                               << ") not multiple of int16_t size. Truncating.");
        }
        outPcmBuffer.assign(reinterpret_cast<int16_t *>(stream_out.stream),
                            reinterpret_cast<int16_t *>(stream_out.stream) + num_samples);
        IMP_ADEC_ReleaseStream(adChn, &stream_out);
        return true;
    }
    else if (ret != 0)
    {
        LOG_ERROR("IMP_ADEC_GetStream failed for channel " << adChn << ": " << ret);
        return false;
    }

    LOG_DEBUG("ADEC_GetStream succeeded but produced no data.");
    outPcmBuffer.clear();
    return true;
}

bool BackchannelWorker::processFrame(const BackchannelFrame &frame)
{
    if (!cfg->audio.output_enabled)
    {
        return true;
    }

    std::vector<int16_t> decoded_pcm;
    if (!decodeFrame(frame.payload.data(), frame.payload.size(), frame.format, decoded_pcm))
    {
        // Error already logged in decodeFrame
        return true; // Continue processing loop, maybe next frame works
    }

    if (decoded_pcm.empty())
    {
        LOG_WARN("decodeFrame returned empty PCM buffer.");
        return true; // Nothing to process
    }

    // Resample only if necessary
    int input_rate = IMPBackchannel::getFormatFrequency(frame.format);
    int target_rate = cfg->audio.output_sample_rate;
    std::vector<int16_t> pcm_to_send;
    if (input_rate != target_rate)
    {
        pcm_to_send = resampleLinear(decoded_pcm, input_rate, target_rate);
    }
    else
    {
        pcm_to_send = std::move(decoded_pcm);
    }

    if (!pcm_to_send.empty())
    {
        AudioOutputWorker::enqueuePcm(std::move(pcm_to_send));
    }

    return true;
}

void BackchannelWorker::run()
{
    if (!global_backchannel)
    {
        LOG_ERROR("Cannot run BackchannelWorker: global_backchannel is null.");
        return;
    }

    LOG_INFO("Processor thread running...");

    global_backchannel->running = true;
    while (global_backchannel->running)
    {
        // Wait for condition: running and at least one sink is sending
        {
            std::unique_lock<std::mutex> lock(global_backchannel->mutex);
            global_backchannel->should_grab_frames.wait(lock, [&] {
                return !global_backchannel->running
                       || global_backchannel->is_sending.load(std::memory_order_acquire) > 0;
            });
        }

        if (!global_backchannel->running)
        {
            break;
        }

        BackchannelFrame frame = global_backchannel->inputQueue->wait_read();

        if (frame.isShutdownSentinel)
        {
            LOG_DEBUG("Received shutdown sentinel frame. Exiting processor loop.");
            break;
        }

        if (!global_backchannel->running)
        {
            break;
        }

        // Handle Zero-Payload Frame (Stop Signal)
        if (frame.payload.empty())
        {
            LOG_DEBUG("Received stop signal (zero-payload) from session " << static_cast<unsigned>(frame.clientSessionId));
            if (frame.clientSessionId == currentSessionId && currentSessionId != 0)
            {
                LOG_INFO("Current session " << static_cast<unsigned>(currentSessionId) << " stopped."
                                             "");
                currentSessionId = 0;
            }
            else if (currentSessionId == 0)
            {
                LOG_DEBUG("Stop signal received but no current session. Ignoring.");
            }
            else
            {
                LOG_WARN("Stop signal from non-current session "
                         << static_cast<unsigned>(frame.clientSessionId) << " (Current: " << static_cast<unsigned>(currentSessionId)
                         << "). Ignoring.");
            }
            continue;
        }

        // Handle Playback Frame (Data)
        if (currentSessionId == 0)
        {
            // No current session, this frame's sender becomes the current one
            currentSessionId = frame.clientSessionId;
            LOG_INFO("New current session " << static_cast<unsigned>(currentSessionId) << " playing "
                                            << IMPBackchannel::getFormatName(frame.format)
                                            << ".");
            processFrame(frame);
        }
        else if (frame.clientSessionId == currentSessionId)
        {
            processFrame(frame);
        }
        else
        {
            // Frame is from a different session, ignore it
            LOG_DEBUG("Discarding frame from non-current session "
                      << static_cast<unsigned>(frame.clientSessionId) << " (Current: " << static_cast<unsigned>(currentSessionId) << ")");
        }
    }

    LOG_INFO("Processor thread stopping.");
}

void *BackchannelWorker::thread_entry(void *arg)
{
    LOG_INFO("Starting BackchannelWorker thread.");

    global_backchannel->imp_backchannel = IMPBackchannel::createNew();

    BackchannelWorker processor;
    processor.run();

    global_backchannel->imp_backchannel->deinit();
    delete global_backchannel->imp_backchannel;
    global_backchannel->imp_backchannel = nullptr;

    LOG_INFO("Exiting BackchannelWorker thread.");
    return nullptr;
}

void BackchannelWorker::signalShutdown()
{
    if (!global_backchannel || !global_backchannel->inputQueue)
    {
        return;
    }

    BackchannelFrame sentinel;
    sentinel.payload.clear();
    sentinel.format = IMPBackchannelFormat::UNKNOWN;
    sentinel.clientSessionId = 0;
    sentinel.isShutdownSentinel = true;

    bool enqueued = global_backchannel->inputQueue->write(sentinel);
    if (!enqueued)
    {
        LOG_WARN("Backchannel shutdown sentinel enqueued after dropping oldest frame (queue was full).");
    }

    global_backchannel->should_grab_frames.notify_one();
}
