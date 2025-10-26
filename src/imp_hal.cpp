#include "imp_hal.hpp"
#include "imp_hal.hpp"
#include "Config.hpp"
#include "Logger.hpp"
#include <cstring>
#include <dlfcn.h>
#include <imp/imp_isp.h>


extern void MakeTables(int q, uint8_t *lqt, uint8_t *cqt);

namespace hal {

static PlatformCaps g_caps = {
    // Encoder capabilities
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    .has_h265 = true,
    .has_capped_quality = true,
    .has_capped_vbr = true,
    .has_ip_pb_delta = true,
    .has_bufshare = true,
    .has_jpeg_set_qtable = false,
    .has_smart_rc = true,
    .has_super_frm = true,
    .has_intra_refresh = true,
#elif defined(PLATFORM_T30)
    .has_h265 = true,
    .has_capped_quality = false,
    .has_capped_vbr = false,
    .has_ip_pb_delta = false,
    .has_bufshare = false,
    .has_jpeg_set_qtable = true,
    .has_smart_rc = false,
    .has_super_frm = false,
    .has_intra_refresh = false,
#else
    .has_h265 = false,
    .has_capped_quality = false,
    .has_capped_vbr = false,
    .has_ip_pb_delta = false,
    .has_bufshare = false,
    .has_jpeg_set_qtable = true,
    .has_smart_rc = false,
    .has_super_frm = false,
    .has_intra_refresh = false,
#endif

    // Audio capabilities
#if defined(PLATFORM_T23) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    .has_audio_aec_channel = true,
#else
    .has_audio_aec_channel = false,
#endif

#if defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T30) || defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    .has_audio_agc = true,
    .has_audio_hpf = true,
    .has_audio_ns = true,
#else
    .has_audio_agc = false,
    .has_audio_hpf = false,
    .has_audio_ns = false,
#endif

#if defined(PLATFORM_T21) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
    .has_audio_alc = true,
#else
    .has_audio_alc = false,
#endif

    // ISP capabilities
#if \!defined(PLATFORM_T21) && \!defined(PLATFORM_T40) && \!defined(PLATFORM_T41)
    .has_isp_sinter = true,
#else
    .has_isp_sinter = false,
#endif

#if \!defined(PLATFORM_T40) && \!defined(PLATFORM_T41)
    .has_isp_temper = true,
#else
    .has_isp_temper = false,
#endif

#if defined(PLATFORM_T23) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
    .has_isp_hue = true,
    .has_isp_defog = true,
    .has_isp_backlight_comp = true,
#else
    .has_isp_hue = false,
    .has_isp_defog = false,
    .has_isp_backlight_comp = false,
#endif

#if defined(PLATFORM_T31) || defined(PLATFORM_C100)
    .has_isp_dpc = true,
#else
    .has_isp_dpc = false,
#endif

#if defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T30) || defined(PLATFORM_T31) || defined(PLATFORM_C100)
    .has_isp_drc = true,
    .has_isp_highlight_depress = true,
#else
    .has_isp_drc = false,
    .has_isp_highlight_depress = false,
#endif

#if \!defined(PLATFORM_T21) && \!defined(PLATFORM_T40) && \!defined(PLATFORM_T41)
    .has_isp_ae_comp = true,
#else
    .has_isp_ae_comp = false,
#endif

#if \!defined(PLATFORM_T40) && \!defined(PLATFORM_T41)
    .has_isp_max_gain = true,
#else
    .has_isp_max_gain = false,
#endif

    // OSD capabilities
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    .has_osd_region_invert = true,
#else
    .has_osd_region_invert = false,
#endif

    // System capabilities
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
    .uses_xburst2 = true,
    .uses_kernel_4 = true,
#else
    .uses_xburst2 = false,
    .uses_kernel_4 = false,
#endif
};

const PlatformCaps& caps() { return g_caps; }

void set_jpeg_quality_qtable(int encChn, int quality, const char* cpu_hint)
{
    if (quality < 1 || quality > 100) return;

    if (!caps().has_jpeg_set_qtable) {
        // Not supported on T31/T40/T41/C100 SDKs (JPEG tables fixed by SDK)
        return;
    }

#if !(defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41))
    IMPEncoderJpegeQl pst{};
    if (cpu_hint && strncmp(cpu_hint, "T10", 3) == 0) {
        pst.user_ql_en = 0;
        LOG_DEBUG("HAL JPEG: default quantization (T10 family)");
    } else {
        uint8_t lqt[64], cqt[64];
        MakeTables(quality, lqt, cqt);
        for (int i = 0; i < 64; ++i) pst.qmem_table[i] = lqt[i];
        for (int i = 0; i < 64; ++i) pst.qmem_table[64 + i] = cqt[i];
        pst.user_ql_en = 1;
        LOG_DEBUG("HAL JPEG: custom quantization tables set");
    }
    IMP_Encoder_SetJpegeQl(encChn, &pst);
#else
    (void)encChn; (void)cpu_hint; // no-op
#endif
}

int maybe_enable_bufshare(int jpegEncGrp, int srcEncChn, bool allow_shared)
{
    if (!allow_shared) return 0;
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    typedef int (*pfn_setbufshare)(int,int);
    void* handle = dlopen(nullptr, RTLD_LAZY);
    pfn_setbufshare fn = handle ? reinterpret_cast<pfn_setbufshare>(dlsym(handle, "IMP_Encoder_SetbufshareChn")) : nullptr;
    if (fn) {
        int ret = fn(jpegEncGrp, srcEncChn);
        LOG_DEBUG_OR_ERROR(ret, "IMP_Encoder_SetbufshareChn(" << jpegEncGrp << ", " << srcEncChn << ")");
        return ret;
    } else {
        LOG_DEBUG("IMP_Encoder_SetbufshareChn not available; skipping bufshare");
        return 0;
    }
#else
    (void)jpegEncGrp; (void)srcEncChn;
    return 0;
#endif
}

#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
void apply_rc_overrides(IMPEncoderCHNAttr &chnAttr, IMPEncoderRcMode rcMode, const _stream &stream)
{
    auto *rcAttr = &chnAttr.rcAttr;
    int qp_init = stream.qp_init;
    int qp_min  = stream.qp_min;
    int qp_max  = stream.qp_max;
    int ip_delta = stream.ip_delta;
    int pb_delta = stream.pb_delta;
    int max_br = stream.max_bitrate;

    switch (rcMode) {
        case IMP_ENC_RC_MODE_FIXQP:
            if (qp_init >= 0) rcAttr->attrRcMode.attrFixQp.iInitialQP = qp_init;
            break;
        case IMP_ENC_RC_MODE_CBR:
            if (qp_init >= 0) rcAttr->attrRcMode.attrCbr.iInitialQP = qp_init;
            if (qp_min >= 0)  rcAttr->attrRcMode.attrCbr.iMinQP = qp_min;
            if (qp_max >= 0)  rcAttr->attrRcMode.attrCbr.iMaxQP = qp_max;
            if (ip_delta != -1) rcAttr->attrRcMode.attrCbr.iIPDelta = ip_delta;
            if (pb_delta != -1) rcAttr->attrRcMode.attrCbr.iPBDelta = pb_delta;
            break;
        case IMP_ENC_RC_MODE_VBR:
            if (qp_init >= 0) rcAttr->attrRcMode.attrVbr.iInitialQP = qp_init;
            if (qp_min >= 0)  rcAttr->attrRcMode.attrVbr.iMinQP = qp_min;
            if (qp_max >= 0)  rcAttr->attrRcMode.attrVbr.iMaxQP = qp_max;
            if (ip_delta != -1) rcAttr->attrRcMode.attrVbr.iIPDelta = ip_delta;
            if (pb_delta != -1) rcAttr->attrRcMode.attrVbr.iPBDelta = pb_delta;
            if (max_br > 0)     rcAttr->attrRcMode.attrVbr.uMaxBitRate = max_br;
            break;
        case IMP_ENC_RC_MODE_CAPPED_VBR:
            if (qp_init >= 0) rcAttr->attrRcMode.attrCappedVbr.iInitialQP = qp_init;
            if (qp_min >= 0)  rcAttr->attrRcMode.attrCappedVbr.iMinQP = qp_min;
            if (qp_max >= 0)  rcAttr->attrRcMode.attrCappedVbr.iMaxQP = qp_max;
            if (ip_delta != -1) rcAttr->attrRcMode.attrCappedVbr.iIPDelta = ip_delta;
            if (pb_delta != -1) rcAttr->attrRcMode.attrCappedVbr.iPBDelta = pb_delta;
            if (max_br > 0)     rcAttr->attrRcMode.attrCappedVbr.uMaxBitRate = max_br;
            break;
        case IMP_ENC_RC_MODE_CAPPED_QUALITY:
            if (qp_init >= 0) rcAttr->attrRcMode.attrCappedQuality.iInitialQP = qp_init;
            if (qp_min >= 0)  rcAttr->attrRcMode.attrCappedQuality.iMinQP = qp_min;
            if (qp_max >= 0)  rcAttr->attrRcMode.attrCappedQuality.iMaxQP = qp_max;
            if (ip_delta != -1) rcAttr->attrRcMode.attrCappedQuality.iIPDelta = ip_delta;
            if (pb_delta != -1) rcAttr->attrRcMode.attrCappedQuality.iPBDelta = pb_delta;
            if (max_br > 0)     rcAttr->attrRcMode.attrCappedQuality.uMaxBitRate = max_br;
            break;
        default:
            break;
    }
}
#else
void apply_rc_overrides(IMPEncoderCHNAttr &chnAttr, int rcMode, const _stream &stream)
{
    auto *rcAttr = &chnAttr.rcAttr;
    int qp_init = stream.qp_init;
    int qp_min  = stream.qp_min;
    int qp_max  = stream.qp_max;
    int max_br  = stream.max_bitrate;

    if (chnAttr.encAttr.enType == PT_H264) {
        switch (rcMode) {
            case ENC_RC_MODE_FIXQP:
                if (qp_init >= 0) rcAttr->attrRcMode.attrH264FixQp.qp = qp_init;
                break;
            case ENC_RC_MODE_CBR:
                if (qp_min >= 0) rcAttr->attrRcMode.attrH264Cbr.minQp = qp_min;
                if (qp_max >= 0) rcAttr->attrRcMode.attrH264Cbr.maxQp = qp_max;
                break;
            case ENC_RC_MODE_VBR:
                if (qp_min >= 0) rcAttr->attrRcMode.attrH264Vbr.minQp = qp_min;
                if (qp_max >= 0) rcAttr->attrRcMode.attrH264Vbr.maxQp = qp_max;
                if (max_br > 0) rcAttr->attrRcMode.attrH264Vbr.maxBitRate = max_br;
                break;
            case ENC_RC_MODE_SMART:
                if (qp_min >= 0) rcAttr->attrRcMode.attrH264Smart.minQp = qp_min;
                if (qp_max >= 0) rcAttr->attrRcMode.attrH264Smart.maxQp = qp_max;
                if (max_br > 0) rcAttr->attrRcMode.attrH264Smart.maxBitRate = max_br;
                break;
            default:
                break;
        }
    }
#if defined(PLATFORM_T30)
    else if (chnAttr.encAttr.enType == PT_H265) {
        // Only SMART mode used in current code for H265 on T30
        if (qp_min >= 0) rcAttr->attrRcMode.attrH265Smart.minQp = qp_min;
        if (qp_max >= 0) rcAttr->attrRcMode.attrH265Smart.maxQp = qp_max;
        if (max_br > 0) rcAttr->attrRcMode.attrH265Smart.maxBitRate = max_br;
    }
#endif
}
#endif



// ============================================================================
// ISP Tuning HAL Implementation
// ============================================================================

namespace isp {

#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
#define IMPVI IMPVI_MAIN
#endif

int set_brightness(unsigned char val)
{
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
    return IMP_ISP_Tuning_SetBrightness(IMPVI, &val);
#else
    return IMP_ISP_Tuning_SetBrightness(val);
#endif
}

int set_contrast(unsigned char val)
{
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
    return IMP_ISP_Tuning_SetContrast(IMPVI, &val);
#else
    return IMP_ISP_Tuning_SetContrast(val);
#endif
}

int set_saturation(unsigned char val)
{
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
    return IMP_ISP_Tuning_SetSaturation(IMPVI, &val);
#else
    return IMP_ISP_Tuning_SetSaturation(val);
#endif
}

int set_sharpness(unsigned char val)
{
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
    return IMP_ISP_Tuning_SetSharpness(IMPVI, &val);
#else
    return IMP_ISP_Tuning_SetSharpness(val);
#endif
}

int set_sinter_strength(unsigned char val)
{
    if (!caps().has_isp_sinter()) {
        LOG_DEBUG("set_sinter_strength not supported on this platform");
        return 0;
    }
    return IMP_ISP_Tuning_SetSinterStrength(val);
}

int set_temper_strength(unsigned char val)
{
    if (!caps().has_isp_temper()) {
        LOG_DEBUG("set_temper_strength not supported on this platform");
        return 0;
    }
    return IMP_ISP_Tuning_SetTemperStrength(val);
}

int set_hue(unsigned char val)
{
    if (!caps().has_isp_hue()) {
        LOG_DEBUG("set_hue not supported on this platform");
        return 0;
    }
    return IMP_ISP_Tuning_SetBcshHue(val);
}

int set_hflip(bool enable)
{
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
    // T40 uses combined HVFLIP function
    // For now, we'll need to get current vflip state and set both
    // This is a limitation - we can't set H and V independently on T40
    LOG_DEBUG("set_hflip: T40 uses combined HVFLIP - feature limited");
    return 0; // TODO: implement combined flip handling
#else
    IMPISPTuningOpsMode mode = enable ? IMPISP_TUNING_OPS_MODE_ENABLE : IMPISP_TUNING_OPS_MODE_DISABLE;
    return IMP_ISP_Tuning_SetISPHflip(mode);
#endif
}

int set_vflip(bool enable)
{
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
    // T40 uses combined HVFLIP function
    LOG_DEBUG("set_vflip: T40 uses combined HVFLIP - feature limited");
    return 0; // TODO: implement combined flip handling
#else
    IMPISPTuningOpsMode mode = enable ? IMPISP_TUNING_OPS_MODE_ENABLE : IMPISP_TUNING_OPS_MODE_DISABLE;
    return IMP_ISP_Tuning_SetISPVflip(mode);
#endif
}

int set_running_mode(int mode)
{
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
    IMPISPRunningMode m = (IMPISPRunningMode)mode;
    return IMP_ISP_Tuning_SetISPRunningMode(IMPVI, &m);
#else
    return IMP_ISP_Tuning_SetISPRunningMode((IMPISPRunningMode)mode);
#endif
}

int set_isp_bypass(bool enable)
{
    IMPISPTuningOpsMode mode = enable ? IMPISP_TUNING_OPS_MODE_ENABLE : IMPISP_TUNING_OPS_MODE_DISABLE;
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
    return IMP_ISP_Tuning_SetISPBypass(IMPVI, &mode);
#else
    return IMP_ISP_Tuning_SetISPBypass(mode);
#endif
}

int set_anti_flicker(int mode)
{
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
    IMPISPAntiflickerAttr attr;
    attr.mode = (IMPISPAntiflickerMode)mode;
    attr.freq = 50; // Default to 50Hz, could be made configurable
    return IMP_ISP_Tuning_SetAntiFlickerAttr(IMPVI, &attr);
#else
    return IMP_ISP_Tuning_SetAntiFlickerAttr((IMPISPAntiflickerAttr)mode);
#endif
}

int set_ae_compensation(int val)
{
    if (!caps().has_isp_ae_comp()) {
        LOG_DEBUG("set_ae_compensation not supported on this platform");
        return 0;
    }
    return IMP_ISP_Tuning_SetAeComp(val);
}

int set_dpc_strength(unsigned char val)
{
    if (!caps().has_isp_dpc()) {
        LOG_DEBUG("set_dpc_strength not supported on this platform");
        return 0;
    }
    return IMP_ISP_Tuning_SetDPC_Strength(val);
}

int set_drc_strength(unsigned char val)
{
    if (!caps().has_isp_drc()) {
        LOG_DEBUG("set_drc_strength not supported on this platform");
        return 0;
    }
    return IMP_ISP_Tuning_SetDRC_Strength(val);
}

int set_defog_strength(uint8_t val)
{
    if (!caps().has_isp_defog()) {
        LOG_DEBUG("set_defog_strength not supported on this platform");
        return 0;
    }
    return IMP_ISP_Tuning_SetDefog_Strength(reinterpret_cast<uint8_t*>(&val));
}

int set_backlight_comp(unsigned char val)
{
    if (!caps().has_isp_backlight_comp()) {
        LOG_DEBUG("set_backlight_comp not supported on this platform");
        return 0;
    }
    return IMP_ISP_Tuning_SetBacklightComp(val);
}

int set_highlight_depress(unsigned char val)
{
    if (!caps().has_isp_highlight_depress()) {
        LOG_DEBUG("set_highlight_depress not supported on this platform");
        return 0;
    }
    return IMP_ISP_Tuning_SetHiLightDepress(val);
}

int set_max_again(unsigned char val)
{
    if (!caps().has_isp_max_gain()) {
        LOG_DEBUG("set_max_again not supported on this platform");
        return 0;
    }
    return IMP_ISP_Tuning_SetMaxAgain(val);
}

int set_max_dgain(unsigned char val)
{
    if (!caps().has_isp_max_gain()) {
        LOG_DEBUG("set_max_dgain not supported on this platform");
        return 0;
    }
    return IMP_ISP_Tuning_SetMaxDgain(val);
}

int set_wb(int mode, unsigned short rgain, unsigned short bgain)
{
#if defined(PLATFORM_T40) || defined(PLATFORM_T41)
    // T40 uses IMPISPWBAttr with different structure
    // For now, log and return success
    LOG_DEBUG("set_wb: T40 WB API differs - needs implementation");
    return 0; // TODO: implement T40 WB using IMPISPWBAttr
#else
    IMPISPWB wb;
    memset(&wb, 0, sizeof(IMPISPWB));
    wb.mode = (isp_core_wb_mode)mode;
    wb.rgain = rgain;
    wb.bgain = bgain;
    return IMP_ISP_Tuning_SetWB(&wb);
#endif
}

} // namespace isp
} // namespace hal

// ============================================================================
// Platform Capabilities Implementation
// ============================================================================


} // namespace hal
