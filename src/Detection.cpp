#include "Detection.hpp"
#include "Logger.hpp"
#include "globals.hpp"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

extern std::shared_ptr<CFG> cfg;

// Static buffer storage - allocated at compile time, no dynamic allocation
uint8_t Detection::lineBuffers[MAX_LINE_REGIONS][MAX_LINE_BUFFER_SIZE];

Detection::Detection(int osdGrp, uint16_t stream_width, uint16_t stream_height)
    : osdGrp(osdGrp), stream_width(stream_width), stream_height(stream_height),
      enabled(false), initialized(false), lastModTime(0), currentBoxCount(0)
{
    for (int i = 0; i < MAX_LINE_REGIONS; i++) {
        lineHandles[i] = -1;
        lineActive[i] = false;
    }
}

Detection::~Detection() { exit(); }

int Detection::init()
{
    LOG_DEBUG("Detection::init() for osdGrp " << osdGrp << " - lazy allocation mode");

    // Don't pre-allocate buffers - allocate on-demand in drawBox()
    // This avoids potential memory issues during startup
    // Buffers will be allocated when first detection is drawn

    enabled = true;
    initialized = true;
    LOG_INFO("Detection overlay initialized (lazy alloc), stream " << stream_width << "x" << stream_height);
    return 0;
}

int Detection::exit()
{
    LOG_DEBUG("Detection::exit()");
    for (int i = 0; i < MAX_LINE_REGIONS; i++) {
        if (lineHandles[i] >= 0) {
            IMP_OSD_UnRegisterRgn(lineHandles[i], osdGrp);
            IMP_OSD_DestroyRgn(lineHandles[i]);
            lineHandles[i] = -1;
        }
        // Static buffers - no need to free
        lineActive[i] = false;
    }
    return 0;
}

bool Detection::isEnabled() const { return initialized && enabled; }
void Detection::setEnabled(bool en) { enabled = en; if (!enabled && initialized) clearBoxes(); }

bool Detection::parseDetectionJSON(const char* path, DetectionResult& result)
{
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char buffer[4096];
    size_t len = fread(buffer, 1, sizeof(buffer) - 1, f);
    fclose(f);
    if (len == 0) return false;
    buffer[len] = '\0';

    result.detections.clear();
    result.count = 0;
    result.inference_ms = 0;

    // Use hardcoded limits to avoid accessing cfg in thread context
    const int max_boxes = MAX_DETECTION_BOXES;
    const float min_confidence = 0.5f;

    char* p = strstr(buffer, "\"inference_ms\":");
    if (p) result.inference_ms = atof(p + 15);
    p = strstr(buffer, "\"count\":");
    if (p) result.count = atoi(p + 8);

    p = strstr(buffer, "\"detections\":[");
    if (!p) return true;
    p += 14;

    while (*p && result.detections.size() < (size_t)max_boxes) {
        char* objStart = strchr(p, '{');
        if (!objStart) break;
        char* objEnd = strchr(objStart, '}');
        if (!objEnd) break;

        DetectionBox box;
        box.confidence = 0;
        box.x1 = box.y1 = box.x2 = box.y2 = 0;

        char* classPtr = strstr(objStart, "\"class\":\"");
        if (classPtr && classPtr < objEnd) {
            classPtr += 9;
            char* classEnd = strchr(classPtr, '"');
            if (classEnd && classEnd < objEnd)
                box.class_name = std::string(classPtr, classEnd - classPtr);
        }
        char* confPtr = strstr(objStart, "\"conf\":");
        if (confPtr && confPtr < objEnd) box.confidence = atof(confPtr + 7);

        char* boxPtr = strstr(objStart, "\"box\":[");
        if (boxPtr && boxPtr < objEnd) {
            boxPtr += 7;
            sscanf(boxPtr, "%f,%f,%f,%f", &box.x1, &box.y1, &box.x2, &box.y2);
        }
        if (box.confidence >= min_confidence)
            result.detections.push_back(box);
        p = objEnd + 1;
    }
    return true;
}

