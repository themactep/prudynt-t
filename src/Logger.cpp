#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <sys/time.h>
#include <syslog.h>
#include <unistd.h>

// Undefine conflicting macros from syslog.h
#undef LOG_INFO
#undef LOG_DEBUG
#undef LOG_CRIT
#undef LOG_NOTICE

#define MODULE "LOGGER"

#include "Logger.hpp"

const char *text_levels[] = {"EMERGENCY", "ALERT", "CRITICAL", "ERROR", "WARN",
                             "NOTICE",    "INFO",  "DEBUG",    "TRACE"};

namespace {
void current_timestamp(char *out, size_t out_size) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);

  time_t now = tv.tv_sec;
  struct tm local_tm;
  if (localtime_r(&now, &local_tm) == nullptr) {
    snprintf(out, out_size, "1970-01-01 00:00:00.000");
    return;
  }

  char time_buf[32];
  if (strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &local_tm) ==
      0) {
    snprintf(out, out_size, "1970-01-01 00:00:00.000");
    return;
  }

  snprintf(out, out_size, "%.19s.%03ld", time_buf,
           static_cast<long>(tv.tv_usec / 1000));
}

bool g_syslog_enabled = true;
bool g_logger_initialized = false;
} // namespace

Logger::Level Logger::parseLevel(const std::string &levelStr) {
  if (levelStr == "EMERGENCY")
    return Logger::EMERGENCY;
  if (levelStr == "ALERT")
    return Logger::ALERT;
  if (levelStr == "CRITICAL")
    return Logger::CRIT;
  if (levelStr == "ERROR")
    return Logger::ERROR;
  if (levelStr == "WARN")
    return Logger::WARN;
  if (levelStr == "NOTICE")
    return Logger::NOTICE;
  if (levelStr == "INFO")
    return Logger::INFO;
  if (levelStr == "DEBUG")
    return Logger::DEBUG;
  if (levelStr == "TRACE")
    return Logger::TRACE;
  // Default level if unknown string
  return Logger::INFO; // or any default level you prefer
}

Logger::Level Logger::level = Logger::INFO;

std::mutex Logger::log_mtx;

bool Logger::init(std::string logLevel) {
  const char *syslog_env = std::getenv("PRUDYNT_ENABLE_SYSLOG");
#if defined(PLATFORM_T23)
  g_syslog_enabled = (syslog_env && std::strcmp(syslog_env, "1") == 0);
#else
  g_syslog_enabled = !(syslog_env && std::strcmp(syslog_env, "0") == 0);
#endif

  if (g_syslog_enabled) {
    openlog("prudynt", LOG_PID | LOG_NDELAY, LOG_USER);
  }
  Logger::level = Logger::parseLevel(logLevel);
  g_logger_initialized = true;
  return false;
}

void Logger::setLevel(std::string lvl) {
  LOG_DEBUG("set loglevel to " << lvl);
  Logger::level = Logger::parseLevel(lvl);
}

void Logger::log(Level lvl, std::string module, LogMsg msg) {
  // Filter TRACE logs unless explicitly enabled at runtime
  if (lvl == Logger::TRACE && Logger::level < Logger::TRACE) {
    return; // skip both syslog and console for TRACE when not enabled
  }
  std::unique_lock<std::mutex> lck(log_mtx);
  char timestamp[40];
  current_timestamp(timestamp, sizeof(timestamp));

  // Log to syslog
  if (g_logger_initialized && g_syslog_enabled && Logger::level >= lvl) {
    int syslogPriority;
    switch (lvl) {
    case EMERGENCY:
      syslogPriority = 0;
      break;
    case ALERT:
      syslogPriority = 1;
      break;
    case CRIT:
      syslogPriority = 2;
      break;
    case ERROR:
      syslogPriority = 3;
      break;
    case WARN:
      syslogPriority = 4;
      break;
    case NOTICE:
      syslogPriority = 5;
      break;
    case INFO:
      syslogPriority = 6;
      break;
    case DEBUG:
      syslogPriority = 7;
      break;
    case TRACE:
      syslogPriority = 7;
      break;
    default:
      syslogPriority = 7;
      break; // Default case for undefined levels
    }
    syslog(syslogPriority, "[%s:%s]: %s", text_levels[lvl], module.c_str(),
           msg.log_str.c_str());
  }

  // Log to console
#if defined(PLATFORM_T23)
  std::string line;
  line.reserve(64 + module.size() + msg.log_str.size());
  line.append(timestamp);
  line.append(" [");
  line.append(text_levels[lvl]);
  line.append(":");
  line.append(module);
  line.append("]: ");
  line.append(msg.log_str);
  line.push_back('\n');
  (void)write(STDOUT_FILENO, line.c_str(), line.size());
#else
  std::fprintf(stdout, "%s [%s:%s]: %s\n", timestamp, text_levels[lvl],
               module.c_str(), msg.log_str.c_str());
  std::fflush(stdout);
#endif
}

// Remember to close the syslog
// closelog();
