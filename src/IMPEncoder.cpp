#include "IMPEncoder.hpp"
#include "Config.hpp"

#define MODULE "IMPENCODER"

#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
#define IMPEncoderCHNAttr IMPEncoderChnAttr
#define IMPEncoderCHNStat IMPEncoderChnStat
#endif

IMPEncoder *IMPEncoder::createNew(
    _stream *stream,
    int encChn,
    int encGrp,
    const char *name)
{
    return new IMPEncoder(stream, encChn, encGrp, name);
}

void IMPEncoder::flush(int encChn)
{
    LOG_DDEBUG("flush(" << encChn << ")");
    IMP_Encoder_RequestIDR(encChn);
    IMP_Encoder_FlushStream(encChn);
}

void MakeTables(int q, uint8_t *lqt, uint8_t *cqt)
{
    // Ensure q is within the expected range
    q = std::max(1, std::min(q, 99));

    // Adjust q based on factor
    if (q < 50)
    {
        q = 5000 / q;
    }
    else
    {
        q = 200 - 2 * q;
    }

    // Fill the quantization tables
    for (int i = 0; i < 64; ++i)
    {
        int lq = (jpeg_luma_quantizer[i] * q + 50) / 100;
        int cq = (jpeg_chroma_quantizer[i] * q + 50) / 100;

        // Ensure the quantization values are within [1, 255]
        lqt[i] = static_cast<uint8_t>(std::max(1, std::min(lq, 255)));
        cqt[i] = static_cast<uint8_t>(std::max(1, std::min(cq, 255)));
    }
}