// Helper to hide a single line region (does NOT free buffer - IMP may still be using it)
void Detection::hideLine(int lineIndex)
{
    if (lineIndex >= MAX_LINE_REGIONS || lineHandles[lineIndex] < 0) return;
    if (lineActive[lineIndex]) {
        IMPOSDGrpRgnAttr grpRgnAttr;
        memset(&grpRgnAttr, 0, sizeof(IMPOSDGrpRgnAttr));
        grpRgnAttr.show = 0;
        IMP_OSD_SetGrpRgnAttr(lineHandles[lineIndex], osdGrp, &grpRgnAttr);
        lineActive[lineIndex] = false;
    }
    // DO NOT free buffer here - IMP OSD may still be accessing it asynchronously
    // Buffers are only freed in exit() after region is destroyed, or reused in drawBox()
}

// Draw a single detection box using 4 thin line regions (top, bottom, left, right)
// This uses ~48KB max per box instead of ~8MB for a full rectangle
void Detection::drawBox(int index, const DetectionBox& box)
{
    if (index >= MAX_DETECTION_BOXES) return;

    int x1 = (int)(box.x1 * stream_width);
    int y1 = (int)(box.y1 * stream_height);
    int x2 = (int)(box.x2 * stream_width);
    int y2 = (int)(box.y2 * stream_height);

    // Clamp to valid screen coordinates
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= stream_width) x2 = stream_width - 1;
    if (y2 >= stream_height) y2 = stream_height - 1;

    int box_width = x2 - x1;
    int box_height = y2 - y1;

    // Use hardcoded values to avoid cfg access issues in thread context
    int lw = 2;  // line width
    if (lw < 1) lw = 1;
    if (lw > 10) lw = 10;

    // Box must be larger than line width
    if (box_width < lw * 2 || box_height < lw * 2) return;

    // Green color in BGRA format: 0xFF00FF00
    unsigned int color = 0xFF00FF00;
    uint8_t cb = color & 0xFF;           // B = 0x00
    uint8_t cg = (color >> 8) & 0xFF;    // G = 0xFF
    uint8_t cr = (color >> 16) & 0xFF;   // R = 0x00
    uint8_t ca = (color >> 24) & 0xFF;   // A = 0xFF

    // Pre-compute the 32-bit BGRA pixel value for memset-style fill
    uint32_t pixel = ((uint32_t)ca << 24) | ((uint32_t)cr << 16) | ((uint32_t)cg << 8) | cb;

    // Line region indices: 0=top, 1=bottom, 2=left, 3=right
    int baseIdx = index * LINES_PER_BOX;

    // Define the 4 lines with safe coordinates
    // Top line: full width at top
    // Bottom line: full width at bottom
    // Left line: only the middle portion (excluding corners already covered by top/bottom)
    // Right line: only the middle portion
    int inner_height = box_height - 2 * lw;
    if (inner_height < 2) inner_height = 2;

    struct { int x, y, w, h; } lines[4] = {
        { x1, y1, box_width, lw },                           // top
        { x1, y1 + box_height - lw, box_width, lw },         // bottom
        { x1, y1 + lw, lw, inner_height },                   // left (between top and bottom)
        { x1 + box_width - lw, y1 + lw, lw, inner_height }   // right (between top and bottom)
    };

    for (int l = 0; l < LINES_PER_BOX; l++) {
        int li = baseIdx + l;
        if (li >= MAX_LINE_REGIONS) continue;

        int lx = lines[l].x;
        int ly = lines[l].y;
        int w = lines[l].w;
        int h = lines[l].h;

        // Skip invalid dimensions
        if (w <= 0 || h <= 0) continue;

        // Ensure even dimensions (IMP OSD requirement)
        if (w % 2 != 0) w++;
        if (h % 2 != 0) h++;
        if (w < 2) w = 2;
        if (h < 2) h = 2;

        int num_pixels = w * h;
        int buf_size = num_pixels * 4;

        // Check static buffer is large enough (should always be true with MAX_LINE_BUFFER_SIZE)
        if (buf_size > MAX_LINE_BUFFER_SIZE) {
            LOG_ERROR("Line buffer " << li << " size " << buf_size << " exceeds max " << MAX_LINE_BUFFER_SIZE);
            continue;
        }

        // Fill static buffer with solid color using 32-bit writes
        uint32_t* buf32 = (uint32_t*)lineBuffers[li];
        for (int p = 0; p < num_pixels; p++) {
            buf32[p] = pixel;
        }

        // Create region on-demand if it doesn't exist
        if (lineHandles[li] < 0) {
            lineHandles[li] = IMP_OSD_CreateRgn(nullptr);
            if (lineHandles[li] < 0) {
                LOG_ERROR("Failed to create OSD region for detection line " << li);
                continue;
            }
            int ret = IMP_OSD_RegisterRgn(lineHandles[li], osdGrp, nullptr);
            if (ret < 0) {
                LOG_ERROR("Failed to register OSD region for detection line " << li);
                IMP_OSD_DestroyRgn(lineHandles[li]);
                lineHandles[li] = -1;
                continue;
            }
        }

        // Set up region attributes with actual data FIRST
        IMPOSDRgnAttr rgnAttr;
        memset(&rgnAttr, 0, sizeof(IMPOSDRgnAttr));
        rgnAttr.type = OSD_REG_PIC;
        rgnAttr.fmt = PIX_FMT_BGRA;
        rgnAttr.rect.p0.x = lx;
        rgnAttr.rect.p0.y = ly;
        rgnAttr.rect.p1.x = lx + w - 1;
        rgnAttr.rect.p1.y = ly + h - 1;
        rgnAttr.data.picData.pData = lineBuffers[li];

        IMP_OSD_SetRgnAttr(lineHandles[li], &rgnAttr);

        // THEN set group region attributes (show the region)
        IMPOSDGrpRgnAttr grpRgnAttr;
        memset(&grpRgnAttr, 0, sizeof(IMPOSDGrpRgnAttr));
        grpRgnAttr.show = 1;
        grpRgnAttr.layer = 5 + li;
        grpRgnAttr.gAlphaEn = 1;
        grpRgnAttr.fgAlhpa = 255;
        grpRgnAttr.bgAlhpa = 0;
        IMP_OSD_SetGrpRgnAttr(lineHandles[li], osdGrp, &grpRgnAttr);

        lineActive[li] = true;
    }
}

