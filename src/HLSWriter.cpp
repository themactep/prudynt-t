#include "HLSWriter.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <faac.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "Config.hpp"
#include "IMPEncoder.hpp"
#include "Logger.hpp"
#include "MP4MuxerFactory.hpp"

extern std::shared_ptr<CFG> cfg;

using namespace std::chrono_literals;

namespace {
constexpr const char *kInitFilename = "init.mp4";
constexpr const char *kPlaylistFilename = "playlist.m3u8";
constexpr const char *kSegmentPrefix = "segment_";
constexpr const char *kSegmentExt = ".m4s";

std::vector<uint8_t> build_avc_decoder_config(const std::vector<uint8_t> &sps,
                                              const std::vector<uint8_t> &pps)
{
    if (sps.size() < 4 || pps.empty())
    {
        return {};
    }
    std::vector<uint8_t> avcC;
    avcC.reserve(11 + sps.size() + pps.size());
    avcC.push_back(0x01);          // configurationVersion
    avcC.push_back(sps[1]);        // AVCProfileIndication
    avcC.push_back(sps[2]);        // profile_compatibility
    avcC.push_back(sps[3]);        // AVCLevelIndication
    avcC.push_back(0xFF);          // reserved + lengthSizeMinusOne
    avcC.push_back(0xE1);          // reserved + numOfSequenceParameterSets
    auto append_u16 = [&avcC](uint16_t value) {
        avcC.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        avcC.push_back(static_cast<uint8_t>(value & 0xFF));
    };
    append_u16(static_cast<uint16_t>(sps.size()));
    avcC.insert(avcC.end(), sps.begin(), sps.end());
    avcC.push_back(0x01);          // numOfPictureParameterSets
    append_u16(static_cast<uint16_t>(pps.size()));
    avcC.insert(avcC.end(), pps.begin(), pps.end());
    return avcC;
}

std::vector<uint8_t> build_aac_decoder_config()
{
#if defined(AUDIO_SUPPORT)
    if (!cfg->audio.input_enabled || std::strcmp(cfg->audio.input_format, "AAC") != 0)
    {
        return {};
    }
    unsigned long inputSamples = 0;
    unsigned long outputBufferSize = 0;
    void *handle = faacEncOpen(cfg->audio.input_sample_rate,
                               cfg->audio.force_stereo ? 2 : 1,
                               &inputSamples,
                               &outputBufferSize);
    if (!handle)
    {
        return {};
    }
    unsigned char *decoder_info = nullptr;
    unsigned long decoder_info_len = 0;
    std::vector<uint8_t> config;
    if (faacEncGetDecoderSpecificInfo(handle, &decoder_info, &decoder_info_len) == 0 &&
        decoder_info && decoder_info_len)
    {
        config.assign(decoder_info, decoder_info + decoder_info_len);
        free(decoder_info);
    }
    faacEncClose(handle);
    return config;
#else
    return {};
#endif
}

std::string segment_filename(uint64_t seq)
{
    std::ostringstream oss;
    oss << kSegmentPrefix << std::setw(5) << std::setfill('0') << seq << kSegmentExt;
    return oss.str();
}
}

std::shared_ptr<HLSWriter> HLSWriter::create(int video_channel,
                                             int audio_channel,
                                             const std::string &output_dir,
                                             double target_segment_seconds,
                                             size_t max_segments)
{
    auto writer = std::shared_ptr<HLSWriter>(
        new HLSWriter(video_channel, audio_channel, output_dir, target_segment_seconds, max_segments));
    writer->attach_sinks();
    return writer;
}

HLSWriter::HLSWriter(int video_channel,
                     int audio_channel,
                     std::string output_dir,
                     double target_segment_seconds,
                     size_t max_segments)
    : video_channel_(video_channel),
      audio_channel_(audio_channel),
      output_dir_(std::move(output_dir)),
      playlist_path_(std::filesystem::path(output_dir_) / kPlaylistFilename),
      init_path_(std::filesystem::path(output_dir_) / kInitFilename),
      target_segment_seconds_(target_segment_seconds),
      max_segments_(std::max<size_t>(max_segments, 3))
{
    std::error_code ec;
    std::filesystem::create_directories(output_dir_, ec);
    if (ec)
    {
        LOG_ERROR("HLSWriter: failed to create directory " << output_dir_ << ": " << ec.message());
    }
    else
    {
        std::error_code cleanup_ec;
        for (const auto &entry : std::filesystem::directory_iterator(output_dir_, cleanup_ec))
        {
            if (cleanup_ec)
            {
                break;
            }
            if (!entry.is_regular_file())
            {
                continue;
            }
            auto filename = entry.path().filename().string();
            bool is_segment = filename.rfind(kSegmentPrefix, 0) == 0 &&
                entry.path().extension() == kSegmentExt;
            if (filename == kPlaylistFilename || filename == kInitFilename || is_segment)
            {
                std::error_code remove_ec;
                std::filesystem::remove(entry.path(), remove_ec);
            }
        }
    }
#if defined(AUDIO_SUPPORT)
    reset_audio_pts();
    if (cfg->audio.input_sample_rate > 0)
    {
        audio_default_step_ms_ = static_cast<int64_t>((1024 * 1000LL) / cfg->audio.input_sample_rate);
    }
    if (audio_default_step_ms_ <= 0)
    {
        audio_default_step_ms_ = 20;
    }
#endif
}

