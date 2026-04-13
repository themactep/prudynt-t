#ifndef IMPSystem_hpp
#define IMPSystem_hpp

#include "Config.hpp"
#include "Logger.hpp"
#include <cstdlib>
#include <cstring>
#include <imp/imp_framesource.h>
#include <imp/imp_isp.h>
#include <imp/imp_osd.h>
#include <imp/imp_system.h>
#include <memory>
#include <sys/time.h>
#include <sysutils/su_base.h>

class IMPSystem {
public:
  static IMPSystem *createNew();

  IMPSystem() {
    if (init() != 0 && Logger::level != Logger::DEBUG) {
      throw std::invalid_argument("error initializing the imp system.");
    };

    /* https://github.com/rara64/prudynt-t/commit/7eda99252b0d1309cbe134dc4143182eda9c21bd
     */
    struct timespec timeSinceBoot;
    clock_gettime(CLOCK_MONOTONIC, &timeSinceBoot);

    uint64_t imp_time_base = (static_cast<uint64_t>(timeSinceBoot.tv_sec) * 1000000ULL) +
                             (static_cast<uint64_t>(timeSinceBoot.tv_nsec) / 1000ULL);

#if defined(PLATFORM_T23)
    const char *force_rebase_ts = std::getenv("PRUDYNT_FORCE_REBASE_TS");
    if (force_rebase_ts && force_rebase_ts[0] != '\0' && std::strcmp(force_rebase_ts, "1") == 0) {
      IMP_System_RebaseTimeStamp(imp_time_base);
      LOG_DEBUG("IMP_System_RebaseTimeStamp(" << imp_time_base << ");");
    } else {
      LOG_WARN("IMPSystem: skipping IMP_System_RebaseTimeStamp on T23 (set PRUDYNT_FORCE_REBASE_TS=1 to enable)");
    }
#else
    IMP_System_RebaseTimeStamp(imp_time_base);
    LOG_DEBUG("IMP_System_RebaseTimeStamp(" << imp_time_base << ");");
#endif
  }

  ~IMPSystem() {
#if defined(PLATFORM_T23)
    const char *force_destroy = std::getenv("PRUDYNT_FORCE_IMP_DESTROY");
    if (force_destroy && force_destroy[0] != '\0' && std::strcmp(force_destroy, "1") == 0) {
      destroy();
    } else {
      LOG_WARN("IMPSystem: skipping vendor destroy path on T23 (set PRUDYNT_FORCE_IMP_DESTROY=1 to enable)");
    }
#else
    destroy();
#endif
  };

  int init();
  int destroy();

private:
  IMPSensorInfo sinfo{};
  IMPSensorInfo create_sensor_info(const char *sensor_name);
};

#endif
