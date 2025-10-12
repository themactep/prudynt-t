#include "OSDAdjust.hpp"
#include "imp_hal.hpp"
#include "Logger.hpp"

namespace impc {

PosHook g_posHook[NUM_REGIONS];

void adjust_osd(IMPRgnHandle handle, IMPOSDRgnAttr *prAttr) {
    if (!(handle >= 0 && handle < NUM_REGIONS)) return;
    if (!(g_posHook[handle].act && prAttr != nullptr)) return;

    // Save original size
    int origWidth  = prAttr->rect.p1.x - prAttr->rect.p0.x;
    int origHeight = prAttr->rect.p1.y - prAttr->rect.p0.y;

    // Query current encoder channel 0 for frame size (compatible with legacy)
    IMPEncoderCHNAttr chnAttr{};
    int ret = IMP_Encoder_GetChnAttr(0, &chnAttr);
    if (ret != 0) {
        LOG_WARN("OSDAdjust: IMP_Encoder_GetChnAttr(0) failed, ret=" << ret);
        return; // Don't alter if we cannot retrieve base size
    }

    // X axis
    if (g_posHook[handle].x == 0) {
        prAttr->rect.p0.x = HAL_ENC_ATTR_WIDTH(chnAttr) / 2 - origWidth / 2;
    } else if (g_posHook[handle].x < 0) {
        prAttr->rect.p0.x = HAL_ENC_ATTR_WIDTH(chnAttr) - origWidth + g_posHook[handle].x;
    } else {
        prAttr->rect.p0.x = g_posHook[handle].x;
    }

    // Y axis
    if (g_posHook[handle].y == 0) {
        prAttr->rect.p0.y = HAL_ENC_ATTR_HEIGHT(chnAttr) / 2 - origHeight / 2;
    } else if (g_posHook[handle].y < 0) {
        prAttr->rect.p0.y = HAL_ENC_ATTR_HEIGHT(chnAttr) - origHeight + g_posHook[handle].y;
    } else {
        prAttr->rect.p0.y = g_posHook[handle].y;
    }

    prAttr->rect.p1.x = prAttr->rect.p0.x + origWidth;
    prAttr->rect.p1.y = prAttr->rect.p0.y + origHeight;
}

} // namespace impc


