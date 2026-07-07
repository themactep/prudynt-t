#ifndef OSD_hpp
#define OSD_hpp

#include "Config.hpp"
#include "imp_hal.hpp"
#include <arpa/inet.h>
#include <array>
#include <cstring>
#include <ifaddrs.h>

#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/sysinfo.h>
#include <vector>

class OSD {
public:
  static OSD *createNew(_osd &osd, int osdGrp, int encChn, const char *parent);
  OSD(_osd &osd, int osdGrp, int encChn, const char *parent)
      : osd(osd), osdGrp(osdGrp), encChn(encChn), parent(parent) {
    init();
  }
  void init();
  int exit();
  int start();

  void updateDisplayEverySecond();
  static void *thread_entry(void *arg);

  /// Return current OSD state as JSON for SEI metadata embedding.
  std::string getSEIJson();

  /// Return current OSD state as a plaintext string (UTF-8, newline-separated)
  /// suitable for T.140 text streaming.
  std::string getPlaintextInfo();

  int startup_delay_ticks{0};
  bool is_started = false;

private:
  _osd &osd;
  int last_updated_second;

  struct BrightnessSample {
    float current{-1.0f};
    float average{-1.0f};
    std::string mode{"UNKNOWN"};
    int total_gain{-1};
    bool valid{false};
  };

  class BrightnessMeter {
  public:
    BrightnessMeter();
    BrightnessSample measure();

  private:
    struct IspStats {
      int integrationTime{-1};
      int maxIntegrationTime{-1};
      int analogGain{-1};
      int digitalGain{-1};
      int ispDigitalGain{-1};
      int evValue{-1};
      int currentBrightness{-1};
      std::string mode;
    };

    bool readIspStats(IspStats &stats);
    float computeFromStats(const IspStats &stats, std::string &mode) const;
    float fallbackTimeBased(std::string &mode) const;
    void updateHistory(float value);
    float historyAverage() const;

    static constexpr size_t historySize = 10;
    std::array<float, historySize> history;
    size_t historyIndex;
    bool historyFilled;
    bool lastReadFailed;
  } brightnessMeter;

  std::string buildBrightnessText(const BrightnessSample &sample);
  void updateBrightnessText();
  std::string lastBrightnessText;

  IMPEncoderCHNAttr channelAttributes;

  int osdGrp{};
  int encChn{};
  const char *parent;

  char hostname[64];
  char ip[INET_ADDRSTRLEN]{};

  uint16_t stream_width;
  uint16_t stream_height;
  int stream_rotation{0};

  time_t current;
  struct tm *ltime;

  char timeFormatted[32];
  char uptimeFormatted[32];
  uint8_t flag{0};

  std::string formattedUsertext_;
  mutable std::mutex stateMutex_;
};

#endif
