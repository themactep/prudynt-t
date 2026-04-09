#include "H264TimingPatch.hpp"

#include <algorithm>
#include <cstddef>

namespace
{
class BitReader
{
public:
    explicit BitReader(const std::vector<uint8_t> &data) : data_(data) {}

    bool read_bit(uint32_t &bit)
    {
        if (bit_pos_ >= data_.size() * 8ULL)
            return false;
        const size_t byte_pos = bit_pos_ / 8ULL;
        const int shift = 7 - static_cast<int>(bit_pos_ % 8ULL);
        bit = (data_[byte_pos] >> shift) & 0x01U;
        ++bit_pos_;
        return true;
    }

    bool read_bits(int count, uint32_t &value)
    {
        value = 0;
        for (int i = 0; i < count; ++i)
        {
            uint32_t bit = 0;
            if (!read_bit(bit))
                return false;
            value = (value << 1U) | bit;
        }
        return true;
    }

    bool read_ue(uint32_t &value)
    {
        int leading_zero_bits = -1;
        uint32_t bit = 0;
        do
        {
            if (!read_bit(bit))
                return false;
            ++leading_zero_bits;
        } while (bit == 0);

        if (leading_zero_bits == 0)
        {
            value = 0;
            return true;
        }

        uint32_t suffix = 0;
        if (!read_bits(leading_zero_bits, suffix))
            return false;
        value = ((1U << leading_zero_bits) - 1U) + suffix;
        return true;
    }

    bool read_se(int32_t &value)
    {
        uint32_t code_num = 0;
        if (!read_ue(code_num))
            return false;
        value = (code_num & 1U) ? static_cast<int32_t>((code_num + 1U) / 2U)
                                : -static_cast<int32_t>(code_num / 2U);
        return true;
    }

    size_t bit_pos() const
    {
        return bit_pos_;
    }

private:
    const std::vector<uint8_t> &data_;
    size_t bit_pos_{0};
};

void write_bits(std::vector<uint8_t> &data, size_t bit_pos, uint32_t value, int count)
{
    for (int i = count - 1; i >= 0; --i, ++bit_pos)
    {
        const size_t byte_pos = bit_pos / 8ULL;
        const int shift = 7 - static_cast<int>(bit_pos % 8ULL);
        const uint8_t mask = static_cast<uint8_t>(1U << shift);
        if ((value >> i) & 0x01U)
            data[byte_pos] |= mask;
        else
            data[byte_pos] &= static_cast<uint8_t>(~mask);
    }
}

bool skip_hrd_parameters(BitReader &br)
{
    uint32_t cpb_cnt_minus1 = 0;
    if (!br.read_ue(cpb_cnt_minus1))
        return false;

    uint32_t tmp = 0;
    if (!br.read_bits(4, tmp) || !br.read_bits(4, tmp))
        return false;

    for (uint32_t sched_sel_idx = 0; sched_sel_idx <= cpb_cnt_minus1; ++sched_sel_idx)
    {
        if (!br.read_ue(tmp) || !br.read_ue(tmp))
            return false;
        if (!br.read_bits(1, tmp))
            return false;
    }

    return br.read_bits(5, tmp) && br.read_bits(5, tmp)
        && br.read_bits(5, tmp) && br.read_bits(5, tmp);
}

bool skip_scaling_list(BitReader &br, int size_of_list)
{
    int32_t last_scale = 8;
    int32_t next_scale = 8;
    for (int j = 0; j < size_of_list; ++j)
    {
        if (next_scale != 0)
        {
            int32_t delta_scale = 0;
            if (!br.read_se(delta_scale))
                return false;
            next_scale = (last_scale + delta_scale + 256) % 256;
        }
        last_scale = (next_scale == 0) ? last_scale : next_scale;
    }
    return true;
}

std::vector<uint8_t> ebsp_to_rbsp(const std::vector<uint8_t> &ebsp)
{
    std::vector<uint8_t> rbsp;
    rbsp.reserve(ebsp.size());
    for (size_t i = 0; i < ebsp.size(); ++i)
    {
        if (i + 2 < ebsp.size() && ebsp[i] == 0x00 && ebsp[i + 1] == 0x00 && ebsp[i + 2] == 0x03)
        {
            rbsp.push_back(0x00);
            rbsp.push_back(0x00);
            i += 2;
            continue;
        }
        rbsp.push_back(ebsp[i]);
    }
    return rbsp;
}

std::vector<uint8_t> rbsp_to_ebsp(const std::vector<uint8_t> &rbsp)
{
    std::vector<uint8_t> ebsp;
    ebsp.reserve(rbsp.size() + rbsp.size() / 16);
    int zero_count = 0;
    for (uint8_t byte : rbsp)
    {
        if (zero_count >= 2 && byte <= 0x03)
        {
            ebsp.push_back(0x03);
            zero_count = 0;
        }

        ebsp.push_back(byte);
        if (byte == 0x00)
            ++zero_count;
        else
            zero_count = 0;
    }
    return ebsp;
}
}

