#include "IPCServer.hpp"
#include "JPEGWorker.hpp"
#include "JsonAPI.hpp" // JSON processing via jct
#include "Logger.hpp"
#include "globals.hpp"
#include "version.hpp"
#include <sys/sysinfo.h>

#include <arpa/inet.h>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "SEIWriter.hpp"
#include <unistd.h>
#include <vector>

// Local snapshot helper: return a clean JPEG starting from SOI
bool get_snapshot_ch_local(int ch, std::vector<unsigned char> &image) {
  std::unique_lock lck(mutex_main);
  if (ch < 0 || ch >= NUM_VIDEO_CHANNELS || !global_jpeg[ch])
    return false;
  auto &buf = global_jpeg[ch]->snapshot_buf;
  if (buf.empty())
    return false;

  // Find first JPEG SOI marker 0xFF 0xD8 and slice from there
  size_t soi = std::string::npos;
  for (size_t i = 0; i + 1 < buf.size(); ++i) {
    if (buf[i] == 0xFF && buf[i + 1] == 0xD8) {
      soi = i;
      break;
    }
  }
  if (soi != std::string::npos) {
    image.assign(buf.begin() + soi, buf.end());
  } else {
    // Fallback: return as-is if no SOI found
    image = buf;
  }
  return true;
}

static bool process_json_with_jct(const std::string &req, std::string &resp) {
  return JsonAPI::process_json(req, resp);
}

static int set_cloexec(int fd) {
  return fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
}

static bool write_full(int fd, const void *data, size_t len) {
  const char *p = static_cast<const char *>(data);
  while (len > 0) {
    ssize_t n = ::write(fd, p, len);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

static const char *SOCK_PATH = "/run/prudynt/prudynt.sock";

IPCServer::IPCServer() {
}
IPCServer::~IPCServer() {
  stop();
}

void IPCServer::configure_http(int port, bool enabled) {
  http_port_ = port;
  http_enabled_ = enabled && port > 0;
}

void IPCServer::start() {
  bool expected = false;
  if (running_.compare_exchange_strong(expected, true)) {
    th_ = std::thread(&IPCServer::server_loop, this);
    if (http_enabled_ && http_port_ > 0) {
      http_running_.store(true);
      http_th_ = std::thread(&IPCServer::http_loop, this);
    }
  }
}

void IPCServer::stop() {
  bool expected = true;
  if (running_.compare_exchange_strong(expected, false)) {
    // Wake accept by connecting to our own socket
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) {
      sockaddr_un sa{};
      sa.sun_family = AF_UNIX;
      std::strncpy(sa.sun_path, SOCK_PATH, sizeof(sa.sun_path) - 1);
      connect(fd, (sockaddr *)&sa, sizeof(sa));
      close(fd);
    }
    if (th_.joinable())
      th_.join();
  }

  if (http_running_.exchange(false)) {
    if (http_listen_fd_ >= 0) {
      ::shutdown(http_listen_fd_, SHUT_RDWR);
      ::close(http_listen_fd_);
      http_listen_fd_ = -1;
    }
    if (http_th_.joinable()) {
      http_th_.join();
    }
  }
}

void IPCServer::server_loop() {
  // Ensure directory exists
  {
    const char *dir = "/run/prudynt";
    ::mkdir(dir, 0775);
  }
  ::unlink(SOCK_PATH);

  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s < 0) {
    LOG_ERROR("IPC: socket() failed: " << strerror(errno));
    return;
  }
  set_cloexec(s);

  sockaddr_un sa{};
  sa.sun_family = AF_UNIX;
  std::strncpy(sa.sun_path, SOCK_PATH, sizeof(sa.sun_path) - 1);
  if (bind(s, (sockaddr *)&sa, sizeof(sa)) < 0) {
    LOG_ERROR("IPC: bind(" << SOCK_PATH << ") failed: " << strerror(errno));
    close(s);
    return;
  }
  // permissions: allow root+www/cgi
  ::chmod(SOCK_PATH, 0660);

  if (listen(s, 4) < 0) {
    LOG_ERROR("IPC: listen failed: " << strerror(errno));
    close(s);
    return;
  }

  LOG_INFO("IPC: listening on " << SOCK_PATH);

  while (running_) {
    int c = accept(s, nullptr, nullptr);
    if (c < 0) {
      if (errno == EINTR)
        continue;
      if (!running_)
        break;
      LOG_ERROR("IPC: accept failed: " << strerror(errno));
      continue;
    }
    set_cloexec(c);
    // Handle each client in a detached thread so EVENTS can stream without
    // blocking accept
    std::thread(
        [this](int client_fd) {
          this->handle_client(client_fd);
          ::close(client_fd);
        },
        c)
        .detach();
  }

  close(s);
  ::unlink(SOCK_PATH);
}

