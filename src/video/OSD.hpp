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

#if defined(OSD_BURN_TIMESTAMP) && defined(USE_OSD_FONT_LIBSCHRIFT)
#include "schrift.h"
#include <unordered_map>
#endif

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
#if defined(OSD_BURN_TIMESTAMP) && defined(USE_OSD_FONT_LIBSCHRIFT)
  // Backstop for the libschrift font/glyph state: normal teardown goes
  // through exit() (called explicitly by the owner before delete), but
  // shutdownTimestampFont() is idempotent, so this guarantees the font
  // is freed even if a future caller deletes an OSD without calling
  // exit() first. Declared only here (not for the other font builds) so
  // it doesn't cost a real destructor when there's nothing for it to do.
  ~OSD();
#endif
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
  bool is_substream_{false}; // stream1 (sub stream)

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
  //
  // Glyph rasterization is pluggable at build time: the default renders a
  // fixed bitmap font (see util/OSDFont.hpp); -DUSE_OSD_FONT_LIBSCHRIFT
  // (make USE_OSD_FONT_LIBSCHRIFT=1 / build.sh --osd-font-libschrift)
  // switches renderTimestamp() to antialiased TrueType rendering via
  // libschrift instead, reading /usr/share/fonts/default.ttf on the
  // camera. initTimestampFont()/shutdownTimestampFont() are the load/free
  // hooks for that -- no-ops under the bitmap-font build.
  void updateTimestampOverlay();
  void renderTimestamp(const char *text);
#ifdef USE_OSD_FONT_LIBSCHRIFT
  void initTimestampFont();
  void shutdownTimestampFont();
#endif

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

#ifdef USE_OSD_FONT_LIBSCHRIFT
  // Antialiased glyph bitmap, rasterized once per character and cached
  // until ts_scale_ changes (osd.burnin.scale takes effect live).
  struct Glyph {
    int width{0};
    int height{0};
    std::vector<uint8_t> bitmap; // one alpha byte per pixel (coverage)
    int advance{0};
    int xmin{0};
    int ymin{0};
  };
  int libschriftRenderGlyph(const char *characters);

  std::unordered_map<char, Glyph> glyphs_;
  SFT *sft_{nullptr};
  // sft_loadmem() does not copy the font bytes -- it stores this pointer
  // and parses tables from it on every lookup/render call. Must outlive
  // sft_->font, so it's a member, not a local in initTimestampFont().
  std::vector<uint8_t> fontData_;
  bool textRenderingAvailable_{false};
  int lastFontSize_{0};
#endif
#endif

  mutable std::mutex stateMutex_;
};

#endif
