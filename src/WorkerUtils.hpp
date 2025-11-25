#ifndef WORKERUTILS_HPP
#define WORKERUTILS_HPP

#include <sys/time.h>
#include "globals.hpp" // for binary_semaphore_compat

// Struct used for signaling thread startup completion
struct StartHelper
{
    int encChn;
    binary_semaphore_compat has_started{0};
};

namespace WorkerUtils {

unsigned long long tDiffInMs(struct timeval *startTime);

} // namespace WorkerUtils

#endif // WORKERUTILS_HPP
