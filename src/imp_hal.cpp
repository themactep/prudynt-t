#include "imp_hal.hpp"
#include "Config.hpp"
#include "Logger.hpp"

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
    int ret = IMP_Encoder_SetbufshareChn(jpegEncGrp, srcEncChn);
    LOG_DEBUG_OR_ERROR(ret, "IMP_Encoder_SetbufshareChn(" << jpegEncGrp << ", " << srcEncChn << ")");
    return ret;
#else
    (void)jpegEncGrp; (void)srcEncChn;
    return 0;
#endif
}

} // namespace hal

