#ifndef RTSP_hpp
#define RTSP_hpp

#include "util/Logger.hpp"
#include <memory>
#include <string>

// Forward declarations
struct _stream;
namespace simple_rtsp {
class RtspServer;
}

class RTSP {
public:
    RTSP();
    ~RTSP();

    // Configure and start a stream (replaces addSubsession)
    void addSubsession(int chnNr, _stream &stream);

    // Start the RTSP server (blocking --- runs event loop)
    void start();

    // pthread entry point
    static void *run(void *arg);

private:
    std::unique_ptr<simple_rtsp::RtspServer> server_;
    int port_ = 554;
    bool audioConfigured_ = false;
};

#endif