HLSWriter::~HLSWriter()
{
    detach_sinks();
    destroy_muxer();
}

void HLSWriter::attach_sinks()
{
    auto self = shared_from_this();
    auto video = global_video[video_channel_];
    if (video)
    {
        std::lock_guard<std::mutex> lock(video->live_frame_sinks_mutex);
        video->live_frame_sinks.push_back(self);
        video->live_frame_sink_count.store(static_cast<int>(video->live_frame_sinks.size()),
                                           std::memory_order_relaxed);
        video->mp4_required_idr_ts.store(video->mp4_last_idr_ts.load(std::memory_order_relaxed),
                                         std::memory_order_relaxed);
        video->mp4_waiting_for_idr.store(true, std::memory_order_relaxed);
        video->mp4_last_idr_request_ms.store(0, std::memory_order_relaxed);
        IMP_Encoder_RequestIDR(video_channel_);
    }
#if defined(AUDIO_SUPPORT)
    if (audio_channel_ >= 0 && audio_channel_ < NUM_AUDIO_CHANNELS)
    {
        auto audio = global_audio[audio_channel_];
        if (audio)
        {
            std::lock_guard<std::mutex> lock(audio->frame_sinks_mutex);
            audio->frame_sinks.push_back(self);
            audio->frame_sink_count.store(static_cast<int>(audio->frame_sinks.size()), std::memory_order_relaxed);
            audio->should_grab_frames.notify_one();
            audio_attached_.store(true, std::memory_order_relaxed);
        }
    }
#endif
}

void HLSWriter::detach_sinks()
{
    auto video = global_video[video_channel_];
    if (video)
    {
        std::lock_guard<std::mutex> lock(video->live_frame_sinks_mutex);
        auto &vec = video->live_frame_sinks;
        vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const std::weak_ptr<LiveFrameSink> &weak) {
                      return weak.expired() || weak.lock().get() == this;
                  }),
                  vec.end());
        video->live_frame_sink_count.store(static_cast<int>(vec.size()), std::memory_order_relaxed);
    }
#if defined(AUDIO_SUPPORT)
    if (audio_channel_ >= 0 && audio_channel_ < NUM_AUDIO_CHANNELS)
    {
        auto audio = global_audio[audio_channel_];
        if (audio)
        {
            std::lock_guard<std::mutex> lock(audio->frame_sinks_mutex);
            auto &vec = audio->frame_sinks;
            vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const std::weak_ptr<AudioFrameSink> &weak) {
                          return weak.expired() || weak.lock().get() == this;
                      }),
                      vec.end());
            audio->frame_sink_count.store(static_cast<int>(vec.size()), std::memory_order_relaxed);
        }
    }
#endif
}

void HLSWriter::destroy_muxer()
{
    std::lock_guard<std::mutex> lock(muxer_mutex_);
    if (mp4_muxer_)
    {
        DestroyMP4Muxer(mp4_muxer_);
        mp4_muxer_ = nullptr;
    }
    muxer_ready_ = false;
#if defined(AUDIO_SUPPORT)
    reset_audio_pts();
#endif
}

