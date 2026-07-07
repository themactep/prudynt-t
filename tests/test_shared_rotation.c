// Minimal test: one rotated FrameSource → two Encoders
// Tests if IMP supports binding multiple encoder groups to one rotated FS channel.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_framesource.h>
#include <imp/imp_encoder.h>
#include <imp/imp_osd.h>

#define SENSOR_W 2304
#define SENSOR_H 1536
#define FS_CHN  0

// ch0: full-res rotated (1536x2304)
#define ENC_CH0 0
#define ENC_GRP0 0

// ch1: shared FS, encode at lower res
#define ENC_CH1 1
#define ENC_GRP1 1

static int init_fs_rotated() {
    IMPFSChnAttr attr;
    memset(&attr, 0, sizeof(attr));

    attr.picWidth  = SENSOR_W;
    attr.picHeight = SENSOR_H;
    attr.pixFmt    = PIX_FMT_NV12;  // 10 = NV12
    attr.outFrmRateNum = 15;
    attr.outFrmRateDen = 1;
    attr.nrVBs = 3;
    attr.type = FS_PHY_CHANNEL;
    attr.crop.enable = 0;
    attr.crop.width  = SENSOR_W;
    attr.crop.height = SENSOR_H;
    attr.scaler.enable = 0;

    // Rotation
    typedef int (*rot_fn)(int, uint8_t, int, int);
    void *h = dlopen(NULL, RTLD_LAZY);
    rot_fn set_rot = (rot_fn)dlsym(h, "IMP_FrameSource_SetChnRotate");

    int ret = IMP_FrameSource_CreateChn(FS_CHN, &attr);
    if (ret) { fprintf(stderr, "CreateChn FS failed: %d\n", ret); return ret; }

    if (set_rot) {
        ret = set_rot(FS_CHN, 2, SENSOR_W, SENSOR_H);  // 270°
        fprintf(stderr, "SetChnRotate(0, 2, %d, %d) = %d\n", SENSOR_W, SENSOR_H, ret);
    } else {
        fprintf(stderr, "SetChnRotate not found\n");
    }

    ret = IMP_FrameSource_EnableChn(FS_CHN);
    fprintf(stderr, "EnableChn FS = %d\n", ret);
    return ret;
}

static int init_encoder(int encChn, int encGrp, int w, int h, const char *label) {
    IMPEncoderChnAttr attr;
    memset(&attr, 0, sizeof(attr));
    int ret = IMP_Encoder_SetDefaultParam(&attr, IMP_ENC_PROFILE_AVC_HIGH,
        IMP_ENC_RC_MODE_CBR, w, h, 15, 1, 30, 2, -1, 2000);
    fprintf(stderr, "%s SetDefaultParam(%dx%d) = %d\n", label, w, h, ret);

    ret = IMP_Encoder_CreateGroup(encGrp);
    if (ret) { fprintf(stderr, "%s CreateGroup(%d) = %d\n", label, encGrp, ret); return ret; }

    ret = IMP_Encoder_CreateChn(encChn, &attr);
    fprintf(stderr, "%s CreateChn(%d) = %d\n", label, encChn, ret);
    if (ret) return ret;

    ret = IMP_Encoder_RegisterChn(encGrp, encChn);
    fprintf(stderr, "%s RegisterChn(%d,%d) = %d\n", label, encGrp, encChn, ret);
    return ret;
}

int main() {
    IMPSystemAttr sysAttr;
    memset(&sysAttr, 0, sizeof(sysAttr));
    IMP_System_Init(&sysAttr);

    // Init ISP (simplified — assumes sensor already set up)
    IMP_ISP_Open();
    // ... sensor init skipped for minimal test ...

    // Create one rotated FrameSource
    init_fs_rotated();

    // Bind two encoder groups to the same FS channel
    IMPCell fs  = {DEV_ID_FS, FS_CHN, 0};
    IMPCell osd0 = {DEV_ID_OSD, ENC_GRP0, 0};
    IMPCell enc0 = {DEV_ID_ENC, ENC_GRP0, 0};
    IMPCell osd1 = {DEV_ID_OSD, ENC_GRP1, 0};
    IMPCell enc1 = {DEV_ID_ENC, ENC_GRP1, 0};

    // Encoder 0: full-res rotated (1536x2304)
    init_encoder(ENC_CH0, ENC_GRP0, 1536, 2304, "ch0");

    // Create manual OSD groups (minimal)
    IMP_OSD_CreateGroup(ENC_GRP0);
    IMP_OSD_CreateGroup(ENC_GRP1);

    // Bind FS → OSD0 → ENC0
    int ret = IMP_System_Bind(&fs, &osd0);
    fprintf(stderr, "Bind FS→OSD0 = %d\n", ret);
    ret = IMP_System_Bind(&osd0, &enc0);
    fprintf(stderr, "Bind OSD0→ENC0 = %d\n", ret);

    // Encoder 1: also bound to FS channel 0, encode at lower res
    init_encoder(ENC_CH1, ENC_GRP1, 384, 576, "ch1");

    // Bind FS → OSD1 → ENC1 (same FS, different encoder group)
    ret = IMP_System_Bind(&fs, &osd1);
    fprintf(stderr, "Bind FS→OSD1 = %d (0=OK, -1=FAIL)\n", ret);
    ret = IMP_System_Bind(&osd1, &enc1);
    fprintf(stderr, "Bind OSD1→ENC1 = %d (0=OK, -1=FAIL)\n", ret);

    // Start encoders
    IMP_Encoder_StartRecvPic(ENC_CH0);
    IMP_Encoder_StartRecvPic(ENC_CH1);
    IMP_OSD_Start(ENC_GRP0);
    IMP_OSD_Start(ENC_GRP1);

    fprintf(stderr, "\nAll set up. Check /proc/jz/ for encoder status.\n");
    fprintf(stderr, "Both encoders bound to rotated FS channel %d.\n", FS_CHN);
    fprintf(stderr, "Press Ctrl+C to exit.\n");
    pause();
    return 0;
}
