#include "WorkerUtils.hpp"

#include <cstddef>

#include "Logger.hpp"
#include <iostream>
#include <string>
#include <algorithm>
#include <cctype> 
#include <fstream> 
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

namespace WorkerUtils {

unsigned long long tDiffInMs(struct timeval *startTime) {
  struct timeval currentTime;
  gettimeofday(&currentTime, NULL);

  long seconds = currentTime.tv_sec - startTime->tv_sec;
  long microseconds = currentTime.tv_usec - startTime->tv_usec;

  unsigned long long milliseconds = (seconds * 1000) + (microseconds / 1000);

  return milliseconds;
}

// GPIO env 
static std::vector<std::string> gpio_keys = {
    "gpio_ir850",
    "gpio_ir940",
    "gpio_white",
    "gpio_ircut",
    "gpio_ircut_1",
    "gpio_ircut_2",
    "gpio_sensor_switch"
};

uint32_t gpio_env[7];

bool ircut_state = true;
bool daynight_mode = false;

// Ingenic T23 GPIO register def
#define GPIO_BASE		    0x10010000
#define GPIO_PortA_PAT0		0x10010040
#define PAT0_SET_REG 		0x10010044
#define PAT0_CLEAR_REG 		0x10010048
#define GPIO_PortB_PAT0		0x10011040
#define PBT0_SET_REG 		0x10011044
#define PBT0_CLEAR_REG 		0x10011048

typedef struct {
	uint32_t REG[1024];
} XHAL_HandleTypeDef;

volatile XHAL_HandleTypeDef *port;

/* #### todo:  need to add test if ircut only has one number for single or more than 2 for others */
/*  get environment GPIO pin variables from file */
void getEnv_gpio(char* fileName) {

    std::ifstream inputFile;
    uint32_t* env = gpio_env;

    inputFile.open(fileName); 

    if (inputFile.is_open()) {
        LOG_DEBUG(__func__ << " ENV File Open:  " << fileName );

        int i, k;
        int q1, q2;
        int pin;
        std::string line, str1, s, s1, s2;

        int key_size = sizeof(gpio_keys)/4;
        while (std::getline(inputFile, line)) {
            //LOG_DEBUG("**** " << line); // Print the line read from the file
            if (line.find("gpio") == 0) {
                env = gpio_env;
                for (const std::string& key : gpio_keys) {
                    k = line.find(key);
                    if ( k == 0) {
                        k = line.find("=");
                        if (k == std::string::npos) {
                            break;
                        }  // get value
                        q1 = line.find('"');
                        if (q1 == std::string::npos) {
                            break;
                        }
                        q2 = line.find('"', q1 + 1);
                        if (q2 == std::string::npos) {
                            break;
                        }
                        str1 = line.substr(q1 + 1, (q2 - q1 - 1));
                        if (key == "gpio_ircut") {  // ircut_1/2 set for pin numbers
                            k = str1.find(" ");
                            s1 = str1.substr(0,k);
                            s2 = str1.substr(k+1,str1.length()-k-1);
                            s1.erase(std::remove_if(s1.begin(), s1.end(), [](unsigned char c) {
                                return !std::isdigit(c);
                            }), s1.end());
                            s2.erase(std::remove_if(s2.begin(), s2.end(), [](unsigned char c) {
                                return !std::isdigit(c);
                            }), s2.end());
                            *env = 99;      // ircut uses two pins.  99 (> 64) value to prevent it sets the GPIO.
                            pin = std::stoi(s1);  // pin 1
                            env++;
                            *env = pin;
                            env++;
                            pin = std::stoi(s2);  // pin 2
                            *env = pin;
                        } else // single pin settings
                        {
                            str1.erase(std::remove_if(str1.begin(), str1.end(), [](unsigned char c) {
                                return !std::isdigit(c);
                            }), str1.end());
                            pin = std::stoi(str1);
                            *env = pin;
                        }
                        break;  // found gpio name in list
                    }  // gpio
                    env++;
                }  // gpio name list
            }  // gpio
        }  // lines
        inputFile.close();

        LOG_DEBUG(__func__ << "**** GPIO ENV pins: **** ");
        env = gpio_env;
        for (const std::string& key : gpio_keys) {
            LOG_DEBUG(" --> " << key << " : " << *env);
            env++;
        }
    } else {
        LOG_DEBUG(__func__ << "Error: Unable to open file: " << fileName);
    }
}

/* set/clear GPIO by pin number */
bool setGPIO(int gpio_num, bool on) {
    uint32_t gpio_set_reg;
    uint32_t gpio_clear_reg;
    uint32_t gpio_port_reg;
    uint32_t gpio_mask;
  
    if (gpio_num < 32) {
        gpio_set_reg = PAT0_SET_REG;
        gpio_clear_reg = PAT0_CLEAR_REG;
        gpio_port_reg = GPIO_PortA_PAT0;
    } else if (gpio_num < 64) {
        gpio_set_reg = PBT0_SET_REG;
        gpio_clear_reg = PBT0_CLEAR_REG;
        gpio_port_reg = GPIO_PortB_PAT0;
    } else {
        LOG_DEBUG(__func__ << " GPIO is out of range: " << gpio_num);
        return false;
    }

    gpio_mask = 0x00000001 << gpio_num;
    if (on) {	// set bit:  
        port->REG[(gpio_set_reg - GPIO_BASE)/4] = gpio_mask;
    }
    else { // clear bit:  
        port->REG[(gpio_clear_reg - GPIO_BASE)/4] = gpio_mask;
    }
   return true;
}

/* get GPIO by state by number */
bool getGPIO(int gpio_num) {
    uint32_t gpio_port_reg;
    uint32_t gpio_mask;

    if (gpio_num < 32) {
        gpio_port_reg = GPIO_PortA_PAT0;
    } else if (gpio_num < 64) {
        gpio_port_reg = GPIO_PortB_PAT0;
    } else {
        LOG_DEBUG(__func__ << " GPIO is out of range: " << gpio_num);
        return false;
    }

    gpio_mask = 0x00000001 << gpio_num;
    return (port->REG[(gpio_port_reg - GPIO_BASE)/4] & gpio_mask);
}

/* set pin state by name */
bool setGPIObyName(std::string gpio_name, bool on) {
    uint32_t* env = gpio_env;
    for (const std::string& key : gpio_keys) {
        if(gpio_name.compare(key) == 0) {
            if(gpio_name.compare("gpio_ircut") == 0) {
                ircut_state = on;
                return ircut_state;
            } else {
                return setGPIO(*env, on);
            }
        }
        env++;
    }
    return false;
}

/* get pin state by name */
bool getGPIObyName(std::string gpio_name) {
    uint32_t* env = gpio_env;
    bool state;

    for (const std::string& key : gpio_keys) {
        if(gpio_name.compare(key) == 0) {
            if(gpio_name.compare("gpio_ircut") == 0) {
                state = ircut_state;
            } else {
                state = getGPIO(*env);
            }
            return state;
        }
        env++;
    }
    return false;
}

/* get pin number by name */
int getGPIO_Pin_byName(std::string gpio_name) {
    uint32_t* env = gpio_env;
    for (const std::string& key : gpio_keys) {
        if(gpio_name.compare(key) == 0) {
            return *env;
        }
        env++;
    }
    return 0;
}

/* get pin index by name */
bool getGPIO_index_byName(std::string gpio_name, int *index) {
    *index = 0;
    for (const std::string& key : gpio_keys) {
        if(gpio_name.compare(key) == 0) {
            LOG_DEBUG(__func__ << " found pin, index =  " << *index);
            return true;
        }
        *index++;
    }
    return false;
}

/*  set MMIO access for the registers (port) */
bool openMMIO (int fd) 
{
    uint32_t* phys_mem;

    //  GPIO mmio registers access setup
    fd = open("/dev/mem", O_RDWR|O_SYNC);

    if (fd < 0) {
        LOG_DEBUG("error: failed to open /dev/mem");
        return false;
    }
    phys_mem = static_cast<uint32_t*> (mmap(NULL, 0x10020000, PROT_READ|PROT_WRITE, MAP_SHARED, fd, GPIO_BASE));  
    if (phys_mem == MAP_FAILED) {
        LOG_DEBUG("error: mmap failed");
        return false;
    }
    /*  The offset in mmap does not work for mmio, use <port> to set at start of the offset */
    port = (XHAL_HandleTypeDef *) (phys_mem + (GPIO_BASE / 4));  
    return true;
}

bool setIRCUT(bool on) {
    std::string gpio_name;
    if (on) {
        ircut_state = true;

        gpio_name = "gpio_ircut_1";
        setGPIObyName(gpio_name, true);
        gpio_name = "gpio_ircut_2";
        setGPIObyName(gpio_name, false);
        usleep(10000);
        gpio_name = "gpio_ircut_1";
        setGPIObyName(gpio_name, false);
    } else
    {
        ircut_state = false;

        gpio_name = "gpio_ircut_1";
        setGPIObyName(gpio_name, false);
        gpio_name = "gpio_ircut_2";
        setGPIObyName(gpio_name, true);
        usleep(10000);
        gpio_name = "gpio_ircut_2";
        setGPIObyName(gpio_name, false);
    }
    /* make sure we have them all off */
    usleep(20000);
    gpio_name = "gpio_ircut_1";
    setGPIObyName(gpio_name, false);
    gpio_name = "gpio_ircut_2";
    setGPIObyName(gpio_name, false);
    return ircut_state;
}

bool getIRCUT() {
    return ircut_state;
}

void setDAYNIGHT(bool on) {
    daynight_mode = on;
}  

bool getDAYNIGHT() {
    return daynight_mode;
}  

} // namespace WorkerUtils
