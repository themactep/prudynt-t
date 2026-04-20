#ifndef Motion_hpp
#define Motion_hpp

#include "Config.hpp"
#include "Logger.hpp"
#include "globals.hpp"
#include "imp/imp_ivs.h"
#include "imp/imp_ivs_move.h"
#include "imp/imp_system.h"
#include "imp_hal.hpp"
#include <atomic>
#include <memory>
#include <thread>

class Motion {
public:
  void detect();
  static void *run(void *arg);
  int init();
  int exit();

private:
  int ivsChn = 0;
  int ivsGrp = 0;

  std::string getConfigPath(const char *itemName);

  std::atomic<bool> moving;
  std::atomic<bool> indicator;
  IMP_IVS_MoveParam move_param;
  IMPIVSInterface *move_intf;
  std::thread detect_thread;

  IMPCell fs = {};
  IMPCell ivs_cell = {};

  IMPEncoderCHNAttr channelAttributes;
};

#endif /* Motion_hpp */
