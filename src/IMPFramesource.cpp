#include "Logger.hpp"
#include "IMPFramesource.hpp"
#include <dlfcn.h>

#define MODULE "IMP_FRAMESOURCE"

#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41)
#define IMPEncoderCHNAttr IMPEncoderChnAttr
#define IMPEncoderCHNStat IMPEncoderChnStat
#endif

IMPFramesource *IMPFramesource::createNew(
    _stream *stream,
    _sensor *sensor,
    int chnNr)
{
    return new IMPFramesource(stream, sensor, chnNr);
}

int IMPFramesource::init()
{
    LOG_DEBUG("IMPFramesource::init()");

    int ret = 0, scale = 0;

    IMPFSChnAttr chnAttr;
    memset(&chnAttr, 0, sizeof(IMPFSChnAttr));

    ret = IMP_FrameSource_GetChnAttr(chnNr, &chnAttr);

    if ((sensor->width != stream->width) || (sensor->height != stream->height)) {
        scale = 1;
    } else {
        scale = 0;
    }

    // Set required base attributes
    chnAttr.picWidth = stream->width;
    chnAttr.picHeight = stream->height;
    chnAttr.pixFmt = PIX_FMT_NV12;
    chnAttr.outFrmRateNum = stream->fps;
    chnAttr.outFrmRateDen = 1;
    // Keep buffers as configured; default to 2 if unset, to remain memory-friendly on low-RAM devices
    chnAttr.nrVBs = (stream->buffers > 0 ? stream->buffers : 2);
    chnAttr.type = FS_PHY_CHANNEL;

    chnAttr.crop.enable = 0;
    chnAttr.crop.top = 0;
    chnAttr.crop.left = 0;
    chnAttr.crop.width = sensor->width;
    chnAttr.crop.height = sensor->height;

    chnAttr.scaler.enable = scale;
    if (stream->rotation != 0) {
        chnAttr.scaler.outwidth = stream->height;
        chnAttr.scaler.outheight = stream->width;
        chnAttr.picWidth = stream->height;
        chnAttr.picHeight = stream->width;
// Breaks OSD
//        chnAttr.picWidth = stream->width;
//        chnAttr.picHeight = stream->height;
    } else {
        chnAttr.scaler.outwidth = stream->width;
        chnAttr.scaler.outheight = stream->height;
        chnAttr.picWidth = stream->width;
        chnAttr.picHeight = stream->height;
    }

    LOG_DEBUG("Channel " << chnNr << " configuration (post-attr):");
    LOG_DEBUG("  pic: " << chnAttr.picWidth << "x" << chnAttr.picHeight);
    LOG_DEBUG("  crop.enable=" << chnAttr.crop.enable << " crop=" << chnAttr.crop.width << "x" << chnAttr.crop.height);
    LOG_DEBUG("  scaler.enable=" << chnAttr.scaler.enable << " out=" << chnAttr.scaler.outwidth << "x" << chnAttr.scaler.outheight);
    LOG_DEBUG("  fps=" << chnAttr.outFrmRateNum << "/" << chnAttr.outFrmRateDen << " nrVBs=" << chnAttr.nrVBs << " pixFmt=" << chnAttr.pixFmt);

#if !defined(KERNEL_VERSION_4)
#if defined(PLATFORM_T31) && !defined(PLATFORM_C100)

    int rot_rotation = stream->rotation;
    int rot_height = stream->height;
    int rot_width = stream->width;

    // Set rotate before FS creation
    // IMP_Encoder_SetFisheyeEnableStatus(0, 1);
    // IMP_Encoder_SetFisheyeEnableStatus(1, 1);

    if (stream->rotation != 0) {
       {
           typedef int (*pfn_fs_rotate)(int,int,int,int);
           void* h = dlopen(nullptr, RTLD_LAZY);
           pfn_fs_rotate fn = h ? reinterpret_cast<pfn_fs_rotate>(dlsym(h, "IMP_FrameSource_SetChnRotate")) : nullptr;
           if (fn) {
               ret = fn(chnNr, rot_rotation, rot_height, rot_width);
           } else {
               LOG_DEBUG("IMP_FrameSource_SetChnRotate not available; skipping rotation");
               ret = 0;
           }
       }
       LOG_DEBUG_OR_ERROR(ret, "IMP_FrameSource_SetChnRotate(0, rotation, rot_height, rot_width)");
    }

#endif
#endif

    ret = IMP_FrameSource_CreateChn(chnNr, &chnAttr);
    LOG_DEBUG_OR_ERROR(ret, "IMP_FrameSource_CreateChn(" << chnNr << ", &chnAttr)");

    ret = IMP_FrameSource_SetChnAttr(chnNr, &chnAttr);
    LOG_DEBUG_OR_ERROR(ret, "IMP_FrameSource_SetChnAttr(" << chnNr << ", &chnAttr)");

#if !defined(NO_FIFO)
    IMPFSChnFifoAttr fifo;
    ret = IMP_FrameSource_GetChnFifoAttr(chnNr, &fifo);
    LOG_DEBUG_OR_ERROR(ret, "IMP_FrameSource_GetChnFifoAttr(" << chnNr << ", &fifo)");

    fifo.maxdepth = 0;
    ret = IMP_FrameSource_SetChnFifoAttr(chnNr, &fifo);
    LOG_DEBUG_OR_ERROR(ret, "IMP_FrameSource_SetChnFifoAttr(" << chnNr << ", &fifo)");

    ret = IMP_FrameSource_SetFrameDepth(chnNr, 0);
    LOG_DEBUG_OR_ERROR(ret, "IMP_FrameSource_SetFrameDepth(" << chnNr << ", 0)");
#endif

    //ret = IMP_FrameSource_EnableChn(chnNr);
    //LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_FrameSource_EnableChn(" << chnNr << ")");

    return ret;
}

int IMPFramesource::enable()
{

    int ret;

    ret = IMP_FrameSource_EnableChn(chnNr);
    LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_FrameSource_EnableChn(" << chnNr << ")");

    return 0;
}

int IMPFramesource::disable()
{

    int ret;

    ret = IMP_FrameSource_DisableChn(chnNr);
    LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_FrameSource_DisableChn(" << chnNr << ")");

    return 0;
}

int IMPFramesource::destroy()
{

    int ret;

    ret = IMP_FrameSource_DestroyChn(chnNr);
    LOG_DEBUG_OR_ERROR_AND_EXIT(ret, "IMP_FrameSource_DestroyChn(" << chnNr << ")");

    return 0;
}
