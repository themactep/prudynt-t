#include "RTSPStatus.hpp"
#include "Logger.hpp"
#include <fstream>

const std::string RTSPStatus::STATUS_BASE_DIR = "/run/prudynt/rtsp/";
std::mutex RTSPStatus::statusMutex;
std::map<std::string, RTSPStatus::StreamInfo> RTSPStatus::activeStreams;

bool RTSPStatus::initialize() {
  std::lock_guard<std::mutex> lock(statusMutex);

  if (!ensureBaseDirectory()) {
    LOG_ERROR("RTSPStatus: failed to create base directory: " << STATUS_BASE_DIR);
    return false;
  }

  activeStreams.clear();
  return true;
}

void RTSPStatus::cleanup() {
  std::lock_guard<std::mutex> lock(statusMutex);

  try {
    if (std::filesystem::exists(STATUS_BASE_DIR)) {
      std::filesystem::remove_all(STATUS_BASE_DIR);
    }
  } catch (const std::exception &e) {
    LOG_ERROR("RTSPStatus: cleanup failed: " << e.what());
  }

  activeStreams.clear();
}

bool RTSPStatus::updateStreamStatus(const std::string &streamName, const StreamInfo &info) {
  std::lock_guard<std::mutex> lock(statusMutex);

  if (!ensureBaseDirectory()) {
    LOG_ERROR("RTSPStatus: base directory not available");
    return false;
  }

  if (!createStreamDirectory(streamName)) {
    LOG_ERROR("RTSPStatus: failed to create stream directory for " << streamName);
    return false;
  }

  bool success = true;
  success &= writeParameter(streamName, "format", info.format);
  success &= writeParameter(streamName, "fps", std::to_string(info.fps));
  success &= writeParameter(streamName, "width", std::to_string(info.width));
  success &= writeParameter(streamName, "height", std::to_string(info.height));
  success &= writeParameter(streamName, "endpoint", info.endpoint);
  success &= writeParameter(streamName, "url", info.url);
  success &= writeParameter(streamName, "bitrate", std::to_string(info.bitrate));
  success &= writeParameter(streamName, "mode", info.mode);
  success &= writeParameter(streamName, "enabled", info.enabled ? "true" : "false");

  if (success) {
    activeStreams[streamName] = info;
  } else {
    LOG_ERROR("RTSPStatus: failed to write one or more parameters for " << streamName);
  }

  return success;
}

bool RTSPStatus::writeCustomParameter(const std::string &streamName, const std::string &parameter,
                                      const std::string &value) {
  std::lock_guard<std::mutex> lock(statusMutex);

  if (!ensureBaseDirectory()) {
    LOG_ERROR("RTSPStatus: base directory not available");
    return false;
  }

  if (!createStreamDirectory(streamName)) {
    LOG_ERROR("RTSPStatus: failed to create stream directory for " << streamName);
    return false;
  }

  return writeParameter(streamName, parameter, value);
}

bool RTSPStatus::removeStreamStatus(const std::string &streamName) {
  std::lock_guard<std::mutex> lock(statusMutex);

  bool success = removeStreamDirectory(streamName);
  if (success) {
    activeStreams.erase(streamName);
  } else {
    LOG_ERROR("RTSPStatus: failed to remove stream status for " << streamName);
  }

  return success;
}

RTSPStatus::StreamInfo RTSPStatus::getStreamStatus(const std::string &streamName) {
  std::lock_guard<std::mutex> lock(statusMutex);

  auto it = activeStreams.find(streamName);
  if (it != activeStreams.end()) {
    return it->second;
  }

  return StreamInfo();
}

bool RTSPStatus::isAvailable() {
  std::lock_guard<std::mutex> lock(statusMutex);
  return std::filesystem::exists(STATUS_BASE_DIR) && std::filesystem::is_directory(STATUS_BASE_DIR);
}

std::vector<std::string> RTSPStatus::getActiveStreams() {
  std::lock_guard<std::mutex> lock(statusMutex);
  std::vector<std::string> streams;

  for (const auto &pair : activeStreams) {
    if (pair.second.enabled) {
      streams.push_back(pair.first);
    }
  }

  return streams;
}

bool RTSPStatus::createStreamDirectory(const std::string &streamName) {
  std::string streamDir = STATUS_BASE_DIR + streamName + "/";

  try {
    std::filesystem::create_directories(streamDir);
    return true;
  } catch (const std::exception &e) {
    LOG_ERROR("RTSPStatus: failed to create stream directory " << streamDir << ": " << e.what());
    return false;
  }
}

bool RTSPStatus::writeParameter(const std::string &streamName, const std::string &parameter,
                                const std::string &value) {
  std::string filePath = STATUS_BASE_DIR + streamName + "/" + parameter;

  try {
    std::ofstream file(filePath);
    if (!file.is_open()) {
      LOG_ERROR("RTSPStatus: failed to open " << filePath);
      return false;
    }

    file << value << std::endl;
    file.close();
    return true;
  } catch (const std::exception &e) {
    LOG_ERROR("RTSPStatus: failed to write " << filePath << ": " << e.what());
    return false;
  }
}

bool RTSPStatus::removeStreamDirectory(const std::string &streamName) {
  std::string streamDir = STATUS_BASE_DIR + streamName + "/";

  try {
    if (std::filesystem::exists(streamDir)) {
      std::filesystem::remove_all(streamDir);
    }
    return true;
  } catch (const std::exception &e) {
    LOG_ERROR("RTSPStatus: failed to remove stream directory " << streamDir << ": " << e.what());
    return false;
  }
}

bool RTSPStatus::ensureBaseDirectory() {
  try {
    if (!std::filesystem::exists(STATUS_BASE_DIR)) {
      std::filesystem::create_directories(STATUS_BASE_DIR);
    }
    return true;
  } catch (const std::exception &e) {
    LOG_ERROR("RTSPStatus: failed to ensure base directory " << STATUS_BASE_DIR << ": " << e.what());
    return false;
  }
}
