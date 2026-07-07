#include "globals.hpp"

bool global_osd_thread_signal = false;
bool global_reload_osd = false;
std::array<MP4Recorder, NUM_VIDEO_CHANNELS> global_mp4_recorders;
std::atomic<bool> global_force_video_active{false};
std::atomic<int> global_mp4_active_recorders{0};
std::atomic<bool> global_shutdown_requested{false};
DayNightHistoryBuffer global_daynight_history;