void IPCServer::http_loop() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    LOG_ERROR("HTTP API: socket() failed: " << strerror(errno));
    http_running_.store(false);
    return;
  }

  http_listen_fd_ = fd;

  int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  // Bind to all interfaces — API key auth secures external access
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(http_port_));
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    LOG_ERROR("HTTP API: bind(" << http_port_
                                << ") failed: " << strerror(errno));
    ::close(fd);
    http_listen_fd_ = -1;
    http_running_.store(false);
    return;
  }

  if (::listen(fd, 8) < 0) {
    LOG_ERROR("HTTP API: listen() failed: " << strerror(errno));
    ::close(fd);
    http_listen_fd_ = -1;
    http_running_.store(false);
    return;
  }

  LOG_INFO("HTTP API: listening on port " << http_port_ << " (/api/v1/config)");

  while (http_running_.load()) {
    sockaddr_in cli{};
    socklen_t clilen = sizeof(cli);
    int cfd = ::accept(fd, reinterpret_cast<sockaddr *>(&cli), &clilen);
    if (cfd < 0) {
      if (errno == EINTR)
        continue;
      if (!http_running_.load())
        break;
      continue;
    }
    std::thread(&IPCServer::handle_http_client, this, cfd).detach();
  }

  if (http_listen_fd_ >= 0) {
    ::close(http_listen_fd_);
    http_listen_fd_ = -1;
  }
  http_running_.store(false);
}

static std::string trim(const std::string &s) {
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
    ++start;
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
    --end;
  return s.substr(start, end - start);
}