bool patch_h264_sps_timing(std::vector<uint8_t> &nal, int fps, uint8_t target_level_idc)
{
    if (nal.empty() || (nal[0] & 0x1FU) != 7U || fps <= 0)
        return false;

    std::vector<uint8_t> rbsp = ebsp_to_rbsp(nal);
    if (rbsp.size() < 4)
        return false;
    constexpr size_t nal_header_bits = 8;

    uint32_t tmp = 0;
    const uint32_t profile_idc = rbsp[1];
    if (target_level_idc != 0 && rbsp.size() > 3)
        rbsp[3] = target_level_idc;

    BitReader br(rbsp);
    if (!br.read_bits(static_cast<int>(nal_header_bits), tmp)
        || !br.read_bits(8, tmp)
        || !br.read_bits(8, tmp)
        || !br.read_bits(8, tmp))
        return false;
    if (!br.read_ue(tmp))
        return false;

    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244
        || profile_idc == 44 || profile_idc == 83 || profile_idc == 86 || profile_idc == 118
        || profile_idc == 128 || profile_idc == 138 || profile_idc == 139 || profile_idc == 134
        || profile_idc == 135)
    {
        uint32_t chroma_format_idc = 0;
        if (!br.read_ue(chroma_format_idc))
            return false;
        if (chroma_format_idc == 3 && !br.read_bits(1, tmp))
            return false;
        if (!br.read_ue(tmp) || !br.read_ue(tmp) || !br.read_bits(1, tmp) || !br.read_bits(1, tmp))
            return false;
        if (tmp)
        {
            const int scaling_list_count = (chroma_format_idc != 3) ? 8 : 12;
            for (int i = 0; i < scaling_list_count; ++i)
            {
                if (!br.read_bits(1, tmp))
                    return false;
                if (tmp)
                {
                    if (!skip_scaling_list(br, i < 6 ? 16 : 64))
                        return false;
                }
            }
        }
    }

    if (!br.read_ue(tmp))
        return false;

    uint32_t pic_order_cnt_type = 0;
    if (!br.read_ue(pic_order_cnt_type))
        return false;
    if (pic_order_cnt_type == 0)
    {
        if (!br.read_ue(tmp))
            return false;
    }
    else if (pic_order_cnt_type == 1)
    {
        int32_t stmp = 0;
        if (!br.read_bits(1, tmp) || !br.read_se(stmp) || !br.read_se(stmp) || !br.read_ue(tmp))
            return false;
        for (uint32_t i = 0; i < tmp; ++i)
        {
            if (!br.read_se(stmp))
                return false;
        }
    }

    if (!br.read_ue(tmp) || !br.read_bits(1, tmp) || !br.read_ue(tmp) || !br.read_ue(tmp))
        return false;

    if (!br.read_bits(1, tmp))
        return false;
    if (tmp == 0 && !br.read_bits(1, tmp))
        return false;
    if (!br.read_bits(1, tmp))
        return false;

    if (!br.read_bits(1, tmp))
        return false;
    if (tmp)
    {
        if (!br.read_ue(tmp) || !br.read_ue(tmp) || !br.read_ue(tmp) || !br.read_ue(tmp))
            return false;
    }

    uint32_t vui_parameters_present_flag = 0;
    if (!br.read_bits(1, vui_parameters_present_flag) || vui_parameters_present_flag == 0)
        return false;

    if (!br.read_bits(1, tmp))
        return false;
    if (tmp)
    {
        if (!br.read_bits(8, tmp))
            return false;
        if (tmp == 255)
        {
            if (!br.read_bits(16, tmp) || !br.read_bits(16, tmp))
                return false;
        }
    }

    if (!br.read_bits(1, tmp))
        return false;
    if (tmp && !br.read_bits(1, tmp))
        return false;

    if (!br.read_bits(1, tmp))
        return false;
    if (tmp)
    {
        if (!br.read_bits(3, tmp) || !br.read_bits(1, tmp) || !br.read_bits(1, tmp))
            return false;
        if (tmp)
        {
            if (!br.read_bits(8, tmp) || !br.read_bits(8, tmp)
                || !br.read_bits(8, tmp))
                return false;
        }
    }

    if (!br.read_bits(1, tmp))
        return false;
    if (tmp)
    {
        if (!br.read_ue(tmp) || !br.read_ue(tmp))
            return false;
    }

    size_t timing_info_flag_pos = br.bit_pos();
    uint32_t timing_info_present_flag = 0;
    if (!br.read_bits(1, timing_info_present_flag) || timing_info_present_flag == 0)
        return false;

    const size_t num_units_in_tick_pos = br.bit_pos();
    uint32_t num_units_in_tick = 1;
    uint32_t time_scale = static_cast<uint32_t>(fps * 2);
    const uint32_t fixed_frame_rate_flag = 1;
    write_bits(rbsp, timing_info_flag_pos, 1, 1);
    write_bits(rbsp, num_units_in_tick_pos, num_units_in_tick, 32);
    write_bits(rbsp, num_units_in_tick_pos + 32, time_scale, 32);
    write_bits(rbsp, num_units_in_tick_pos + 64, fixed_frame_rate_flag, 1);

    nal = rbsp_to_ebsp(rbsp);
    return true;
}
