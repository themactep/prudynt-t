#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "globals.hpp"
#include "MP4Muxer.hpp"

class HLSWriter : public LiveFrameSink,
                  public AudioFrameSink,
                  public std::enable_shared_from_this<HLSWriter>
{
public:
    static std::shared_ptr<HLSWriter> create(int video_channel,
                                             int audio_channel,
                                             const std::string &output_dir,
                                             double target_segment_seconds = 1.5,
                                             size_t max_segments = 8);

    ~HLSWriter() override;

    void onFrame(std::shared_ptr<std::vector<uint8_t>> sample,
                 bool isKey,
                 int64_t pts_ms) override;

    void onAudioFrame(const AudioFrame &frame) override;

private:
    HLSWriter(int video_channel,
              int audio_channel,
              std::string output_dir,
              double target_segment_seconds,
              size_t max_segments);

    void attach_sinks();
    void detach_sinks();

    bool ensure_muxer_ready();
    void destroy_muxer();
    void write_init_segment(const std::vector<uint8_t> &init_segment);

    void append_fragment(std::vector<uint8_t> &&fragment,
                         int64_t pts_ms,
                         double duration_seconds,
                         bool is_key);
    void finalize_segment_locked();
    void write_playlist_locked();
#if defined(AUDIO_SUPPORT)
    int64_t normalize_audio_pts(int64_t pts_ms);
    void reset_audio_pts();
#endif

    struct SegmentInfo {
        uint64_t sequence = 0;
        double duration = 0.0;
        std::string filename;
    };

    const int video_channel_;
    const int audio_channel_;
    const std::string output_dir_;
    const std::string playlist_path_;
    const std::string init_path_;
    const double target_segment_seconds_;
    const size_t max_segments_;

    std::mutex muxer_mutex_;
    MP4Muxer *mp4_muxer_ = nullptr;
    bool muxer_ready_ = false;

    std::mutex segment_mutex_;
    std::vector<uint8_t> current_segment_;
    int64_t segment_start_pts_ms_ = -1;
    int64_t segment_last_pts_ms_ = -1;
    bool segment_has_keyframe_ = false;
    bool waiting_for_first_keyframe_ = true;
    uint64_t next_segment_sequence_ = 1;
    std::deque<SegmentInfo> playlist_;

    int64_t last_video_pts_ms_ = -1;
    int64_t last_audio_pts_ms_ = -1;

#if defined(AUDIO_SUPPORT)
    std::atomic<bool> audio_attached_{false};
    int64_t audio_pts_base_ms_ = -1;
    int64_t audio_default_step_ms_ = 20;
    std::mutex audio_pts_mutex_;
#endif
};
