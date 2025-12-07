#ifndef DETECTION_HPP
#define DETECTION_HPP

#include <vector>
#include <string>
#include <ctime>
#include <imp/imp_osd.h>
#include "Config.hpp"

// Maximum number of detection boxes to display (keep low to avoid OSD region exhaustion)
#define MAX_DETECTION_BOXES 4
// 4 line regions per box: top, bottom, left, right
#define LINES_PER_BOX 4
#define MAX_LINE_REGIONS (MAX_DETECTION_BOXES * LINES_PER_BOX)
// Fixed line width and max buffer size per line
// Max line size: 1920 pixels * 4 bytes/pixel * 4 pixels wide = 30720 bytes
#define DETECTION_LINE_WIDTH 4
#define MAX_LINE_BUFFER_SIZE (1920 * DETECTION_LINE_WIDTH * 4)

struct DetectionBox {
    float x1, y1, x2, y2;  // Normalized coordinates (0.0-1.0)
    float confidence;
    std::string class_name;
};

struct DetectionResult {
    float inference_ms;
    int count;
    std::vector<DetectionBox> detections;
    time_t last_modified;
};

class Detection {
public:
    Detection(int osdGrp, uint16_t stream_width, uint16_t stream_height);
    ~Detection();

    // Initialize OSD regions for detection boxes
    int init();

    // Clean up OSD regions
    int exit();

    // Update detection boxes from JSON file
    void update();

    // Check if detection is enabled
    bool isEnabled() const;

    // Set enabled state
    void setEnabled(bool enabled);

private:
    // Parse JSON detection file
    bool parseDetectionJSON(const char* path, DetectionResult& result);

    // Draw a single detection box using 4 line regions (top, bottom, left, right)
    void drawBox(int index, const DetectionBox& box);

    // Clear all detection boxes
    void clearBoxes();

    // Hide a single line region
    void hideLine(int lineIndex);

    // Configuration
    int osdGrp;
    uint16_t stream_width;
    uint16_t stream_height;
    bool enabled;
    bool initialized;  // Prevent update() before init() completes

    // OSD handles for line regions (4 per box: top, bottom, left, right)
    IMPRgnHandle lineHandles[MAX_LINE_REGIONS];
    // Static buffers - no dynamic allocation, IMP OSD can safely access these
    static uint8_t lineBuffers[MAX_LINE_REGIONS][MAX_LINE_BUFFER_SIZE];
    bool lineActive[MAX_LINE_REGIONS];

    // Last file modification time to avoid unnecessary re-reads
    time_t lastModTime;

    // Current detection count
    int currentBoxCount;
};

#endif // DETECTION_HPP

