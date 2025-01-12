#include <iostream>
#include <thread>

#include "MsgChannel.hpp"
#include "Encoder.hpp"
#include "RTSP.hpp"
#include "Logger.hpp"
#include "IMP.hpp"
#include "Config.hpp"

template <class T> void start_component(T c) {
    c.run();
}

Encoder enc;
RTSP rtsp;

bool timesync_wait() {
    // I don't really have a better way to do this than
    // a no-earlier-than time. The most common sync failure
    // is time() == 0
    int timeout = 0;
    while (time(NULL) < 1647489843) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        ++timeout;
        if (timeout == 60)
            return false;
    }
    return true;
}

int main(int argc, const char *argv[]) {
    std::thread enc_thread;
    std::thread rtsp_thread;

    if (Logger::init()) {
        LOG_DEBUG("Logger initialization failed.");
        return 1;
    }
    LOG_INFO("Starting Prudynt Video Server.");

    if (!timesync_wait()) {
        LOG_DEBUG("Time is not synchronized.");
        return 1;
    }
    if (IMP::init()) {
        LOG_DEBUG("IMP initialization failed.");
        return 1;
    }
    if (enc.init()) {
        LOG_DEBUG("Encoder initialization failed.");
        return 1;
    }

    enc_thread = std::thread(start_component<Encoder>, enc);
    rtsp_thread = std::thread(start_component<RTSP>, rtsp);

    enc_thread.join();
    rtsp_thread.join();
    return 0;
}
