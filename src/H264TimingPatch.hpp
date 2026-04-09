#ifndef H264_TIMING_PATCH_HPP
#define H264_TIMING_PATCH_HPP

#include <cstdint>
#include <vector>

bool patch_h264_sps_timing(std::vector<uint8_t> &nal, int fps, uint8_t target_level_idc = 0);

#endif
