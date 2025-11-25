#include "MP4ControlSocket.hpp"

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <chrono>
#include <thread>
#include <vector>
#include <array>
#include <cstdlib>
#include <atomic>
#include <condition_variable>
#include <string>
#include <cstdio>

#include "Config.hpp"
#include "Logger.hpp"
#include "MP4Muxer.hpp"
#include "globals.hpp"

#include <imp/imp_encoder.h>

namespace {
    constexpr const char *FIFO_DIR = "/run/prudynt";
    constexpr const char *FIFO_PATH = "/run/prudynt/mp4ctl";

    std::string channel_state_path(int channel)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%s/mp4ctl-ch%d.active", FIFO_DIR, channel);
        return std::string(buffer);
    }

    void write_channel_state_file(int channel,
                                  const std::string &record_path,
                                  int duration_seconds)
    {
        if (channel < 0 || channel >= NUM_VIDEO_CHANNELS)
        {
            return;
        }
        auto state_path = channel_state_path(channel);
        int fd = ::open(state_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0)
        {
            LOG_WARN("MP4ControlSocket: failed to update state file " << state_path);
            return;
        }
        std::string payload = "path=" + record_path + "\n";
        if (duration_seconds > 0)
        {
            payload += "duration=" + std::to_string(duration_seconds) + "\n";
        }
        ssize_t ignored = ::write(fd, payload.c_str(), payload.size());
        (void) ignored;
        ::close(fd);
    }

    void remove_channel_state_file(int channel)
    {
        if (channel < 0 || channel >= NUM_VIDEO_CHANNELS)
        {
            return;
        }
        auto state_path = channel_state_path(channel);
        ::unlink(state_path.c_str());
    }

    std::mutex stop_timer_mutex;
    std::array<std::thread, NUM_VIDEO_CHANNELS> stop_timer_threads;
    std::array<std::atomic<bool>, NUM_VIDEO_CHANNELS> stop_timer_cancel;
    struct StopTimerInitializer
    {
        StopTimerInitializer()
        {
            for (auto &flag : stop_timer_cancel)
            {
                flag.store(false, std::memory_order_relaxed);
            }
        }
    } stop_timer_initializer;

    bool snapshot_codec_config(std::vector<uint8_t> &sps,
                               std::vector<uint8_t> &pps,
                               int channel = -1)
    {
        auto snapshot_from_channel = [&](int ch) -> bool {
            if (ch < 0 || ch >= NUM_VIDEO_CHANNELS)
            {
                return false;
            }
            auto &vs = global_video[ch];
            if (!vs)
            {
                return false;
            }
            std::lock_guard<std::mutex> lock(vs->codec_config_mutex);
            if (vs->have_sps && vs->have_pps && !vs->latest_sps.empty()
                && !vs->latest_pps.empty())
            {
                sps = vs->latest_sps;
                pps = vs->latest_pps;
                return true;
            }
            return false;
        };

        if (channel >= 0)
        {
            return snapshot_from_channel(channel);
        }

        for (int ch = 0; ch < NUM_VIDEO_CHANNELS; ++ch)
        {
            if (snapshot_from_channel(ch))
            {
                return true;
            }
        }
        return false;
    }

    bool wait_for_codec_config(std::vector<uint8_t> &sps,
                               std::vector<uint8_t> &pps,
                               std::chrono::milliseconds timeout,
                               int channel = -1)
    {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (snapshot_codec_config(sps, pps, channel))
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

    bool build_avcc(const std::vector<uint8_t> &sps,
                    const std::vector<uint8_t> &pps,
                    std::vector<uint8_t> &avcC)
    {
        if (sps.size() < 4 || pps.empty())
        {
            return false;
        }

        avcC.clear();
        avcC.reserve(11 + sps.size() + pps.size());
        avcC.push_back(0x01);
        avcC.push_back(sps[1]);
        avcC.push_back(sps[2]);
        avcC.push_back(sps[3]);
        avcC.push_back(0xFF);
        avcC.push_back(0xE1);
        uint16_t sps_len = static_cast<uint16_t>(sps.size());
        avcC.push_back(static_cast<uint8_t>((sps_len >> 8) & 0xFF));
        avcC.push_back(static_cast<uint8_t>(sps_len & 0xFF));
        avcC.insert(avcC.end(), sps.begin(), sps.end());

        avcC.push_back(0x01);
        uint16_t pps_len = static_cast<uint16_t>(pps.size());
        avcC.push_back(static_cast<uint8_t>((pps_len >> 8) & 0xFF));
        avcC.push_back(static_cast<uint8_t>(pps_len & 0xFF));
        avcC.insert(avcC.end(), pps.begin(), pps.end());
        return true;
    }

#if defined(AUDIO_SUPPORT)
    bool build_aac_config(std::vector<uint8_t> &aacConfig)
    {
        if (!cfg || !cfg->audio.input_enabled || std::strcmp(cfg->audio.input_format, "AAC") != 0)
        {
            return false;
        }

        static constexpr int sample_rate_table[] = {96000, 88200, 64000, 48000, 44100, 32000,
                                                    24000, 22050, 16000, 12000, 11025, 8000,
                                                    7350};
        int sample_rate = cfg->audio.input_sample_rate;
        int sample_rate_index = -1;
        for (int i = 0; i < static_cast<int>(sizeof(sample_rate_table) / sizeof(sample_rate_table[0]));
             ++i)
        {
            if (sample_rate_table[i] == sample_rate)
            {
                sample_rate_index = i;
                break;
            }
        }

#if defined(LIB_AUDIO_PROCESSING)
        int channels = cfg->audio.force_stereo ? 2 : 1;
#else
        int channels = 1;
#endif
        if (channels < 1 || channels > 7)
        {
            LOG_ERROR("MP4ControlSocket: unsupported channel count for AAC AudioSpecificConfig");
            return false;
        }

        uint8_t audioObjectType = 2; // AAC LC
        auto append_bits = [&](uint32_t value, int bits,
                               uint8_t &current_byte,
                               int &bit_count,
                               std::vector<uint8_t> &out) {
            for (int i = bits - 1; i >= 0; --i)
            {
                current_byte = static_cast<uint8_t>((current_byte << 1) | ((value >> i) & 0x01));
                bit_count++;
                if (bit_count == 8)
                {
                    out.push_back(current_byte);
                    bit_count = 0;
                    current_byte = 0;
                }
            }
        };
        auto finalize_bits = [&](uint8_t &current_byte, int &bit_count, std::vector<uint8_t> &out) {
            if (bit_count > 0)
            {
                current_byte <<= (8 - bit_count);
                out.push_back(current_byte);
                current_byte = 0;
                bit_count = 0;
            }
        };

        aacConfig.clear();
        uint8_t current_byte = 0;
        int bit_count = 0;
        append_bits(audioObjectType, 5, current_byte, bit_count, aacConfig);
        if (sample_rate_index >= 0)
        {
            append_bits(static_cast<uint32_t>(sample_rate_index), 4, current_byte, bit_count, aacConfig);
        }
        else
        {
            append_bits(0x0F, 4, current_byte, bit_count, aacConfig);
            append_bits(static_cast<uint32_t>(sample_rate), 24, current_byte, bit_count, aacConfig);
        }
        append_bits(static_cast<uint32_t>(channels), 4, current_byte, bit_count, aacConfig);
        finalize_bits(current_byte, bit_count, aacConfig);
        return true;
    }
#endif

    bool start_recording(const std::string &path, int target_channel) {
        if (!cfg) {
            return false;
        }

        if (target_channel < 0 || target_channel >= NUM_VIDEO_CHANNELS)
        {
            LOG_ERROR("MP4ControlSocket: invalid channel " << target_channel);
            return false;
        }

        auto video = global_video[target_channel];
        if (!video)
        {
            LOG_ERROR("MP4ControlSocket: video channel " << target_channel << " not available");
            return false;
        }

        auto &recorder = global_mp4_recorders[target_channel];
        if (recorder.isActive())
        {
            LOG_WARN("MP4ControlSocket: recorder already active on channel " << target_channel);
            return false;
        }

        auto disable_force_if_idle = []() {
            if (global_mp4_active_recorders.load(std::memory_order_relaxed) == 0)
            {
                global_force_video_active.store(false, std::memory_order_relaxed);
            }
        };
        auto reset_wait_state = [&]() {
            video->mp4_waiting_for_idr.store(false, std::memory_order_relaxed);
        };

        global_force_video_active.store(true, std::memory_order_relaxed);
        for (int i = 0; i < NUM_VIDEO_CHANNELS; ++i)
        {
            auto worker = global_video[i];
            if (!worker)
            {
                continue;
            }
            worker->should_grab_frames.notify_one();
            bool is_target = (i == target_channel);
            worker->mp4_waiting_for_idr.store(is_target, std::memory_order_relaxed);
            if (is_target)
            {
                int64_t last_idr = worker->mp4_last_idr_ts.load(std::memory_order_relaxed);
                worker->mp4_required_idr_ts.store(last_idr, std::memory_order_relaxed);
                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
                worker->mp4_last_idr_request_ms.store(static_cast<uint64_t>(now_ms),
                                                      std::memory_order_relaxed);
                IMP_Encoder_RequestIDR(worker->encChn);
            }
        }

        std::vector<uint8_t> sps;
        std::vector<uint8_t> pps;
        if (!wait_for_codec_config(sps, pps, std::chrono::milliseconds(1500), target_channel))
        {
            LOG_ERROR("MP4ControlSocket: timed out waiting for SPS/PPS before START");
            reset_wait_state();
            disable_force_if_idle();
            return false;
        }

        const _stream *stream_cfg = nullptr;
        if (target_channel == 0)
        {
            stream_cfg = &cfg->stream0;
        }
        else if (target_channel == 1)
        {
            stream_cfg = &cfg->stream1;
        }

        MP4Muxer::InitParams init{};
        if (stream_cfg)
        {
            init.width = stream_cfg->width;
            init.height = stream_cfg->height;
            init.fps = stream_cfg->fps;
        }
        else
        {
            init.width = cfg->stream0.width;
            init.height = cfg->stream0.height;
            init.fps = cfg->stream0.fps;
        }
        init.sampleRate = 0;
        init.channels = 0;
#if defined(AUDIO_SUPPORT)
        if (cfg->audio.input_enabled)
        {
#if defined(LIB_AUDIO_PROCESSING)
            init.channels = cfg->audio.force_stereo ? 2 : 1;
#else
            init.channels = 1;
#endif
            init.sampleRate = cfg->audio.input_sample_rate;
            if (std::strcmp(cfg->audio.input_format, "AAC") == 0)
            {
                if (!build_aac_config(init.aacConfig))
                {
                    init.channels = 0;
                    init.sampleRate = 0;
                }
            }
            else
            {
                // PCM/OPUS/G711 currently unsupported in MP4 recorder
                init.channels = 0;
                init.sampleRate = 0;
            }
        }
#endif

        if (!build_avcc(sps, pps, init.avcC))
        {
            LOG_ERROR("MP4ControlSocket: failed to build avcC from SPS/PPS");
            reset_wait_state();
            disable_force_if_idle();
            return false;
        }

        bool ok = recorder.start(path.c_str(), init);
        if (ok) {
            global_mp4_active_recorders.fetch_add(1, std::memory_order_relaxed);
            LOG_INFO("MP4ControlSocket: recorder started with avcC payload size=" << init.avcC.size()
                                                                                  << " on channel "
                                                                                  << target_channel);
        } else {
            reset_wait_state();
            disable_force_if_idle();
        }
        return ok;
    }

    void stop_recording(int channel)
    {
        if (channel < 0 || channel >= NUM_VIDEO_CHANNELS)
        {
            return;
        }

        auto &recorder = global_mp4_recorders[channel];
        if (!recorder.isActive())
        {
            return;
        }

        recorder.stop();
        if (global_video[channel])
        {
            global_video[channel]->mp4_waiting_for_idr.store(false);
        }
        remove_channel_state_file(channel);

        int remaining = global_mp4_active_recorders.fetch_sub(1, std::memory_order_relaxed) - 1;
        if (remaining <= 0)
        {
            global_mp4_active_recorders.store(0, std::memory_order_relaxed);
            global_force_video_active.store(false, std::memory_order_relaxed);
        }
        LOG_INFO("MP4ControlSocket: recorder stopped for channel " << channel);
    }

    void stop_all_recordings()
    {
        for (int ch = 0; ch < NUM_VIDEO_CHANNELS; ++ch)
        {
            stop_recording(ch);
        }
    }

    void cancel_stop_timer(int channel)
    {
        if (channel < 0 || channel >= NUM_VIDEO_CHANNELS)
        {
            return;
        }

        std::thread to_join;
        {
            std::lock_guard<std::mutex> lock(stop_timer_mutex);
            if (stop_timer_threads[channel].joinable())
            {
                stop_timer_cancel[channel].store(true, std::memory_order_relaxed);
                to_join = std::move(stop_timer_threads[channel]);
            }
        }
        if (to_join.joinable())
        {
            to_join.join();
        }
        stop_timer_cancel[channel].store(false, std::memory_order_relaxed);
    }

    void cancel_all_stop_timers()
    {
        for (int ch = 0; ch < NUM_VIDEO_CHANNELS; ++ch)
        {
            cancel_stop_timer(ch);
        }
    }

    void schedule_stop_timer(int channel, int duration_seconds)
    {
        if (channel < 0 || channel >= NUM_VIDEO_CHANNELS || duration_seconds <= 0)
        {
            return;
        }

        cancel_stop_timer(channel);
        {
            std::lock_guard<std::mutex> lock(stop_timer_mutex);
            stop_timer_cancel[channel].store(false, std::memory_order_relaxed);
            stop_timer_threads[channel] = std::thread([channel, duration_seconds]() {
                LOG_INFO("MP4ControlSocket: auto-stop timer scheduled for " << duration_seconds
                                                                            << " seconds on channel "
                                                                            << channel);
                auto deadline = std::chrono::steady_clock::now()
                                 + std::chrono::seconds(duration_seconds);
                while (!stop_timer_cancel[channel].load(std::memory_order_relaxed)
                       && std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (!stop_timer_cancel[channel].load(std::memory_order_relaxed))
                {
                    LOG_INFO("MP4ControlSocket: auto-stop timer elapsed for channel " << channel);
                    stop_recording(channel);
                }
            });
        }
    }

    bool parse_int_token(const std::string &token, int &value)
    {
        if (token.empty())
        {
            return false;
        }
        char *end = nullptr;
        long v = std::strtol(token.c_str(), &end, 10);
        if (*end != '\0')
        {
            return false;
        }
        value = static_cast<int>(v);
        return true;
    }
}

void MP4ControlSocket::run() {
    LOG_INFO("MP4ControlSocket: run() entered");

    // Create FIFO if it does not exist
    if (mkdir(FIFO_DIR, 0775) < 0 && errno != EEXIST)
    {
        LOG_ERROR("MP4ControlSocket: mkdir failed for " << FIFO_DIR);
        return;
    }

    unlink(FIFO_PATH);
    if (mkfifo(FIFO_PATH, 0666) < 0) {
        LOG_ERROR("MP4ControlSocket: mkfifo failed for " << FIFO_PATH);
        return;
    }

    LOG_INFO("MP4ControlSocket: listening on FIFO " << FIFO_PATH);
    LOG_INFO("MP4ControlSocket: FIFO setup completed, entering accept/read loop");

    char buf[512];
    while (true) {
        int fd = open(FIFO_PATH, O_RDONLY);
        if (fd < 0) {
            LOG_ERROR("MP4ControlSocket: open() failed for FIFO, exiting loop");
            break;
        }

        LOG_INFO("MP4ControlSocket: FIFO opened, waiting for commands");

        while (true) {
            std::memset(buf, 0, sizeof(buf));
            ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
            if (n <= 0) {
                LOG_INFO("MP4ControlSocket: read() returned " << n << ", closing FIFO and reopening");
                break;
            }

            std::string cmd(buf, static_cast<size_t>(n));
            LOG_INFO("MP4ControlSocket: raw command buffer ('" << cmd << "') length=" << n);
            std::istringstream iss(cmd);
            std::string op;
            iss >> op;
            LOG_INFO("MP4ControlSocket: parsed op='" << op << "'");

            if (op == "START") {
                std::string path;
                iss >> path;
                int duration_seconds = 0;
                int channel = 0;
                bool duration_set = false;
                bool channel_set = false;
                std::string token;
                while (iss >> token)
                {
                    std::string key;
                    std::string value = token;
                    auto eq_pos = token.find('=');
                    if (eq_pos != std::string::npos)
                    {
                        key = token.substr(0, eq_pos);
                        value = token.substr(eq_pos + 1);
                    }
                    int parsed_value = 0;
                    if (!parse_int_token(value, parsed_value))
                    {
                        LOG_WARN("MP4ControlSocket: ignoring non-numeric token '" << token
                                                                                   << "' in START command");
                        continue;
                    }
                    auto assign_if_matches = [&](const std::string &k, const char *opt1, const char *opt2, bool &flag, int &dest) {
                        if (!flag && !k.empty() && (k == opt1 || k == opt2))
                        {
                            dest = parsed_value;
                            flag = true;
                            return true;
                        }
                        return false;
                    };
                    if (!key.empty())
                    {
                        if (assign_if_matches(key, "dur", "duration", duration_set, duration_seconds))
                        {
                            continue;
                        }
                        if (assign_if_matches(key, "ch", "channel", channel_set, channel))
                        {
                            continue;
                        }
                        LOG_WARN("MP4ControlSocket: unrecognized key '" << key
                                                                       << "' in START command");
                        continue;
                    }
                    if (!duration_set)
                    {
                        duration_seconds = parsed_value;
                        duration_set = true;
                    }
                    else if (!channel_set)
                    {
                        channel = parsed_value;
                        channel_set = true;
                    }
                }
                LOG_INFO("MP4ControlSocket: START requested, path='" << path << "' duration="
                                                                       << duration_seconds
                                                                       << "s channel=" << channel);
                bool recorder_active = false;
                if (channel >= 0 && channel < NUM_VIDEO_CHANNELS)
                {
                    recorder_active = global_mp4_recorders[channel].isActive();
                }
                if (!recorder_active)
                {
                    cancel_stop_timer(channel);
                }

                bool ok = !path.empty() && start_recording(path, channel);
                if (!ok) {
                    LOG_ERROR("MP4ControlSocket: START failed for path '" << path << "'");
                } else {
                    write_channel_state_file(channel, path, duration_seconds);
                    if (duration_seconds > 0) {
                        schedule_stop_timer(channel, duration_seconds);
                    }
                }
            } else if (op == "STOP") {
                int stop_channel = -1;
                std::string token;
                while (iss >> token)
                {
                    std::string key;
                    std::string value = token;
                    auto eq_pos = token.find('=');
                    if (eq_pos != std::string::npos)
                    {
                        key = token.substr(0, eq_pos);
                        value = token.substr(eq_pos + 1);
                    }
                    int parsed_value = 0;
                    if (!parse_int_token(value, parsed_value))
                    {
                        LOG_WARN("MP4ControlSocket: ignoring non-numeric token '" << token
                                                                                   << "' in STOP command");
                        continue;
                    }
                    if (!key.empty() && key != "ch" && key != "channel")
                    {
                        LOG_WARN("MP4ControlSocket: unrecognized key '" << key
                                                                       << "' in STOP command");
                        continue;
                    }
                    stop_channel = parsed_value;
                    break;
                }

                if (stop_channel >= 0)
                {
                    LOG_INFO("MP4ControlSocket: STOP requested for channel " << stop_channel);
                    cancel_stop_timer(stop_channel);
                    stop_recording(stop_channel);
                }
                else
                {
                    LOG_INFO("MP4ControlSocket: STOP requested for all channels");
                    cancel_all_stop_timers();
                    stop_all_recordings();
                }
            } else {
                LOG_ERROR("MP4ControlSocket: unknown command '" << op << "'");
            }
        }

        ::close(fd);
    }

    cancel_all_stop_timers();
    stop_all_recordings();
    ::unlink(FIFO_PATH);
}
