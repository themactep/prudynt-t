#include "util/WorkerUtils.hpp"

#include <cstddef>
#include <sys/prctl.h>

namespace WorkerUtils {

void setCurrentThreadName(const char *name) {
  if (name && name[0])
    prctl(PR_SET_NAME, name, 0, 0, 0);
}

unsigned long long tDiffInMs(struct timeval *startTime) {
  struct timeval currentTime;
  gettimeofday(&currentTime, NULL);

  long seconds = currentTime.tv_sec - startTime->tv_sec;
  long microseconds = currentTime.tv_usec - startTime->tv_usec;

  unsigned long long milliseconds = (seconds * 1000) + (microseconds / 1000);

  return milliseconds;
}

} // namespace WorkerUtils
