#pragma once

#include <imp/imp_osd.h>
#include <imp/imp_encoder.h>
#include <cstdint>

namespace impc {

struct PosHook {
    int x{0};
    int y{0};
    int act{0};
};

// Number of OSD regions prudynt uses commonly; keep small but safe
constexpr int NUM_REGIONS = 4;

// Global positional overrides used by command handlers
extern PosHook g_posHook[NUM_REGIONS];

// Adjust region coordinates in-place to preserve legacy semantics
// - x==0 or y==0 => center along that axis
// - x<0 or y<0 => offset from right/bottom by |x|/|y|
void adjust_osd(IMPRgnHandle handle, IMPOSDRgnAttr *attr);

// Allow handlers to set positions
inline void set_position(int handle, int x, int y) {
    if (handle >= 0 && handle < NUM_REGIONS) {
        g_posHook[handle].x = x;
        g_posHook[handle].y = y;
        g_posHook[handle].act = 1;
    }
}

} // namespace impc


