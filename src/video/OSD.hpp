#ifndef OSD_hpp
#define OSD_hpp

#include "config/Config.hpp"
#include "isp/imp_hal.hpp"
#include <arpa/inet.h>
#include <array>
#include <cstring>
#include <ifaddrs.h>

#include <map>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/sysinfo.h>
#include <vector>

struct OSDElement {
  std::string name;
  std::string type;      // "timestamp", "hostname", "uptime", "gain", "text"
  std::string format;
  std::string position;
  std::string text;      // current rendered text (populated each second)
  bool in_subtitle = false;  // include in RTP subtitle track
};

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

  std::string getSEIJson();
  std::string getPlaintextInfo();

  int startup_delay_ticks{0};
  bool is_started = false;

private:
  void loadElements();
  void updateElementText();

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
  std::string lastBrightnessText;
  void updateBrightnessText();

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

  std::vector<OSDElement> elements_;
  uint8_t flag{0};

#ifdef OSD_BURN_TIMESTAMP
  // Burned-in timestamp overlay (hardware OSD region on the encoder group).
  // Renders "YYYY-MM-DD HH:MM:SS" with a small embedded bitmap font so no
  // external font/library is required. The region is created lazily on the
  // first update because IMPEncoder creates/binds the OSD group only after
  // this OSD object is constructed. Compile-time opt-in via -DOSD_BURN_TIMESTAMP
  // (make USE_OSD_BURNIN=1 / build.sh --osd-burnin).
  void updateTimestampOverlay();
  void renderTimestamp(const char *text);

  IMPRgnHandle ts_rgn_{INVHANDLE};
  IMPOSDRgnAttr ts_attr_{};
  std::vector<uint8_t> ts_buf_;   // BGRA pixel buffer for the region
  uint16_t ts_width_{0};
  uint16_t ts_height_{0};
  int ts_scale_{2};
  int ts_margin_{8};
  bool ts_region_created_{false};
  bool osd_group_started_{false};
  std::string last_ts_text_;
#endif

  mutable std::mutex stateMutex_;
};

#endif
