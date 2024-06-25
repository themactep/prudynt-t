#ifndef RTSP_hpp
#define RTSP_hpp

#include <memory>

#include "Encoder.hpp"
#include "Logger.hpp"
#include <queue>
#include <utility>

class RTSP {
public:
    explicit RTSP(std::shared_ptr<CFG> _cfg) : cfg(std::move(_cfg)) {};

    void run();

private:
    std::shared_ptr<CFG> cfg;
};

#endif
