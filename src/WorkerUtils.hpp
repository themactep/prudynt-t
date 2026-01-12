#ifndef WORKERUTILS_HPP
#define WORKERUTILS_HPP

#include "globals.hpp" // for binary_semaphore_compat
#include <sys/time.h>

// Struct used for signaling thread startup completion
struct StartHelper {
  int encChn;
  binary_semaphore_compat has_started{0};
};

namespace WorkerUtils {

unsigned long long tDiffInMs(struct timeval *startTime);

void getEnv_gpio(char* fileName);
bool openMMIO (int fd);

bool setGPIO(int gpio_num, bool on);  // by pin number
bool setGPIObyName(std::string gpio_name, bool on);  // by pin function name
bool getGPIO(int gpio_num); // by pin number
bool getGPIObyName(std::string gpio_name);  // by pin name

int getGPIO_Pin_byName(std::string gpio_name);  // get pin number by name
bool getGPIO_index_byName(std::string gpio_name, int *index);  // get pin index for gpio_keys by name
bool setIRCUT(bool on);
bool getIRCUT();
void setDAYNIGHT(bool on);
bool getDAYNIGHT();

} // namespace WorkerUtils

#endif // WORKERUTILS_HPP