bool HLSWriter::ensure_muxer_ready()
{
    std::lock_guard<std::mutex> lock(muxer_mutex_);
    if (muxer_ready_ && mp4_muxer_)
    {
        return true;
    }
    auto video_state = global_video[video_channel_];
    if (!video_state)
    {
        return false;
    }
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    {
        std::lock_guard<std::mutex> cfg_lock(video_state->codec_config_mutex);
        if (video_state->have_sps)
        {
            sps = video_state->latest_sps;
        }
        if (video_state->have_pps)
        {
            pps = video_state->latest_pps;
        }
    }
    if (sps.empty() || pps.empty())
    {
        return false;
    }
    auto avc_config = build_avc_decoder_config(sps, pps);
    if (avc_config.empty())
    {
        return false;
    }
    auto muxer = CreateMP4Muxer();
    if (!muxer)
    {
        LOG_ERROR("HLSWriter: failed to create MP4 muxer");
        return false;
    }
    MP4Muxer::InitParams params;
    params.width = cfg->stream0.width;
    params.height = cfg->stream0.height;
    params.fps = cfg->stream0.fps;
    params.avcC = avc_config;
#if defined(AUDIO_SUPPORT)
    params.sampleRate = cfg->audio.input_sample_rate;
    params.channels = (cfg->audio.input_enabled && std::strcmp(cfg->audio.input_format, "AAC") == 0)
        ? (cfg->audio.force_stereo ? 2 : 1)
        : 0;
    if (params.channels > 0)
    {
        params.aacConfig = build_aac_decoder_config();
    }
#endif
    if (!muxer->init(params))
    {
        LOG_ERROR("HLSWriter: MP4 muxer init failed");
        DestroyMP4Muxer(muxer);
        return false;
    }
    auto init_segment = muxer->getInitSegment();
    write_init_segment(init_segment);
    mp4_muxer_ = muxer;
    muxer_ready_ = true;
    waiting_for_first_keyframe_ = true;
    current_segment_.clear();
#if defined(AUDIO_SUPPORT)
    reset_audio_pts();
#endif
    return true;
}