void IMPEncoder::initProfile()
{
    IMPEncoderRcAttr *rcAttr;
    memset(&chnAttr, 0, sizeof(IMPEncoderCHNAttr));
    rcAttr = &chnAttr.rcAttr;

#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    IMPEncoderRcMode rcMode = IMP_ENC_RC_MODE_CAPPED_QUALITY;
    IMPEncoderProfile encoderProfile = IMP_ENC_PROFILE_AVC_HIGH;

    if (strcmp(stream->format, "H265") == 0)
    {
        encoderProfile = IMP_ENC_PROFILE_HEVC_MAIN;
    }
    else if (strcmp(stream->format, "JPEG") == 0)
    {
        encoderProfile = IMP_ENC_PROFILE_JPEG;
        IMP_Encoder_SetDefaultParam(&chnAttr, encoderProfile, IMP_ENC_RC_MODE_FIXQP,
                                    stream->width, stream->height, 24, 1, 0, 0, stream->jpeg_quality, 0);
        // 1000 / stream->jpeg_refresh
        LOG_DEBUG("STREAM PROFILE " << encChn << ", " <<
                    encGrp << ", " << stream->format << ", " <<
                    chnAttr.rcAttr.outFrmRate.frmRateNum << "fps, profile:" <<
                    stream->profile << ", " << stream->width << "x" << stream->height);
        return;
    }

    if (strcmp(stream->mode, "FIXQP") == 0)
    {
        rcMode = IMP_ENC_RC_MODE_FIXQP;
    }
    else if (strcmp(stream->mode, "VBR") == 0)
    {
        rcMode = IMP_ENC_RC_MODE_VBR;
    }
    else if (strcmp(stream->mode, "CBR") == 0)
    {
        rcMode = IMP_ENC_RC_MODE_CBR;
    }
    else if (strcmp(stream->mode, "CAPPED_VBR") == 0)
    {
        rcMode = IMP_ENC_RC_MODE_CAPPED_VBR;
    }
    else if (strcmp(stream->mode, "CAPPED_QUALITY") == 0)
    {
        rcMode = IMP_ENC_RC_MODE_CAPPED_QUALITY;
    }
    else
    {
        LOG_ERROR("unsupported stream->mode (" << stream->mode << "). we only support FIXQP, CBR, VBR, CAPPED_VBR and CAPPED_QUALITY on T31");
    }

    IMP_Encoder_SetDefaultParam(
        &chnAttr, encoderProfile, rcMode, stream->width, stream->height,
        stream->fps, 1, stream->gop, 2, -1, stream->bitrate);

    switch (rcMode)
    {
    case IMP_ENC_RC_MODE_FIXQP:
        rcAttr->attrRcMode.attrFixQp.iInitialQP = 38;
        break;
    case IMP_ENC_RC_MODE_CBR:
        rcAttr->attrRcMode.attrCbr.uTargetBitRate = stream->bitrate;
        rcAttr->attrRcMode.attrCbr.iInitialQP = -1;
        rcAttr->attrRcMode.attrCbr.iMinQP = 34;
        rcAttr->attrRcMode.attrCbr.iMaxQP = 51;
        rcAttr->attrRcMode.attrCbr.iIPDelta = -1;
        rcAttr->attrRcMode.attrCbr.iPBDelta = -1;
        // rcAttr->attrRcMode.attrCbr.eRcOptions = IMP_ENC_RC_SCN_CHG_RES | IMP_ENC_RC_OPT_SC_PREVENTION;
        rcAttr->attrRcMode.attrCbr.uMaxPictureSize = stream->bitrate;
        break;
    case IMP_ENC_RC_MODE_VBR:
        rcAttr->attrRcMode.attrVbr.uTargetBitRate = stream->bitrate;
        rcAttr->attrRcMode.attrVbr.uMaxBitRate = stream->bitrate;
        rcAttr->attrRcMode.attrVbr.iInitialQP = -1;
        rcAttr->attrRcMode.attrVbr.iMinQP = 20;
        rcAttr->attrRcMode.attrVbr.iMaxQP = 45;
        rcAttr->attrRcMode.attrVbr.iIPDelta = 3;
        rcAttr->attrRcMode.attrVbr.iPBDelta = 3;
        // rcAttr->attrRcMode.attrVbr.eRcOptions = IMP_ENC_RC_SCN_CHG_RES | IMP_ENC_RC_OPT_SC_PREVENTION;
        rcAttr->attrRcMode.attrVbr.uMaxPictureSize = stream->bitrate;
        break;
    case IMP_ENC_RC_MODE_CAPPED_VBR:
        rcAttr->attrRcMode.attrCappedVbr.uTargetBitRate = stream->bitrate;
        rcAttr->attrRcMode.attrCappedVbr.uMaxBitRate = stream->bitrate;
        rcAttr->attrRcMode.attrCappedVbr.iInitialQP = -1;
        rcAttr->attrRcMode.attrCappedVbr.iMinQP = 20;
        rcAttr->attrRcMode.attrCappedVbr.iMaxQP = 45;
        rcAttr->attrRcMode.attrCappedVbr.iIPDelta = 3;
        rcAttr->attrRcMode.attrCappedVbr.iPBDelta = 3;
        // rcAttr->attrRcMode.attrCappedVbr.eRcOptions = IMP_ENC_RC_SCN_CHG_RES | IMP_ENC_RC_OPT_SC_PREVENTION;
        rcAttr->attrRcMode.attrCappedVbr.uMaxPictureSize = stream->bitrate;
        rcAttr->attrRcMode.attrCappedVbr.uMaxPSNR = 42;
        break;
    case IMP_ENC_RC_MODE_CAPPED_QUALITY:
        rcAttr->attrRcMode.attrCappedQuality.uTargetBitRate = stream->bitrate;
        rcAttr->attrRcMode.attrCappedQuality.uMaxBitRate = stream->bitrate;
        rcAttr->attrRcMode.attrCappedQuality.iInitialQP = -1;
        rcAttr->attrRcMode.attrCappedQuality.iMinQP = 20;
        rcAttr->attrRcMode.attrCappedQuality.iMaxQP = 45;
        rcAttr->attrRcMode.attrCappedQuality.iIPDelta = 3;
        rcAttr->attrRcMode.attrCappedQuality.iPBDelta = 4;
        // rcAttr->attrRcMode.attrCappedQuality.eRcOptions = IMP_ENC_RC_SCN_CHG_RES | IMP_ENC_RC_OPT_SC_PREVENTION;
        rcAttr->attrRcMode.attrCappedQuality.uMaxPictureSize = stream->bitrate;
        rcAttr->attrRcMode.attrCappedQuality.uMaxPSNR = 42;
        break;
    case IMP_ENC_RC_MODE_INVALID:
        break;
    }

    // Apply optional overrides from config
    {
        const char* key = (stream == &cfg->stream0) ? "stream0" : "stream1";
        std::string p(key);
        int qp_init = cfg->get<int>(p + ".qp_init");
        int qp_min = cfg->get<int>(p + ".qp_min");
        int qp_max = cfg->get<int>(p + ".qp_max");
        int ip_delta = cfg->get<int>(p + ".ip_delta");
        int pb_delta = cfg->get<int>(p + ".pb_delta");
        int max_br = cfg->get<int>(p + ".max_bitrate");

        switch (rcMode)
        {
        case IMP_ENC_RC_MODE_FIXQP:
            if (qp_init >= 0) rcAttr->attrRcMode.attrFixQp.iInitialQP = qp_init;
            break;
        case IMP_ENC_RC_MODE_CBR:
            if (qp_init >= 0) rcAttr->attrRcMode.attrCbr.iInitialQP = qp_init;
            if (qp_min >= 0) rcAttr->attrRcMode.attrCbr.iMinQP = qp_min;
            if (qp_max >= 0) rcAttr->attrRcMode.attrCbr.iMaxQP = qp_max;
            if (ip_delta != -1) rcAttr->attrRcMode.attrCbr.iIPDelta = ip_delta;
            if (pb_delta != -1) rcAttr->attrRcMode.attrCbr.iPBDelta = pb_delta;
            break;
        case IMP_ENC_RC_MODE_VBR:
            if (qp_init >= 0) rcAttr->attrRcMode.attrVbr.iInitialQP = qp_init;
            if (qp_min >= 0) rcAttr->attrRcMode.attrVbr.iMinQP = qp_min;
            if (qp_max >= 0) rcAttr->attrRcMode.attrVbr.iMaxQP = qp_max;
            if (ip_delta != -1) rcAttr->attrRcMode.attrVbr.iIPDelta = ip_delta;
            if (pb_delta != -1) rcAttr->attrRcMode.attrVbr.iPBDelta = pb_delta;
            if (max_br > 0) rcAttr->attrRcMode.attrVbr.uMaxBitRate = max_br;
            break;
        case IMP_ENC_RC_MODE_CAPPED_VBR:
            if (qp_init >= 0) rcAttr->attrRcMode.attrCappedVbr.iInitialQP = qp_init;
            if (qp_min >= 0) rcAttr->attrRcMode.attrCappedVbr.iMinQP = qp_min;
            if (qp_max >= 0) rcAttr->attrRcMode.attrCappedVbr.iMaxQP = qp_max;
            if (ip_delta != -1) rcAttr->attrRcMode.attrCappedVbr.iIPDelta = ip_delta;
            if (pb_delta != -1) rcAttr->attrRcMode.attrCappedVbr.iPBDelta = pb_delta;
            if (max_br > 0) rcAttr->attrRcMode.attrCappedVbr.uMaxBitRate = max_br;
            break;
        case IMP_ENC_RC_MODE_CAPPED_QUALITY:
            if (qp_init >= 0) rcAttr->attrRcMode.attrCappedQuality.iInitialQP = qp_init;
            if (qp_min >= 0) rcAttr->attrRcMode.attrCappedQuality.iMinQP = qp_min;
            if (qp_max >= 0) rcAttr->attrRcMode.attrCappedQuality.iMaxQP = qp_max;
            if (ip_delta != -1) rcAttr->attrRcMode.attrCappedQuality.iIPDelta = ip_delta;
            if (pb_delta != -1) rcAttr->attrRcMode.attrCappedQuality.iPBDelta = pb_delta;
            if (max_br > 0) rcAttr->attrRcMode.attrCappedQuality.uMaxBitRate = max_br;
            break;
        case IMP_ENC_RC_MODE_INVALID:
            break;
        }
    }

#elif defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T30)
    if (strcmp(stream->format, "JPEG") == 0)
    {
        IMPEncoderAttr *encAttr;
        encAttr = &chnAttr.encAttr;
        encAttr->enType = PT_JPEG;
        encAttr->bufSize = 0;
        encAttr->profile = 2;
        encAttr->picWidth = stream->width;
        encAttr->picHeight = stream->height;
        return;
    }
    else if (strcmp(stream->format, "H264") == 0)
    {
        chnAttr.encAttr.enType = PT_H264;
    }
#if defined(PLATFORM_T30)
    else if (strcmp(stream->format, "H265") == 0)
    {
        chnAttr.encAttr.enType = PT_H265;
    }
#endif

    IMPEncoderRcMode rcMode = ENC_RC_MODE_SMART;

    if (strcmp(stream->mode, "FIXQP") == 0)
    {
        rcMode = ENC_RC_MODE_FIXQP;
    }
    else if (strcmp(stream->mode, "VBR") == 0)
    {
        rcMode = ENC_RC_MODE_VBR;
    }
    else if (strcmp(stream->mode, "CBR") == 0)
    {
        rcMode = ENC_RC_MODE_CBR;
    }
    else if (strcmp(stream->mode, "SMART") == 0)
    {
        rcMode = ENC_RC_MODE_SMART;
    }
    else
    {
        LOG_ERROR("unsupported stream->mode (" << stream->mode << "). we only support FIXQP, CBR, VBR and SMART");
    }

    // 0 = Baseline
    // 1 = Main
    // 2 = High
    // Note: The encoder seems to emit frames at half the
    // requested framerate when the profile is set to Baseline.
    // For this reason, Main or High are recommended.
    chnAttr.encAttr.profile = stream->profile;
    chnAttr.encAttr.bufSize = 0;
    chnAttr.encAttr.picWidth = stream->width;
    chnAttr.encAttr.picHeight = stream->height;
    chnAttr.rcAttr.outFrmRate.frmRateNum = stream->fps;
    chnAttr.rcAttr.outFrmRate.frmRateDen = 1;
    rcAttr->maxGop = stream->max_gop;

    if (chnAttr.encAttr.enType == PT_H264)
    {
        switch (rcMode)
        {
        case ENC_RC_MODE_FIXQP:
            rcAttr->attrRcMode.rcMode = ENC_RC_MODE_FIXQP;
            rcAttr->attrRcMode.attrH264FixQp.qp = 42;
            break;
        case ENC_RC_MODE_CBR:
            rcAttr->attrRcMode.rcMode = ENC_RC_MODE_CBR;
            rcAttr->attrRcMode.attrH264Cbr.outBitRate = stream->bitrate;
            rcAttr->attrRcMode.attrH264Cbr.maxQp = 45;
            rcAttr->attrRcMode.attrH264Cbr.minQp = 15;
            rcAttr->attrRcMode.attrH264Cbr.iBiasLvl = 0;
            rcAttr->attrRcMode.attrH264Cbr.frmQPStep = 3;
            rcAttr->attrRcMode.attrH264Cbr.gopQPStep = 15;
            rcAttr->attrRcMode.attrH264Cbr.adaptiveMode = false;
            rcAttr->attrRcMode.attrH264Cbr.gopRelation = false;
            break;
        case ENC_RC_MODE_VBR:
            rcAttr->attrRcMode.rcMode = ENC_RC_MODE_VBR;
            rcAttr->attrRcMode.attrH264Vbr.maxQp = 45;
            rcAttr->attrRcMode.attrH264Vbr.minQp = 15;
            rcAttr->attrRcMode.attrH264Vbr.staticTime = 2;
            rcAttr->attrRcMode.attrH264Vbr.maxBitRate = stream->bitrate;
            rcAttr->attrRcMode.attrH264Vbr.iBiasLvl = 0;
            rcAttr->attrRcMode.attrH264Vbr.changePos = 80;
            rcAttr->attrRcMode.attrH264Vbr.qualityLvl = 2;
            rcAttr->attrRcMode.attrH264Vbr.frmQPStep = 3;
            rcAttr->attrRcMode.attrH264Vbr.gopQPStep = 15;
            rcAttr->attrRcMode.attrH264Vbr.gopRelation = false;
            break;
        case ENC_RC_MODE_SMART:
            rcAttr->attrRcMode.rcMode = ENC_RC_MODE_SMART;
            rcAttr->attrRcMode.attrH264Smart.maxQp = 45;
            rcAttr->attrRcMode.attrH264Smart.minQp = 24;
            rcAttr->attrRcMode.attrH264Smart.staticTime = 2;
            rcAttr->attrRcMode.attrH264Smart.maxBitRate = stream->bitrate;
            rcAttr->attrRcMode.attrH264Smart.iBiasLvl = 0;
            rcAttr->attrRcMode.attrH264Smart.changePos = 80;
            rcAttr->attrRcMode.attrH264Smart.qualityLvl = 2;
            rcAttr->attrRcMode.attrH264Smart.frmQPStep = 3;
            rcAttr->attrRcMode.attrH264Smart.gopQPStep = 15;
            rcAttr->attrRcMode.attrH264Smart.gopRelation = false;
            break;
        case ENC_RC_MODE_INV:
            break;
        }
    #if defined(PLATFORM_T30)
    }
    else if (chnAttr.encAttr.enType == PT_H265)
    {
        rcAttr->attrRcMode.rcMode = ENC_RC_MODE_SMART;
        rcAttr->attrRcMode.attrH265Smart.maxQp = 45;
        rcAttr->attrRcMode.attrH265Smart.minQp = 15;
        rcAttr->attrRcMode.attrH265Smart.staticTime = 2;
        rcAttr->attrRcMode.attrH265Smart.maxBitRate = stream->bitrate;
        rcAttr->attrRcMode.attrH265Smart.iBiasLvl = 0;
        rcAttr->attrRcMode.attrH265Smart.changePos = 80;
        rcAttr->attrRcMode.attrH265Smart.qualityLvl = 2;
        rcAttr->attrRcMode.attrH265Smart.frmQPStep = 3;
        rcAttr->attrRcMode.attrH265Smart.gopQPStep = 15;
        rcAttr->attrRcMode.attrH265Smart.flucLvl = 2;
    #endif //defined(PLATFORM_T30)
    }
    // Optional overrides from config (legacy platforms)
    {
        const char* key = (stream == &cfg->stream0) ? "stream0" : "stream1";
        std::string p(key);
        int qp_init = cfg->get<int>(p + ".qp_init");
        int qp_min  = cfg->get<int>(p + ".qp_min");
        int qp_max  = cfg->get<int>(p + ".qp_max");
        int max_br  = cfg->get<int>(p + ".max_bitrate");
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


    rcAttr->attrHSkip.hSkipAttr.skipType = IMP_Encoder_STYPE_N1X;
    rcAttr->attrHSkip.hSkipAttr.m = rcAttr->maxGop - 1;
    rcAttr->attrHSkip.hSkipAttr.n = 1;
    rcAttr->attrHSkip.hSkipAttr.maxSameSceneCnt = 0;
    rcAttr->attrHSkip.hSkipAttr.bEnableScenecut = 0;
    rcAttr->attrHSkip.hSkipAttr.bBlackEnhance = 0;
    rcAttr->attrHSkip.maxHSkipType = IMP_Encoder_STYPE_N1X;
#endif //defined(PLATFORM_T10) || defined(PLATFORM_T20) || defined(PLATFORM_T21) || defined(PLATFORM_T23) || defined(PLATFORM_T30)
    LOG_DEBUG("STREAM PROFILE " <<
        stream->rtsp_endpoint << ", " <<
        "fps:" << chnAttr.rcAttr.outFrmRate.frmRateNum << ", " <<
        "bps:" << stream->bitrate << ", " <<
        "gop:" << stream->gop << ", " <<
        "profile:" << stream->profile << ", " <<
//        "mode:" << rcMode << ", " <<
        stream->width << "x" <<
        stream->height);
}

int IMPEncoder::init()
{
    LOG_DEBUG("IMPEncoder::init(" << encChn << ", " << encGrp << ")");

    int ret = 0;

    initProfile();

#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
    if (cfg->stream2.enabled && cfg->stream2.jpeg_channel == encChn && stream->allow_shared) {
        ret = IMP_Encoder_SetbufshareChn(2, encChn);
        LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_Encoder_SetbufshareChn(2, " << encChn << ")");
    }
#endif

    ret = IMP_Encoder_CreateChn(encChn, &chnAttr);
    LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_Encoder_CreateChn(" << encChn << ", chnAttr)");

    ret = IMP_Encoder_RegisterChn(encGrp, encChn);
    LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_Encoder_RegisterChn(" << encGrp << ", " << encChn << ")");

    if (strcmp(stream->format, "JPEG") != 0)
    {
        ret = IMP_Encoder_CreateGroup(encGrp);
        LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_Encoder_CreateGroup(" << encGrp << ")");

        fs = {DEV_ID_FS, encGrp, 0};
        enc = {DEV_ID_ENC, encGrp, 0};
        osd_cell = {DEV_ID_OSD, encGrp, 0};

        if (stream->osd.enabled)
        {
            osd = OSD::createNew(stream->osd, encGrp, encChn, name);

            ret = IMP_System_Bind(&fs, &osd_cell);
            LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_System_Bind(&fs, &osd_cell)");

            ret = IMP_System_Bind(&osd_cell, &enc);
            LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_System_Bind(&osd_cell, &enc)");
        }
        else
        {
            ret = IMP_System_Bind(&fs, &enc);
            LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_System_Bind(&fs, &enc)");
        }
    }
#if !(defined(PLATFORM_T31) || !defined(PLATFORM_C100) || !defined(PLATFORM_T40) || !defined(PLATFORM_T41))
    else
    {
        IMPEncoderJpegeQl pstJpegeQl;
        // fix for bad jpeg image quality on T10 based cameras
        if (strncmp(cfg->sysinfo.cpu, "T10", 3)==0)
        {
            pstJpegeQl.user_ql_en = 0;
            LOG_DEBUG("JPEG use default quantization table");
        }
        else
        {
            MakeTables(stream->jpeg_quality, &(pstJpegeQl.qmem_table[0]), &(pstJpegeQl.qmem_table[64]));
            pstJpegeQl.user_ql_en = 1;
            LOG_DEBUG("JPEG use custom user quantization table");
        }

        IMP_Encoder_SetJpegeQl(2, &pstJpegeQl);
    }
#endif

    return ret;
}

int IMPEncoder::deinit()
{
    LOG_DEBUG("IMPEncoder::deinit(" << encChn << ", " << encGrp << ")");

    int ret;

    if (strcmp(stream->format, "JPEG") != 0)
    {
        if (osd)
        {
            ret = IMP_System_UnBind(&fs, &osd_cell);
            LOG_DEBUG_OR_ERROR(ret, "IMP_System_UnBind(&fs, &osd_cell)");

            ret = IMP_System_UnBind(&osd_cell, &enc);
            LOG_DEBUG_OR_ERROR(ret, "IMP_System_UnBind(&osd_cell, &enc)");

            osd->exit();
            delete osd;
            osd = nullptr;
        }
        else
        {
            ret = IMP_System_UnBind(&fs, &enc);
            LOG_DEBUG_OR_ERROR(ret, "IMP_System_UnBind(&fs, &enc)");
        }
    }
    else
    {

        ret = IMP_Encoder_StopRecvPic(encChn);
        LOG_DEBUG("IMP_Encoder_StopRecvPic(" << encChn << ")");
    }

    ret = IMP_Encoder_UnRegisterChn(encChn);
    LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_Encoder_UnRegisterChn(" << encChn << ")");

    ret = IMP_Encoder_DestroyChn(encChn);
    LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_Encoder_DestroyChn(" << encChn << ")");

    return ret;
}

int IMPEncoder::destroy()
{

    int ret;

    // Only destroy group for non-JPEG streams (JPEG streams don't create groups)
    if (strcmp(stream->format, "JPEG") != 0)
    {
        ret = IMP_Encoder_DestroyGroup(encGrp);
        LOG_DEBUG_OR_ERROR(ret, "IMP_Encoder_DestroyGroup(" << encGrp << ")");
    }
    else
    {
        ret = 0; // No group to destroy for JPEG
    }

    return ret;
}
