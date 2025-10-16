#include "imp_hal.hpp"
#include "imp_hal.hpp"
#include "Config.hpp"
#include "Logger.hpp"
#include <cstring>
#include <dlfcn.h>


extern void MakeTables(int q, uint8_t *lqt, uint8_t *cqt);

namespace hal {

static PlatformCaps g_caps = {
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    /*has_h265*/ true,
    /*has_capped_quality*/ true,
    /*has_capped_vbr*/ true,
    /*has_ip_pb_delta*/ true,
    /*has_bufshare*/ true,
    /*has_jpeg_set_qtable*/ false,
#elif defined(PLATFORM_T30)
    /*has_h265*/ true,
    /*has_capped_quality*/ false,
    /*has_capped_vbr*/ false,
    /*has_ip_pb_delta*/ false,
    /*has_bufshare*/ false,
    /*has_jpeg_set_qtable*/ true,
#else
    /*has_h265*/ false,
    /*has_capped_quality*/ false,
    /*has_capped_vbr*/ false,
    /*has_ip_pb_delta*/ false,
    /*has_bufshare*/ false,
    /*has_jpeg_set_qtable*/ true,
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

} // namespace hal

