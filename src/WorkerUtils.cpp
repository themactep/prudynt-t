#include "WorkerUtils.hpp"

#include <cstddef>
#include <time.h>

namespace WorkerUtils {

void getMonotonicTimeOfDay(struct timeval *tv)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
}

unsigned long long getMonotonicTimeDiffInMs(struct timeval *startTime)
{
    struct timeval currentTime;
    getMonotonicTimeOfDay(&currentTime);

    uint64_t start_us = (uint64_t) startTime->tv_sec * 1000000 + startTime->tv_usec;
    uint64_t current_us = (uint64_t) currentTime.tv_sec * 1000000 + currentTime.tv_usec;

    return (current_us - start_us) / 1000;
}

} // namespace WorkerUtils
