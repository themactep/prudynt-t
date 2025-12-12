#ifndef OSD_hpp
#define OSD_hpp

// #include <map>
#include "Config.hpp"
#include "imp_hal.hpp"
#include "schrift.h"
#include <arpa/inet.h>
#include <array>
#include <ifaddrs.h>

#include <imp/imp_osd.h>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/sysinfo.h>
#include <unordered_map>
#include <vector>

struct OSDItem {
  IMPRgnHandle imp_rgn;
  uint8_t *data;
  uint16_t width;
  uint16_t height;
  IMPOSDRgnAttr rgnAttr;
  IMPOSDRgnAttrData *rgnAttrData;
};

struct Glyph {
  int width;
  int height;
  std::vector<uint8_t> bitmap;
  int advance;
  int xmin;
  int ymin;
  SFT_Glyph glyph;
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

  void rotateBGRAImage(uint8_t *&inputImage, uint16_t &width, uint16_t &height, int angle, bool del);
  static void set_pos(IMPOSDRgnAttr *rgnAttr, int x, int y, uint16_t width, uint16_t height, const uint16_t max_width,
                      const uint16_t max_height);
  static uint16_t get_abs_pos(const uint16_t max, const uint16_t size, const int pos);
  int startup_delay{0};
  bool is_started = false;

private:
  // libschrift
  // std::vector<uint8_t> fontData;
  std::unordered_map<char, Glyph> glyphs;
  SFT *sft;
  int load_font();
  int libschrift_init();
  int renderGlyph(const char *characters);
  void drawOutline(uint8_t *image, const Glyph &g, int x, int y, int outlineSize, int WIDTH, int HEIGHT, const uint8_t *strokeColor);
  int calculateTextSize(const char *text, uint16_t &width, uint16_t &height, int outlineSize);
  int drawText(uint8_t *image, const char *text, int WIDTH, int HEIGHT, int outlineSize, unsigned int fill_color,
               unsigned int stroke_color);

  _osd &osd;
  int last_updated_second;

  OSDItem osdTime{};
  OSDItem osdUser{};
  OSDItem osdUptm{};
  OSDItem osdLogo{};
  OSDItem osdBrightness{};

  void set_text(OSDItem *osdItem, IMPOSDRgnAttr *rgnAttr, const char *text, const char *position, int angle,
                unsigned int fill_color, unsigned int stroke_color);
  std::string getConfigPath(const char *itemName);
  struct BrightnessSample {
    float current{-1.0f};
    float average{-1.0f};
    std::string mode{"UNKNOWN"};
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

  bool initialized{0};
  int osdGrp{};
  int encChn{};
  const char *parent;

  char hostname[64];
  char ip[INET_ADDRSTRLEN]{};

  uint16_t stream_width;
  uint16_t stream_height;

  time_t current;
  struct tm *ltime;
  struct timeval tm;

  char timeFormatted[32];
  char uptimeFormatted[32];
  char fps[4];
  char bps[8];
  uint8_t flag{0};
};

#endif
