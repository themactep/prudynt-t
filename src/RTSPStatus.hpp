#ifndef RTSP_STATUS_HPP
#define RTSP_STATUS_HPP

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

class RTSPStatus {
public:
  struct StreamInfo {
    std::string format;
    int fps;
    int width;
    int height;
    std::string endpoint;
    std::string url;
    int bitrate;
    std::string mode;
    bool enabled;

    StreamInfo() : fps(0), width(0), height(0), bitrate(0), enabled(false) {
    }
  };

  static bool initialize();
  static void cleanup();
  static bool updateStreamStatus(const std::string &streamName,
                                 const StreamInfo &info);
  static bool writeCustomParameter(const std::string &streamName,
                                   const std::string &parameter,
                                   const std::string &value);
  static bool removeStreamStatus(const std::string &streamName);
  static StreamInfo getStreamStatus(const std::string &streamName);
  static bool isAvailable();
  static std::vector<std::string> getActiveStreams();

private:
  static const std::string STATUS_BASE_DIR;
  static std::mutex statusMutex;
  static std::map<std::string, StreamInfo> activeStreams;

  static bool createStreamDirectory(const std::string &streamName);
  static bool writeParameter(const std::string &streamName,
                             const std::string &parameter,
                             const std::string &value);
  static bool removeStreamDirectory(const std::string &streamName);
  static bool ensureBaseDirectory();
};

#endif // RTSP_STATUS_HPP
