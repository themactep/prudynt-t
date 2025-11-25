#pragma once

#include <string>
#include <thread>

// Simple FIFO-based control helper for MP4 recording.
// Uses a named pipe and accepts text commands (one per line):
//   START <path>  - begin recording to given file
//   STOP          - stop current recording
// Runs in its own thread and uses global_mp4_recorders defined in globals.

class MP4ControlSocket {
public:
    static void run();
};