void HLSWriter::write_init_segment(const std::vector<uint8_t> &init_segment)
{
    if (init_segment.empty())
    {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(output_dir_, ec);
    std::ofstream out(init_path_, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
        LOG_ERROR("HLSWriter: failed to write init segment to " << init_path_);
        return;
    }
    out.write(reinterpret_cast<const char *>(init_segment.data()), static_cast<std::streamsize>(init_segment.size()));
}

void HLSWriter::onFrame(std::shared_ptr<std::vector<uint8_t>> sample,
                        bool isKey,
                        int64_t pts_ms)
{
    if (!sample || sample->empty())
    {
        return;
    }
    if (!ensure_muxer_ready())
    {
        return;
    }
    std::vector<uint8_t> fragment;
    {
        std::lock_guard<std::mutex> lock(muxer_mutex_);
        if (!mp4_muxer_)
        {
            return;
        }
        fragment = mp4_muxer_->muxVideo(sample->data(), sample->size(), pts_ms, isKey);
    }
    if (fragment.empty())
    {
        return;
    }
    if (waiting_for_first_keyframe_ && !isKey)
    {
        return;
    }
    if (waiting_for_first_keyframe_ && isKey)
    {
        waiting_for_first_keyframe_ = false;
    }
    double duration = 0.0;
    if (last_video_pts_ms_ >= 0 && pts_ms > last_video_pts_ms_)
    {
        duration = static_cast<double>(pts_ms - last_video_pts_ms_) / 1000.0;
    }
    else if (cfg->stream0.fps > 0)
    {
        duration = 1.0 / cfg->stream0.fps;
    }
    last_video_pts_ms_ = pts_ms;
    append_fragment(std::move(fragment), pts_ms, duration, isKey);
}

void HLSWriter::onAudioFrame(const AudioFrame &frame)
{
#if defined(AUDIO_SUPPORT)
    if (audio_channel_ < 0 || frame.data.empty())
    {
        return;
    }
    if (!ensure_muxer_ready())
    {
        return;
    }
    int64_t pts_ms = frame.time.tv_sec * 1000LL + frame.time.tv_usec / 1000LL;
    pts_ms = normalize_audio_pts(pts_ms);
    std::vector<uint8_t> fragment;
    {
        std::lock_guard<std::mutex> lock(muxer_mutex_);
        if (!mp4_muxer_)
        {
            return;
        }
        fragment = mp4_muxer_->muxAudio(frame.data.data(), frame.data.size(), pts_ms);
    }
    if (fragment.empty())
    {
        return;
    }
    if (waiting_for_first_keyframe_)
    {
        return;
    }
    double duration = 0.0;
    if (last_audio_pts_ms_ >= 0 && pts_ms > last_audio_pts_ms_)
    {
        duration = static_cast<double>(pts_ms - last_audio_pts_ms_) / 1000.0;
    }
    else if (cfg->audio.input_sample_rate > 0)
    {
        duration = 1024.0 / static_cast<double>(cfg->audio.input_sample_rate);
    }
    last_audio_pts_ms_ = pts_ms;
    append_fragment(std::move(fragment), pts_ms, duration, false);
#else
    (void)frame;
#endif
}

#if defined(AUDIO_SUPPORT)
void HLSWriter::reset_audio_pts()
{
    std::lock_guard<std::mutex> lock(audio_pts_mutex_);
    audio_pts_base_ms_ = -1;
    last_audio_pts_ms_ = -1;
}

int64_t HLSWriter::normalize_audio_pts(int64_t pts_ms)
{
    std::lock_guard<std::mutex> lock(audio_pts_mutex_);
    if (audio_pts_base_ms_ < 0)
    {
        audio_pts_base_ms_ = pts_ms;
    }
    pts_ms -= audio_pts_base_ms_;
    if (pts_ms < 0)
    {
        pts_ms = 0;
    }
    if (last_audio_pts_ms_ >= 0 && pts_ms <= last_audio_pts_ms_)
    {
        pts_ms = last_audio_pts_ms_ + audio_default_step_ms_;
    }
    return pts_ms;
}
#endif

void HLSWriter::append_fragment(std::vector<uint8_t> &&fragment,
                                int64_t pts_ms,
                                double duration_seconds,
                                bool is_key)
{
    (void)duration_seconds;
    if (fragment.empty())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(segment_mutex_);
    if (is_key && segment_has_keyframe_ &&
        segment_last_pts_ms_ >= 0 && segment_start_pts_ms_ >= 0 &&
        (segment_last_pts_ms_ - segment_start_pts_ms_) / 1000.0 >= target_segment_seconds_)
    {
        finalize_segment_locked();
    }
    if (current_segment_.empty())
    {
        segment_start_pts_ms_ = pts_ms;
        segment_last_pts_ms_ = pts_ms;
    }
    if (is_key)
    {
        if (segment_has_keyframe_ && !current_segment_.empty())
        {
            finalize_segment_locked();
        }
        segment_has_keyframe_ = true;
    }
    current_segment_.insert(current_segment_.end(), fragment.begin(), fragment.end());
    if (pts_ms > segment_last_pts_ms_)
    {
        segment_last_pts_ms_ = pts_ms;
    }
    bool duration_ready = false;
    if (segment_start_pts_ms_ >= 0 && segment_last_pts_ms_ >= segment_start_pts_ms_)
    {
        double seg_duration = static_cast<double>(segment_last_pts_ms_ - segment_start_pts_ms_) / 1000.0;
        if (seg_duration >= target_segment_seconds_ && segment_has_keyframe_)
        {
            duration_ready = true;
        }
    }
    if (duration_ready)
    {
        finalize_segment_locked();
    }
}

void HLSWriter::finalize_segment_locked()
{
    if (current_segment_.empty() || !segment_has_keyframe_ || segment_start_pts_ms_ < 0 || segment_last_pts_ms_ < 0)
    {
        current_segment_.clear();
        segment_start_pts_ms_ = -1;
        segment_last_pts_ms_ = -1;
        segment_has_keyframe_ = false;
        return;
    }
    double duration = static_cast<double>(segment_last_pts_ms_ - segment_start_pts_ms_) / 1000.0;
    if (duration <= 0)
    {
        duration = target_segment_seconds_;
    }
    const uint64_t seq = next_segment_sequence_++;
    const auto filename = segment_filename(seq);
    const auto path = std::filesystem::path(output_dir_) / filename;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
        LOG_ERROR("HLSWriter: failed to write segment " << path);
    }
    else
    {
        out.write(reinterpret_cast<const char *>(current_segment_.data()),
                  static_cast<std::streamsize>(current_segment_.size()));
        out.close();
    }
    playlist_.push_back({seq, duration, filename});
    while (playlist_.size() > max_segments_)
    {
        auto &seg = playlist_.front();
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(output_dir_) / seg.filename, ec);
        playlist_.pop_front();
    }
    write_playlist_locked();
    current_segment_.clear();
    segment_start_pts_ms_ = -1;
    segment_last_pts_ms_ = -1;
    segment_has_keyframe_ = false;
}

void HLSWriter::write_playlist_locked()
{
    if (playlist_.empty())
    {
        return;
    }
    double max_duration = 0.0;
    for (const auto &seg : playlist_)
    {
        max_duration = std::max(max_duration, seg.duration);
    }
    int target_duration = static_cast<int>(std::ceil(std::max(1.0, max_duration)));
    std::ofstream out(playlist_path_, std::ios::trunc);
    if (!out.is_open())
    {
        LOG_ERROR("HLSWriter: failed to write playlist " << playlist_path_);
        return;
    }
    out << "#EXTM3U\n";
    out << "#EXT-X-VERSION:7\n";
    out << "#EXT-X-TARGETDURATION:" << target_duration << "\n";
    out << "#EXT-X-MEDIA-SEQUENCE:" << playlist_.front().sequence << "\n";
    out << "#EXT-X-PLAYLIST-TYPE:EVENT\n";
    out << "#EXT-X-MAP:URI=\"" << kInitFilename << "\"\n";
    out << std::fixed << std::setprecision(3);
    for (const auto &seg : playlist_)
    {
        out << "#EXTINF:" << seg.duration << ",\n";
        out << seg.filename << "\n";
    }
}
