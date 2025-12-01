#pragma once

#include <atomic>

// Coordinates runtime imaging parameter export and control IPC so that
// other daemons (e.g. ONVIF) can read/update Prudynt's ISP state without
// linking against libimp directly.
class ImagingControl
{
public:
    static void start();
    static void stop();
    static bool isRunning();
    static void refreshSnapshot();

private:
    static void run();
};