int IPCServer::handle_http_client(int fd) {
  // Light-weight HTTP parser; assumes Content-Length is provided
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  std::string req;
  char buf[2048];
  ssize_t r = 0;
  while (req.find("\r\n\r\n") == std::string::npos) {
    r = ::recv(fd, buf, sizeof(buf), 0);
    if (r <= 0)
      break;
    req.append(buf, buf + r);
    if (req.size() > 65536)
      break; // avoid abuse
  }

  size_t hdr_end = req.find("\r\n\r\n");
  if (hdr_end == std::string::npos) {
    ::close(fd);
    return -1;
  }

  // Parse request line
  std::string method;
  std::string path;
  size_t line_end = req.find('\n');
  if (line_end != std::string::npos) {
    std::string line = req.substr(0, line_end);
    size_t sp1 = line.find(' ');
    size_t sp2 = line.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
    if (sp1 != std::string::npos && sp2 != std::string::npos) {
      method = line.substr(0, sp1);
      path = line.substr(sp1 + 1, sp2 - (sp1 + 1));
    }
  }

  // Headers
  size_t body_start = hdr_end + 4;
  int content_length = 0;
  bool wants_sse = false;
  std::string api_key_header;
  size_t pos = req.find('\n');
  while (pos != std::string::npos && pos < hdr_end) {
    size_t next = req.find('\n', pos + 1);
    size_t line_start = (pos == std::string::npos) ? 0 : pos + 1;
    std::string line = trim(req.substr(
        line_start, (next == std::string::npos ? hdr_end : next) - line_start));
    pos = next;
    if (line.empty())
      continue;
    size_t colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    std::string key = line.substr(0, colon);
    std::string val = trim(line.substr(colon + 1));
    for (auto &c : key)
      c = std::tolower(static_cast<unsigned char>(c));
    if (key == "content-length") {
      content_length = std::atoi(val.c_str());
    } else if (key == "accept" && val.find("text/event-stream") != std::string::npos) {
      wants_sse = true;
    } else if (key == "x-api-key") {
      api_key_header = val;
    }
  }

  // Read body if not fully buffered
  std::string body = req.substr(body_start);
  while (content_length > 0 && static_cast<int>(body.size()) < content_length) {
    r = ::recv(fd, buf, sizeof(buf), 0);
    if (r <= 0)
      break;
    body.append(buf, buf + r);
  }

  auto send_response = [&](int code, const std::string &ctype,
                           const std::string &payload) {
    char hdr[512];
    int n = snprintf(
        hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: "
        "%zu\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        code,
        (code == 200 ? "OK" : (code == 404 ? "Not Found" : "Bad Request")),
        ctype.c_str(), payload.size());
    write_full(fd, hdr, static_cast<size_t>(n));
    if (!payload.empty()) {
      write_full(fd, payload.data(), payload.size());
    }
  };

  // Handle CORS preflight
  if (method == "OPTIONS") {
    const char *cors = "HTTP/1.1 204 No Content\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
      "Access-Control-Allow-Headers: Content-Type, X-API-Key\r\n"
      "Access-Control-Max-Age: 86400\r\n"
      "Connection: close\r\n\r\n";
    write_full(fd, cors, strlen(cors));
    ::close(fd);
    return 0;
  }

  if (method == "GET" && path == "/api/v1/osd-sei") {
    if (wants_sse) {
      // SSE streaming mode: keep connection open, push events every 2s.
      // Run in a detached thread so we don't block the HTTP accept loop.
      int client_fd = fd;
      std::thread([this, client_fd]() {
        const char *sse_hdr =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "Pragma: no-cache\r\n"
            "Expires: 0\r\n"
            "\r\n";
        if (!write_full(client_fd, sse_hdr, std::strlen(sse_hdr))) {
          ::close(client_fd);
          return;
        }

        // Write initial retry directive
        const char *retry_line = "retry: 2000\n\n";
        if (!write_full(client_fd, retry_line, std::strlen(retry_line))) {
          ::close(client_fd);
          return;
        }

        char buf[4096];
        while (running_.load()) {
          std::string sei_json;
          for (int ch = 0; ch < NUM_VIDEO_CHANNELS && sei_json.empty(); ++ch) {
            auto vs = global_video[ch];
            if (vs && vs->imp_encoder && vs->imp_encoder->osd) {
              sei_json = vs->imp_encoder->osd->getSEIJson();
            }
          }
          if (sei_json.empty()) {
            sei_json = "{}";
          }

          int n = std::snprintf(buf, sizeof(buf),
                                "data: %s\n\n", sei_json.c_str());
          if (n > 0 && static_cast<size_t>(n) < sizeof(buf)) {
            if (!write_full(client_fd, buf, static_cast<size_t>(n))) {
              break; // client disconnected
            }
          }

          // Sleep 2s, but break if server is shutting down
          for (int i = 0; i < 20 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
          if (!running_.load())
            break;
        }
        ::close(client_fd);
      }).detach();
      return 0; // fd ownership transferred to thread
    }

    // One-shot JSON response (backward compat for CGI relay / direct poll)
    std::string sei_json;
    for (int ch = 0; ch < NUM_VIDEO_CHANNELS && sei_json.empty(); ++ch) {
      auto vs = global_video[ch];
      if (vs && vs->imp_encoder && vs->imp_encoder->osd) {
        sei_json = vs->imp_encoder->osd->getSEIJson();
      }
    }
    if (sei_json.empty()) {
      send_response(200, "application/json", "{}");
    } else {
      send_response(200, "application/json", sei_json);
    }
    ::close(fd);
    return 0;
  }

  // Verify API key for config endpoints (same key as web UI)
  auto verify_api_key = [&](const std::string &provided) -> bool {
    if (provided.empty()) return false;
    std::ifstream kf("/etc/thingino-api.key");
    if (!kf.is_open()) return false;
    std::string stored;
    std::getline(kf, stored);
    // trim trailing whitespace/newline
    while (!stored.empty() && (stored.back() == '\n' || stored.back() == '\r' || stored.back() == ' '))
      stored.pop_back();
    return provided == stored;
  };

  if (method != "POST" || path != "/api/v1/config") {
    send_response(404, "text/plain", "not found\n");
    ::close(fd);
    return 0;
  }

  if (!api_key_header.empty() && verify_api_key(api_key_header))
    ; // authenticated
  else {
    send_response(401, "application/json", "{\"error\":\"Authentication required. Use X-API-Key header\"}\n");
    ::close(fd);
    return -1;
  }

  if (content_length <= 0) {
    send_response(400, "text/plain", "missing content-length\n");
    ::close(fd);
    return -1;
  }

  std::string resp_json;
  bool ok = process_json_with_jct(body, resp_json);
  if (!ok) {
    send_response(400, "application/json", "{\"error\":\"invalid_request\"}\n");
  } else {
    send_response(200, "application/json", resp_json);
  }

  ::close(fd);
  return 0;
}

static bool starts_with(const std::string &s, const char *pfx) {
  return s.rfind(pfx, 0) == 0; // prefix match
}

int IPCServer::handle_client(int fd) {
  // Read all into buffer until EOF
  std::string req;
  char buf[2048];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0)
    req.append(buf, buf + n);

  if (req.empty())
    return 0;

  // Determine command
  if (starts_with(req, "JSON ") || req[0] == '{') {
    const char *json = req[0] == '{' ? req.c_str() : (req.c_str() + 5);
    std::string resp;
    if (process_json_with_jct(json, resp)) {
      write(fd, resp.c_str(), resp.size());
      write(fd, "\n", 1);
    } else {
      const char *err = "{\"error\":\"json_process_failed\"}\n";
      write(fd, err, strlen(err));
    }
    return 0;
  }

  if (starts_with(req, "SNAPSHOT")) {
    // parse ch and q (optional)
    int ch = 0;
    int q = -1;
    // very small parser
    auto find_kv = [&](const char *key) -> int {
      size_t pos = req.find(key);
      if (pos == std::string::npos)
        return -999999;
      pos += std::strlen(key);
      while (pos < req.size() && (req[pos] == ' ' || req[pos] == '='))
        pos++;
      size_t start = pos;
      while (pos < req.size() && isdigit(static_cast<unsigned char>(req[pos])))
        pos++;
      if (start == pos)
        return -999999;
      return std::stoi(req.substr(start, pos - start));
    };
    int v;
    v = find_kv("ch");
    if (v != -999999)
      ch = v;
    v = find_kv("q");
    if (v != -999999)
      q = v;

    // optional: quality override (like HTTP path)
    if (ch >= 0 && ch < NUM_VIDEO_CHANNELS && global_jpeg[ch]) {
      if (!JPEGWorker::ensure_running(ch)) {
        const char *err = "ERR jpeg_unavailable\n";
        write(fd, err, strlen(err));
        return 0;
      }
      if (q >= 1 && q <= 100)
        global_jpeg[ch]->quality_override = q;
    }
    // Signal demand to speed up capture and wake JPEG worker if idle
    if (ch >= 0 && ch < NUM_VIDEO_CHANNELS && global_jpeg[ch]) {
      global_jpeg[ch]->request();
    }

    // Try to get a fresh snapshot, waiting briefly if not yet available
    std::vector<unsigned char> img;
    const int max_wait_ms = 250;
    int waited = 0;
    while (!(get_snapshot_ch_local(ch, img) && !img.empty()) &&
           waited < max_wait_ms) {
      usleep(10 * 1000);
      waited += 10;
    }
    // Fallback: if requested channel isn't available, try ch0 to avoid
    // client hangs
    if (img.empty() && ch != 0) {
      get_snapshot_ch_local(0, img);
    }
    if (!img.empty()) {
      // Protocol: "OK <len>\n<bytes>"
      char hdr[64];
      int len = (int)img.size();
      int m = snprintf(hdr, sizeof(hdr), "OK %d\n", len);
      (void)write_full(fd, hdr, (size_t)m);
      (void)write_full(fd, (const char *)img.data(), (size_t)len);
    } else {
      const char *err = "ERR no_image\n";
      write(fd, err, strlen(err));
    }
    return 0;
  }

  if (starts_with(req, "MJPEG")) {
    // Parse: MJPEG ch=<0|1> w=<W> h=<H> f=<FPS> q=<Q> boundary=<str>
    int ch = 0, w = -1, h = -1, fps = -1, q = -1;
    std::string boundary = "prudyntmjpegboundary";
    auto find_kv = [&](const char *key) -> int {
      size_t pos = req.find(key);
      if (pos == std::string::npos)
        return -999999;
      pos += std::strlen(key);
      while (pos < req.size() && (req[pos] == ' ' || req[pos] == '='))
        pos++;
      size_t start = pos;
      while (pos < req.size() && isdigit(static_cast<unsigned char>(req[pos])))
        pos++;
      if (start == pos)
        return -999999;
      return std::stoi(req.substr(start, pos - start));
    };
    auto find_str = [&](const char *key) -> std::string {
      size_t pos = req.find(key);
      if (pos == std::string::npos)
        return {};
      pos += std::strlen(key);
      while (pos < req.size() && (req[pos] == ' ' || req[pos] == '='))
        pos++;
      size_t start = pos;
      while (pos < req.size() && !isspace(static_cast<unsigned char>(req[pos])))
        pos++;
      return req.substr(start, pos - start);
    };
    int v;
    v = find_kv("ch");
    if (v != -999999)
      ch = v;
    v = find_kv("w");
    if (v != -999999)
      w = v;
    v = find_kv("h");
    if (v != -999999)
      h = v;
    v = find_kv("f");
    if (v != -999999)
      fps = v;
    v = find_kv("q");
    if (v != -999999)
      q = v;
    std::string b = find_str("boundary");
    if (!b.empty())
      boundary = b;

    if (ch < 0 || ch >= NUM_VIDEO_CHANNELS || !global_jpeg[ch]) {
      const char *err = "ERR bad_ch\n";
      write(fd, err, strlen(err));
      return 0;
    }
    if (!JPEGWorker::ensure_running(ch)) {
      const char *err = "ERR jpeg_unavailable\n";
      write(fd, err, strlen(err));
      return 0;
    }

    // Limit concurrent MJPEG connections – close silently so the
    // CGI pipeline (prudyntctl → uhttpd → browser) tears down cleanly
    int current = active_mjpeg_clients_.fetch_add(1);
    if (current >= kMaxMjpegClients) {
      active_mjpeg_clients_.fetch_sub(1);
      return 0;
    }

    // Quantize w/h to multiples of 16 and cap to source size
    if (w > 0 && h > 0) {
      auto src_w = (global_jpeg[ch]->streamChn == 0) ? cfg->stream0.width
                                                     : cfg->stream1.width;
      auto src_h = (global_jpeg[ch]->streamChn == 0) ? cfg->stream0.height
                                                     : cfg->stream1.height;
      if (w > src_w)
        w = src_w;
      if (h > src_h)
        h = src_h;
      w = (w / 16) * 16;
      if (w < 16)
        w = 16;
      h = (h / 16) * 16;
      if (h < 16)
        h = 16;
    }
    if (fps > 0) {
      int max_fps = (cfg->sensor.fps > 0) ? cfg->sensor.fps : 30;
      if (fps > max_fps)
        fps = max_fps;
      if (fps < 1)
        fps = 1;
    }
    if (q >= 1 && q <= 100)
      global_jpeg[ch]->quality_override = q;

    auto *stream_cfg = global_jpeg[ch]->stream;
    int orig_fps = stream_cfg->fps;

    bool size_change =
        (w > 0 && h > 0 && (w != stream_cfg->width || h != stream_cfg->height));
    bool fps_change = (fps > 0 && fps != stream_cfg->fps);

    // Request reconfiguration and wake worker if something actually changes
    if (size_change) {
      global_jpeg[ch]->req_width = w;
      global_jpeg[ch]->req_height = h;
    }
    if (fps_change) {
      global_jpeg[ch]->req_fps = fps;
    }

    // Only trigger encoder reconfig on SIZE changes.  FPS changes are handled
    // by the JPEG worker without deinit/init (it just adjusts polling rate).
    // Reconfiguring the JPEG encoder (deinit/init) steals ISP frames from the
    // H.264 encoder and causes decode errors on the RTSP stream.
    // We still set reconfig=true for fps changes so the worker picks up the
    // new fps value, but the worker only reinit on size changes.
    if (size_change) {
      global_jpeg[ch]->reconfig = true;
      global_jpeg[ch]->request();

      // Wait briefly for reconfig to apply
      int wait_ms = 500;
      while (global_jpeg[ch]->reconfig.load() && wait_ms > 0) {
        usleep(10 * 1000);
        wait_ms -= 10;
      }
    } else if (fps_change) {
      // Set reconfig flag so worker picks up the fps change, but don't wait
      // for reinit since there's no size change
      global_jpeg[ch]->reconfig = true;
      global_jpeg[ch]->request();
    } else {
      global_jpeg[ch]->request();
    }

    // Start streaming loop: multipart MJPEG parts
    std::vector<unsigned char> img;
    std::string hdr;
    while (1) {
      img.clear();
      if (!get_snapshot_ch_local(ch, img) || img.empty()) {
        // keep the JPEG worker in "active" mode while client is connected
        global_jpeg[ch]->request();
        // if no data yet, yield a bit
        usleep(10 * 1000);
        continue;
      }
      char ph[128];
      int len = (int)img.size();
      int m = snprintf(ph, sizeof(ph),
                       "--%s\r\nContent-Type: "
                       "image/jpeg\r\nContent-Length: %d\r\n\r\n",
                       boundary.c_str(), len);
      if (!write_full(fd, ph, (size_t)m))
        break;
      if (!write_full(fd, (const char *)img.data(), (size_t)len))
        break;
      if (!write_full(fd, "\r\n", (size_t)2))
        break;
      // keep the JPEG worker at full fps while streaming
      global_jpeg[ch]->request();
      // Pace output
      int usec = 1000000 / (fps > 0 ? fps : orig_fps);

      active_mjpeg_clients_.fetch_sub(1);
      usleep(usec);
    }

    return 0;
  }

  if (starts_with(req, "EVENTS")) {
    // Stream newline-delimited JSON events until client closes
    while (running_) {
      // Build compact stats + daynight live payload for SSE consumers.
      char line[1024];
      int fps0 = cfg->stream0.stats.fps;
      int bps0 = cfg->stream0.stats.bps;
      int fps1 = cfg->stream1.stats.fps;
      int bps1 = cfg->stream1.stats.bps;
      int fps2 = cfg->stream2.stats.fps;
      int bps2 = cfg->stream2.stats.bps;
      /* Daynight telemetry sourced from daynightd files.
       * Cache reads to avoid excessive file I/O on every SSE event. */
      static time_t last_sensor_read = 0;
      static int cached_brightness = -1;
      static char cached_mode[16] = "unknown";
      static bool sensor_file_found = false;

      time_t tnow = time(NULL);
      if (tnow - last_sensor_read >= 1) {
        last_sensor_read = tnow;
        /* Read sensor JSON for ev/total_gain/thresholds */
        FILE *sf = fopen("/run/thingino/daynight_sensors", "r");
        if (sf) {
          sensor_file_found = true;
          char sbuf[2048];
          size_t sr = fread(sbuf, 1, sizeof(sbuf) - 1, sf);
          fclose(sf);
          if (sr > 0) {
            sbuf[sr] = '\0';
            /* Simple JSON value extraction — avoids linking a full parser */
            auto js_int = [&](const char *key, int def) -> int {
              char search[64];
              snprintf(search, sizeof(search), "\"%s\":", key);
              const char *p = strstr(sbuf, search);
              if (p) return atoi(p + strlen(search));
              return def;
            };
            auto js_str = [&](const char *key, const char *def, char *out, size_t outsz) {
              char search[64];
              snprintf(search, sizeof(search), "\"%s\":\"", key);
              const char *p = strstr(sbuf, search);
              if (p) {
                p += strlen(search);
                const char *q = strchr(p, '"');
                if (q && (size_t)(q - p) < outsz) {
                  memcpy(out, p, q - p);
                  out[q - p] = '\0';
                  return;
                }
              }
              strncpy(out, def, outsz - 1);
            };
            cached_brightness = js_int("brightness_percent", -1);
            js_str("mode", "unknown", cached_mode, sizeof(cached_mode));
          }
        }
        /* Fallback: read simple text files */
        if (!sensor_file_found) {
          FILE *bf = fopen("/run/thingino/daynight_brightness", "r");
          if (bf) {
            char bbuf[16];
            if (fgets(bbuf, sizeof(bbuf), bf)) cached_brightness = atoi(bbuf);
            fclose(bf);
          }
          FILE *mf = fopen("/run/thingino/daynight_mode", "r");
          if (mf) {
            char mbuf[16];
            if (fgets(mbuf, sizeof(mbuf), mf)) {
              size_t l = strlen(mbuf);
              while (l > 0 && (mbuf[l-1] == '\n' || mbuf[l-1] == ' ')) mbuf[--l] = '\0';
              strncpy(cached_mode, mbuf, sizeof(cached_mode) - 1);
              cached_mode[sizeof(cached_mode) - 1] = '\0';
            }
            fclose(mf);
          }
        }
      }

      /* Use cached values for the SSE payload.
       * Only emit fields with distinct, useful data — no duplicates. */
      int live_brightness = cached_brightness;
      const char *mode = cached_mode;
      long now = static_cast<long>(time(NULL));
      int n = snprintf(
          line, sizeof(line),
          "{\"ts\":%ld,\"time_now\":%ld,\"stats\":{\"stream0\":{\"fps\":%d,"
          "\"Bps\":%d},\"stream1\":{\"fps\":%d,\"Bps\":%d},"
          "\"stream2\":{\"fps\":%d,\"Bps\":%d}},"
          "\"daynight_brightness\":%d,"
          "\"daynight_mode\":\"%s\"}\n",
          now, now, fps0, bps0, fps1, bps1, fps2, bps2,
          live_brightness, mode);
      ssize_t w = write(fd, line, n);
      if (w <= 0)
        break; // client closed
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
  }

  if (starts_with(req, "METRICS")) {
    // Prometheus exposition format (text/plain; version 0.0.4)
    // Stream stats
    char line[256];
    // HELP/TYPE for stream fps/bps
    write(
        fd, "# HELP prudynt_stream_fps Stream frames per second (instant).\n",
        strlen(
            "# HELP prudynt_stream_fps Stream frames per second (instant).\n"));
    write(fd, "# TYPE prudynt_stream_fps gauge\n",
          strlen("# TYPE prudynt_stream_fps gauge\n"));
    snprintf(line, sizeof(line), "prudynt_stream_fps{stream=\"0\"} %d\n",
             cfg->stream0.stats.fps);
    write(fd, line, strlen(line));
    snprintf(line, sizeof(line), "prudynt_stream_fps{stream=\"1\"} %d\n",
             cfg->stream1.stats.fps);
    write(fd, line, strlen(line));
    snprintf(line, sizeof(line), "prudynt_stream_fps{stream=\"2\"} %d\n",
             cfg->stream2.stats.fps);
    write(fd, line, strlen(line));

    write(
        fd, "# HELP prudynt_stream_Bps Stream bytes per second (instant).\n",
        strlen(
            "# HELP prudynt_stream_Bps Stream bytes per second (instant).\n"));
    write(fd, "# TYPE prudynt_stream_Bps gauge\n",
          strlen("# TYPE prudynt_stream_Bps gauge\n"));
    snprintf(line, sizeof(line), "prudynt_stream_Bps{stream=\"0\"} %u\n",
             (unsigned)cfg->stream0.stats.bps);
    write(fd, line, strlen(line));
    snprintf(line, sizeof(line), "prudynt_stream_Bps{stream=\"1\"} %u\n",
             (unsigned)cfg->stream1.stats.bps);
    write(fd, line, strlen(line));
    snprintf(line, sizeof(line), "prudynt_stream_Bps{stream=\"2\"} %u\n",
             (unsigned)cfg->stream2.stats.bps);
    write(fd, line, strlen(line));

    // Day/Night live — sourced from daynightd files
    int live_brightness_pct = -1;
    int live_ev_val = -1;
    int live_gb_val = -1;
    int live_gr_val = -1;
    /* Read simple text files for basic metrics */
    FILE *prom_bf = fopen("/run/thingino/daynight_brightness", "r");
    if (prom_bf) {
      char bbuf[16];
      if (fgets(bbuf, sizeof(bbuf), prom_bf)) live_brightness_pct = atoi(bbuf);
      fclose(prom_bf);
    }
    /* Read sensor JSON for ev_log2 (prometheus ev metric) */
    FILE *prom_sf = fopen("/run/thingino/daynight_sensors", "r");
    if (prom_sf) {
      char sbuf[2048];
      size_t sr = fread(sbuf, 1, sizeof(sbuf) - 1, prom_sf);
      fclose(prom_sf);
      if (sr > 0) {
        sbuf[sr] = '\0';
        auto prom_js_int = [&](const char *key, int def) -> int {
          char search[64];
          snprintf(search, sizeof(search), "\"%s\":", key);
          const char *p = strstr(sbuf, search);
          if (p) return atoi(p + strlen(search));
          return def;
        };
        live_ev_val = prom_js_int("ev_log2", -1);
        live_gb_val = prom_js_int("wb_bgain", -1);
        live_gr_val = prom_js_int("wb_rgain", -1);
      }
    }
    write(fd,
          "# HELP prudynt_daynight_brightness_percent Day/Night brightness "
          "percent (0..100).\n",
          strlen("# HELP prudynt_daynight_brightness_percent Day/Night "
                 "brightness percent (0..100).\n"));
    write(fd, "# TYPE prudynt_daynight_brightness_percent gauge\n",
          strlen("# TYPE prudynt_daynight_brightness_percent gauge\n"));
    snprintf(line, sizeof(line), "prudynt_daynight_brightness_percent %d\n",
             live_brightness_pct);
    write(fd, line, strlen(line));

    write(fd,
          "# HELP prudynt_daynight_ev ISP exposure value (platform units).\n",
          strlen("# HELP prudynt_daynight_ev ISP exposure value (platform "
                 "units).\n"));
    write(fd, "# TYPE prudynt_daynight_ev gauge\n",
          strlen("# TYPE prudynt_daynight_ev gauge\n"));
    snprintf(line, sizeof(line), "prudynt_daynight_ev %d\n", live_ev_val);
    write(fd, line, strlen(line));

    write(fd, "# HELP prudynt_daynight_gb AWB blue/green gain (b/g).\n",
          strlen("# HELP prudynt_daynight_gb AWB blue/green gain (b/g).\n"));
    write(fd, "# TYPE prudynt_daynight_gb gauge\n",
          strlen("# TYPE prudynt_daynight_gb gauge\n"));
    snprintf(line, sizeof(line), "prudynt_daynight_gb %d\n", live_gb_val);
    write(fd, line, strlen(line));

    write(fd, "# HELP prudynt_daynight_gr AWB red/green gain (r/g).\n",
          strlen("# HELP prudynt_daynight_gr AWB red/green gain (r/g).\n"));
    write(fd, "# TYPE prudynt_daynight_gr gauge\n",
          strlen("# TYPE prudynt_daynight_gr gauge\n"));
    snprintf(line, sizeof(line), "prudynt_daynight_gr %d\n", live_gr_val);
    write(fd, line, strlen(line));

    write(fd,
          "# HELP prudynt_image_sinter_strength Current ISP sinter denoise "
          "strength.\n",
          strlen("# HELP prudynt_image_sinter_strength Current ISP sinter "
                 "denoise strength.\n"));
    write(fd, "# TYPE prudynt_image_sinter_strength gauge\n",
          strlen("# TYPE prudynt_image_sinter_strength gauge\n"));
    snprintf(line, sizeof(line), "prudynt_image_sinter_strength %d\n",
             cfg->image.sinter_strength);
    write(fd, line, strlen(line));

    write(fd,
          "# HELP prudynt_image_temper_strength Current ISP temper denoise "
          "strength.\n",
          strlen("# HELP prudynt_image_temper_strength Current ISP temper "
                 "denoise strength.\n"));
    write(fd, "# TYPE prudynt_image_temper_strength gauge\n",
          strlen("# TYPE prudynt_image_temper_strength gauge\n"));
    snprintf(line, sizeof(line), "prudynt_image_temper_strength %d\n",
             cfg->image.temper_strength);
    write(fd, line, strlen(line));

    // Uptime seconds
    struct sysinfo sinfo{};
    if (sysinfo(&sinfo) == 0) {
      snprintf(line, sizeof(line), "prudynt_uptime_seconds %lu\n",
               (unsigned long)sinfo.uptime);
      write(fd, line, strlen(line));
    }

    // RTSP active client sessions
    snprintf(line, sizeof(line), "prudynt_rtsp_clients %d\n",
             (int)global_rtsp_clients.load());
    write(fd, line, strlen(line));

    // Build info as a const gauge with labels
    write(fd, "# HELP prudynt_build_info Build and commit info.\n",
          strlen("# HELP prudynt_build_info Build and commit info.\n"));
    write(fd, "# TYPE prudynt_build_info gauge\n",
          strlen("# TYPE prudynt_build_info gauge\n"));
    const char *platform =
#if defined(PLATFORM_T31)
        "T31";
#elif defined(PLATFORM_T23)
        "T23";
#elif defined(PLATFORM_T21)
        "T21";
#elif defined(PLATFORM_T30)
        "T30";
#elif defined(PLATFORM_T40)
        "T40";
#elif defined(PLATFORM_T41)
        "T41";
#else
        "unknown";
#endif
    snprintf(line, sizeof(line),
             "prudynt_build_info{commit=\"%s\",platform=\"%s\"} 1\n",
             BUILD_COMMIT, platform);
    write(fd, line, strlen(line));

    /* Day/night state from daynightd file */
    const char *m = "unknown";
    char mode_buf[16];
    FILE *mode_fp = fopen("/run/thingino/daynight_mode", "r");
    if (mode_fp) {
      if (fgets(mode_buf, sizeof(mode_buf), mode_fp)) {
        size_t l = strlen(mode_buf);
        while (l > 0 && (mode_buf[l-1] == '\n' || mode_buf[l-1] == ' '))
          mode_buf[--l] = '\0';
        static char mode_static[16];
        strncpy(mode_static, mode_buf, sizeof(mode_static)-1);
        mode_static[sizeof(mode_static)-1] = '\0';
        m = mode_static;
      }
      fclose(mode_fp);
    }
    int is_day = (strcmp(m, "day") == 0);
    int is_night = (strcmp(m, "night") == 0);
    write(fd,
          "# HELP prudynt_daynight_state Day/Night state as one-hot time "
          "series.\n",
          strlen("# HELP prudynt_daynight_state Day/Night state as one-hot "
                 "time series.\n"));
    write(fd, "# TYPE prudynt_daynight_state gauge\n",
          strlen("# TYPE prudynt_daynight_state gauge\n"));
    snprintf(line, sizeof(line), "prudynt_daynight_state{state=\"day\"} %d\n",
             is_day);
    write(fd, line, strlen(line));
    snprintf(line, sizeof(line), "prudynt_daynight_state{state=\"night\"} %d\n",
             is_night);
    write(fd, line, strlen(line));
    snprintf(line, sizeof(line),
             "prudynt_daynight_state{state=\"unknown\"} %d\n",
             (!is_day && !is_night));
    write(fd, line, strlen(line));

    return 0;
  }

  const char *err = "ERR unknown_command\n";
  write(fd, err, strlen(err));
  return 0;
}