void Detection::clearBoxes()
{
    for (int i = 0; i < MAX_LINE_REGIONS; i++) {
        hideLine(i);
    }
    currentBoxCount = 0;
}

void Detection::update()
{
    // Don't update if not initialized or not enabled
    if (!initialized) return;

    if (!isEnabled()) {
        if (currentBoxCount > 0) clearBoxes();
        return;
    }

    // Use hardcoded path to avoid any issues with cfg pointer
    const char* json_path = "/tmp/detections.json";

    struct stat st;
    if (stat(json_path, &st) != 0) {
        // File doesn't exist yet, clear boxes and return
        if (currentBoxCount > 0) clearBoxes();
        return;
    }

    // Skip if file hasn't changed
    if (st.st_mtime == lastModTime) return;
    lastModTime = st.st_mtime;

    DetectionResult result;
    if (!parseDetectionJSON(json_path, result)) return;

    // Limit to max detection boxes
    size_t numBoxes = result.detections.size();
    if (numBoxes > MAX_DETECTION_BOXES) numBoxes = MAX_DETECTION_BOXES;

    // Hide line regions for boxes that are no longer needed
    for (int i = (int)numBoxes; i < currentBoxCount; i++) {
        int baseIdx = i * LINES_PER_BOX;
        for (int l = 0; l < LINES_PER_BOX; l++) {
            if (baseIdx + l < MAX_LINE_REGIONS) {
                hideLine(baseIdx + l);
            }
        }
    }

    // Draw active boxes
    for (size_t i = 0; i < numBoxes; i++) {
        drawBox((int)i, result.detections[i]);
    }

    currentBoxCount = (int)numBoxes;
}
