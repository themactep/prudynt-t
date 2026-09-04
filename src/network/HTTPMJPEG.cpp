#include "network/HTTPMJPEG.hpp"

#include "config/Config.hpp"
#include "video/JPEGWorker.hpp"
#include "config/JsonAPI.hpp"
#include "util/Logger.hpp"
#include "stream/globals.hpp"

#include <json_config.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include <chrono>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std::chrono;

namespace {

std::string base64_decode(const std::string &encoded);

bool is_privacy_active_for_jpeg(int ch) {
  if (ch < 0 || ch >= NUM_JPEG_CHANNELS) {
    return false;
  }
  auto jpeg = global_jpeg[ch];
  if (!jpeg) {
    return false;
  }
  int source_ch = jpeg->streamChn;
  if (source_ch < 0 || source_ch >= NUM_VIDEO_CHANNELS) {
    return false;
  }
  auto video = global_video[source_ch];
  if (!video) {
    return false;
  }
  return video->privacy_requested.load(std::memory_order_acquire);
}

const std::vector<unsigned char> &privacy_placeholder_jpeg() {
  static const std::vector<unsigned char> jpeg = []() {
    const std::string decoded = base64_decode(
        "/9j/4AAQSkZJRgABAQAAAQABAAD/"
        "2wBDAAYEBQYFBAYGBQYHBwYIChAKCgkJChQODwwQFxQYGBcUFhYaHSUfGhsjHBYWICwgIy"
        "YnKSopGR8tMC0oMCUoKSj/"
        "2wBDAQcHBwoIChMKChMoGhYaKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKC"
        "goKCgoKCgoKCgoKCgoKCj/wAARCAAJABADASIAAhEBAxEB/"
        "8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/"
        "8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fA"
        "kM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZ"
        "WmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5us"
        "LDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/"
        "8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/"
        "8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvA"
        "VYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldY"
        "WVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uL"
        "m6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/"
        "9oADAMBAAIRAxEAPwD5UooooA//2Q==");
    return std::vector<unsigned char>(decoded.begin(), decoded.end());
  }();
  return jpeg;
}

// Simple base64 decoder for HTTP Basic Authentication
std::string base64_decode(const std::string &encoded) {
  static const std::string base64_chars =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string decoded;
  std::vector<int> T(256, -1);
  for (int i = 0; i < 64; i++)
    T[base64_chars[i]] = i;

  int val = 0, valb = -8;
  for (unsigned char c : encoded) {
    if (T[c] == -1)
      break;
    val = (val << 6) + T[c];
    valb += 6;
    if (valb >= 0) {
      decoded.push_back(char((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return decoded;
}

// Helper: write all bytes (handles EINTR/partial)
bool write_full(int fd, const void *data, size_t len) {
  const char *p = static_cast<const char *>(data);
  while (len > 0) {
    ssize_t n = ::write(fd, p, len);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (n == 0)
      return false;
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

// Write in small chunks, paced across the target duration to avoid bursts
bool write_chunked_paced(int fd, const unsigned char *data, size_t len,
                         size_t chunk_sz, milliseconds total) {
  auto start = steady_clock::now();
  size_t sent = 0;
  while (sent < len) {
    size_t to_send = std::min(chunk_sz, len - sent);
    ssize_t n = ::send(fd, data + sent, to_send, 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (n == 0)
      return false;
    sent += static_cast<size_t>(n);

    // Target schedule: proportion of total duration based on bytes sent
    auto elapsed = steady_clock::now() - start;
    auto target_us = static_cast<uint64_t>(sent) *
                     duration_cast<microseconds>(total).count() /
                     static_cast<uint64_t>(len);
    auto elapsed_us =
        static_cast<uint64_t>(duration_cast<microseconds>(elapsed).count());
    if (target_us > elapsed_us) {
      auto sleep_us = target_us - elapsed_us;
      if (sleep_us > 50) { // avoid oversleeping tiny fragments
        usleep(static_cast<useconds_t>(sleep_us));
      }
    }
  }
  return true;
}

// Local snapshot helper: copy the latest JPEG frame (already clean)
bool get_snapshot_ch_local_http(int ch, std::vector<unsigned char> &image) {
  std::unique_lock lck(mutex_main);
  if (ch < 0 || ch >= NUM_JPEG_CHANNELS)
    return false;
  auto s = global_jpeg[ch];
  if (!s)
    return false;
  auto &buf = s->snapshot_buf;
  if (buf.empty())
    return false;
  image = buf; // snapshot_buf is built as contiguous JPEG; no prepadding
  return true;
}

std::string get_param(const std::string &qs, const std::string &name) {
  size_t p = 0;
  while (p < qs.size()) {
    size_t k = qs.find(name, p);
    if (k == std::string::npos)
      break;
    if (k == 0 || qs[k - 1] == '&') {
      size_t eq = k + name.size();
      if (eq < qs.size() && qs[eq] == '=') {
        size_t v0 = eq + 1;
        size_t amp = qs.find('&', v0);
        return qs.substr(v0, amp == std::string::npos ? std::string::npos
                                                      : amp - v0);
      }
    }
    p = k + 1;
  }
  return "";
}

} // namespace

HTTPMJPEG::HTTPMJPEG() = default;
HTTPMJPEG::~HTTPMJPEG() {
  stop();
}

void HTTPMJPEG::start(int port, bool enable_mjpeg, bool enable_api,
                      bool auth_required, const char *username,
                      const char *password) {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true))
    return; // already running
  mjpeg_enabled_ = enable_mjpeg;
  api_enabled_ = enable_api;
  auth_required_ = auth_required;
  username_ = username;
  password_ = password;
  th_ = std::thread(&HTTPMJPEG::server_loop, this, port);
}

void HTTPMJPEG::stop() {
  if (!running_.load())
    return;
  running_.store(false);
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (th_.joinable())
    th_.join();
}

void HTTPMJPEG::server_loop(int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    LOG_ERROR("HTTPMJPEG: socket() failed: " << strerror(errno));
    running_.store(false);
    return;
  }
  listen_fd_ = fd;
  int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    LOG_ERROR("HTTPMJPEG: bind(" << port << ") failed: " << strerror(errno));
    ::close(fd);
    listen_fd_ = -1;
    running_.store(false);
    return;
  }
  if (::listen(fd, 8) < 0) {
    LOG_ERROR("HTTPMJPEG: listen() failed: " << strerror(errno));
    ::close(fd);
    listen_fd_ = -1;
    running_.store(false);
    return;
  }
  LOG_INFO("HTTPMJPEG: listening on port "
           << port << " (/mjpg?ch=0.." << (NUM_JPEG_CHANNELS - 1)
           << "&f=&q=&w=&h=)" << (api_enabled_ ? " and /api/v1/config" : ""));

  while (running_.load()) {
    sockaddr_in cli{};
    socklen_t clilen = sizeof(cli);
    int cfd = ::accept(fd, reinterpret_cast<sockaddr *>(&cli), &clilen);
    if (cfd < 0) {
      if (errno == EINTR)
        continue;
      if (!running_.load())
        break;
      // accept interrupted by shutdown or error; continue
      continue;
    }
    std::thread(&HTTPMJPEG::handle_client, this, cfd).detach();
  }
}

void HTTPMJPEG::handle_client(int cfd) {
  // Optimize socket for low-latency, steady delivery
  int one = 1;
  ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  int tos = 0x10; // IPTOS_LOWDELAY
  ::setsockopt(cfd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
   // 64 KB send buffer: JPEG frames at 2304x1296 are ~100 KB, so the old
   // 16 KB buffer caused frequent stalls waiting for client reads on WiFi.
   int sndbuf = 64 * 1024;
   ::setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

  // Read request headers
  std::string req;
  char buf[2048];
  while (req.find("\r\n\r\n") == std::string::npos) {
    ssize_t r = ::recv(cfd, buf, sizeof(buf), 0);
    if (r <= 0)
      break;
    req.append(buf, buf + r);
    if (req.size() > 8192)
      break; // avoid header abuse
  }

  // Parse request line
  std::string method;
  int ch = 0, q = -1, fps = -1, w = -1, h = -1;
   int chunk_sz = 4096;       // default paced chunk size (bytes)
  int sndbuf_override = -1; // bytes; <=0 means use default
  int tos_override = -1;    // IP TOS override; <0 means default
  std::string boundary = "prudyntmjpegboundary";
  std::string path = "/";
  std::string qs;
  size_t l0 = req.find('\n');
  if (l0 != std::string::npos) {
    std::string line = req.substr(0, l0);
    // Expect: GET /mjpg?... HTTP/1.1
    size_t sp1 = line.find(' ');
    size_t sp2 = line.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
    if (sp1 != std::string::npos && sp2 != std::string::npos) {
      method = line.substr(0, sp1);
      std::string url = line.substr(sp1 + 1, sp2 - (sp1 + 1));
      size_t qm = url.find('?');
      if (qm == std::string::npos) {
        path = url;
      } else {
        path = url.substr(0, qm);
        qs = url.substr(qm + 1);
      }
    }
  }

  auto send_response = [&](int code, const char *ctype,
                           const std::string &payload) {
    char hdr[512];
    int n = snprintf(
        hdr, sizeof(hdr),
        "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: "
        "%zu\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        code,
        (code == 200 ? "OK" : (code == 404 ? "Not Found" : "Bad Request")),
        ctype, payload.size());
    write_full(cfd, hdr, static_cast<size_t>(n));
    if (!payload.empty())
      write_full(cfd, payload.data(), payload.size());
  };

  // Extract X-API-Key header for config endpoint auth
  auto extract_header = [&](const std::string &name) -> std::string {
    std::string lower = name;
    for (auto &c : lower) c = std::tolower(static_cast<unsigned char>(c));
    lower += ':';
    size_t pos = 0;
    while (pos < req.size()) {
      size_t eol = req.find('\r', pos);
      if (eol == std::string::npos) eol = req.find('\n', pos);
      if (eol == std::string::npos) break;
      std::string line = req.substr(pos, eol - pos);
      if (line.size() > lower.size()) {
        std::string key = line.substr(0, lower.size());
        for (auto &c : key) c = std::tolower(static_cast<unsigned char>(c));
        if (key == lower) {
          std::string val = line.substr(lower.size());
          size_t a = 0;
          while (a < val.size() && val[a] == ' ') ++a;
          return val.substr(a);
        }
      }
      pos = eol;
      while (pos < req.size() && (req[pos] == '\r' || req[pos] == '\n')) ++pos;
    }
    return {};
  };

  auto verify_api_key = [&](const std::string &provided) -> bool {
    if (provided.empty()) return false;
    std::ifstream kf("/etc/thingino-api.key");
    if (!kf.is_open()) return false;
    std::string stored;
    std::getline(kf, stored);
    while (!stored.empty() && (stored.back() == '\n' || stored.back() == '\r' || stored.back() == ' '))
      stored.pop_back();
    return provided == stored;
  };

  // Verify API key for config endpoints (skip for loopback)
  std::string api_key = extract_header("X-API-Key");
  if (api_key.empty()) {
    // Fall back to ?token= query parameter
    auto find_qs = [&](const std::string &key) -> std::string {
      std::string search = key + '=';
      size_t p = qs.find(search);
      if (p == std::string::npos) return {};
      p += search.size();
      size_t e = qs.find('&', p);
      return qs.substr(p, e == std::string::npos ? std::string::npos : e - p);
    };
    api_key = find_qs("token");
  }
  bool api_authenticated = verify_api_key(api_key);

  // Handle CORS preflight
  if (method == "OPTIONS") {
    const char *cors = "HTTP/1.0 204 No Content\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
      "Access-Control-Allow-Headers: Content-Type, X-API-Key\r\n"
      "Access-Control-Max-Age: 86400\r\n"
      "Connection: close\r\n\r\n";
    write_full(cfd, cors, strlen(cors));
    ::close(cfd);
    return;
  }

  // Handle SEI OSD metadata endpoint (no auth required --- read-only, non-sensitive)
  if (api_enabled_ && path == "/api/v1/osd-sei") {
    if (method != "GET") {
      send_response(405, "text/plain", "method not allowed\n");
      ::close(cfd);
      return;
    }
    std::string sei_json;
    for (int ch = 0; ch < NUM_VIDEO_CHANNELS && sei_json.empty(); ++ch) {
      auto vs = global_video[ch];
      if (vs && vs->imp_encoder && vs->imp_encoder->osd) {
        sei_json = vs->imp_encoder->osd->getSEIJson();
      }
    }
    if (sei_json.empty())
      sei_json = "{}";
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n", sei_json.size());
    write_full(cfd, hdr, static_cast<size_t>(n));
    write_full(cfd, sei_json.data(), sei_json.size());
    ::close(cfd);
    return;
  }

  // Handle REST GET for config subtrees
  const char *cfg_prefix = "/api/v1/config/";
  if (api_enabled_ && path.rfind(cfg_prefix, 0) == 0 && method == "GET") {
    if (!api_authenticated) {
      send_response(401, "application/json", "{\"error\":\"Authentication required\"}\n");
      ::close(cfd);
      return;
    }
    std::string json_path = path.substr(strlen(cfg_prefix));
    // Replace / with . for nested paths: e.g. osd/sei -> osd.sei
    for (auto &c : json_path)
      if (c == '/') c = '.';

    JsonValue *sub = get_nested_item(cfg->jsonConfig, json_path.c_str());
    char *js = json_to_string(sub ? sub : create_json_value(JSON_OBJECT), 0);
    std::string body = js ? js : "{}";
    free(js);

    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n", body.size());
    write_full(cfd, hdr, static_cast<size_t>(n));
    write_full(cfd, body.data(), body.size());
    ::close(cfd);
    return;
  }

  // Handle JSON API on the same port (before Basic Auth check)
  if (api_enabled_ && path == "/api/v1/config") {
    if (!api_authenticated) {
      send_response(401, "application/json", "{\"error\":\"Authentication required\"}\n");
      ::close(cfd);
      return;
    }
    // Require POST
    if (method != "POST") {
      send_response(405, "text/plain", "method not allowed\n");
      ::close(cfd);
      return;
    }

    // Parse Content-Length
    size_t hdr_end = req.find("\r\n\r\n");
    int content_length = 0;
    size_t pos = req.find('\n');
    auto trim = [](const std::string &s) {
      size_t a = 0, b = s.size();
      while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
      while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
      return s.substr(a, b - a);
    };
    while (pos != std::string::npos && pos < hdr_end) {
      size_t next = req.find('\n', pos + 1);
      size_t line_start = (pos == std::string::npos) ? 0 : pos + 1;
      std::string line = trim(
          req.substr(line_start, (next == std::string::npos ? hdr_end : next) -
                                     line_start));
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
      }
    }

    std::string body = req.substr(hdr_end + 4);
    while (content_length > 0 &&
           static_cast<int>(body.size()) < content_length) {
      ssize_t r = ::recv(cfd, buf, sizeof(buf), 0);
      if (r <= 0)
        break;
      body.append(buf, buf + r);
    }

    std::string resp_json;
    bool ok = JsonAPI::process_json(body, resp_json);
    if (!ok) {
      send_response(400, "application/json",
                    "{\"error\":\"invalid_request\"}\n");
    } else {
      // Persist in-memory changes to disk so they survive reboots
      save_config(cfg->filePath.c_str(), cfg->jsonConfig);

      // Also update /etc/onvif.json with the new RTSP credentials
      // so ONVIF stays in sync
      JsonValue *rtsp_pw = get_nested_item(cfg->jsonConfig, "rtsp.password");
      JsonValue *rtsp_un = get_nested_item(cfg->jsonConfig, "rtsp.username");
      if (rtsp_pw || rtsp_un) {
        JsonValue *onvif_root = load_config("/etc/onvif.json");
        if (onvif_root) {
          if (rtsp_pw && rtsp_pw->type == JSON_STRING && rtsp_pw->value.string)
            set_nested_item(onvif_root, "server.password", rtsp_pw->value.string);
          if (rtsp_un && rtsp_un->type == JSON_STRING && rtsp_un->value.string)
            set_nested_item(onvif_root, "server.username", rtsp_un->value.string);
          save_config("/etc/onvif.json", onvif_root);
          free_json_value(onvif_root);
        }
      }

      send_response(200, "application/json", resp_json);
    }
    ::close(cfd);
    return;
  }

  if (!mjpeg_enabled_ || (path != "/mjpg" && path != "/x/mjpg" &&
                          path != "/mjpeg" && path != "/x/mjpeg")) {
    send_response(404, "text/plain", "not found\n");
    ::close(cfd);
    return;
  }

  // Extract params
  auto p = get_param(qs, "ch");
  if (!p.empty())
    ch = std::clamp(atoi(p.c_str()), 0, NUM_JPEG_CHANNELS - 1);
  p = get_param(qs, "f");
  if (!p.empty())
    fps = std::max(1, std::min(30, atoi(p.c_str())));
  p = get_param(qs, "q");
  if (!p.empty())
    q = std::max(1, std::min(100, atoi(p.c_str())));
  p = get_param(qs, "w");
  if (!p.empty())
    w = std::max(1, atoi(p.c_str()));
  p = get_param(qs, "h");
  if (!p.empty())
    h = std::max(1, atoi(p.c_str()));
  p = get_param(qs, "boundary");
  if (!p.empty())
    boundary = p;

  if (ch < 0 || ch >= NUM_JPEG_CHANNELS || !global_jpeg[ch]) {
    const char *resp = "HTTP/1.0 400 Bad Request\r\nContent-Type: "
                       "text/plain\r\nConnection: close\r\n\r\nbad channel\n";
    (void)write_full(cfd, resp, strlen(resp));
    ::close(cfd);
    return;
  }
  if (!JPEGWorker::ensure_running(ch)) {
    const char *resp =
        "HTTP/1.0 503 Service Unavailable\r\nContent-Type: "
        "text/plain\r\nConnection: close\r\n\r\njpeg unavailable\n";
    (void)write_full(cfd, resp, strlen(resp));
    ::close(cfd);
    return;
  }

  auto *stream_cfg = global_jpeg[ch]->stream;

  // Quantize w/h to multiples of 16 and cap to source size. If only one
  // dimension is given, derive the other from the source aspect ratio so
  // the encoder never stretches the image.
  if (w > 0 || h > 0) {
    auto src_w = (global_jpeg[ch]->streamChn == 0) ? cfg->stream0.width
                                                   : cfg->stream1.width;
    auto src_h = (global_jpeg[ch]->streamChn == 0) ? cfg->stream0.height
                                                   : cfg->stream1.height;
    if (src_w > 0 && src_h > 0) {
      if (w > 0 && h <= 0)
        h = (int)((long long)w * src_h / src_w);
      else if (h > 0 && w <= 0)
        w = (int)((long long)h * src_w / src_h);
    }
    // Round to the nearest multiple of 16 (not always up) so the derived
    // dimension stays closest to the source aspect ratio.
    w = (w + 8) & ~15;
    h = (h + 8) & ~15;
    // Cap after rounding so a non-16-aligned source (e.g. 1080p) is not
    // exceeded.
    if (w > src_w)
      w = src_w;
    if (h > src_h)
      h = src_h;
  }

  int orig_fps = stream_cfg->fps;

  bool size_change =
      (w > 0 && h > 0 && (w != stream_cfg->width || h != stream_cfg->height));
  bool fps_change = (fps > 0 && fps != stream_cfg->fps);
  bool quality_change =
      (q >= 1 && q <= 100 && q != stream_cfg->jpeg_quality);

  if (size_change) {
    global_jpeg[ch]->req_width = w;
    global_jpeg[ch]->req_height = h;
  }
  if (fps_change) {
    global_jpeg[ch]->req_fps = fps;
  }
  if (quality_change) {
    global_jpeg[ch]->req_quality = q;
  }

  // Optional tuning: chunk size, sndbuf, tos
  p = get_param(qs, "cs");
  if (!p.empty()) {
    int cs = atoi(p.c_str());
    chunk_sz = std::max(256, std::min(4096, cs));
  }
  p = get_param(qs, "sndbuf");
  if (!p.empty()) {
    int sb = atoi(p.c_str());
    sndbuf_override = std::max(4 * 1024, std::min(128 * 1024, sb));
  }
  p = get_param(qs, "tos");
  if (!p.empty()) {
    tos_override = static_cast<int>(strtol(p.c_str(), nullptr, 0));
    if (tos_override < 0 || tos_override > 0xff)
      tos_override = -1;
  }

  bool needs_reconfig = size_change || fps_change || quality_change;
  if (needs_reconfig) {
    global_jpeg[ch]->reconfig = true;
    global_jpeg[ch]->request();

    // Wait briefly for reconfig to apply
    int wait_ms = 500;
    while (global_jpeg[ch]->reconfig.load() && wait_ms > 0) {
      usleep(10 * 1000);
      wait_ms -= 10;
    }
  } else {
    global_jpeg[ch]->request();
  }

  // Apply optional socket overrides from query
  if (tos_override >= 0) {
    ::setsockopt(cfd, IPPROTO_IP, IP_TOS, &tos_override, sizeof(tos_override));
  }
  if (sndbuf_override > 0) {
    ::setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf_override,
                 sizeof(sndbuf_override));
  }

  // Headers
  std::string hdr;
  hdr.reserve(256);
  hdr += "HTTP/1.0 200 OK\r\n";
  hdr += "Content-Type: multipart/x-mixed-replace; boundary=";
  hdr += boundary;
  hdr +=
      "\r\nCache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n";
  hdr += "Pragma: no-cache\r\n";
  hdr += "X-Chunk-Size: ";
  hdr += std::to_string(chunk_sz);
  hdr += "\r\n";
  hdr += "Connection: close\r\n\r\n";
  if (!write_full(cfd, hdr.data(), hdr.size())) {
    ::close(cfd);
    return;
  }

  // Stream loop: send only on new frames to keep cadence steady
  std::vector<unsigned char> img, last_img;
  uint32_t last_seq = global_jpeg[ch]->frame_seq.load();
  // Determine pacing from requested/original fps (fallback to 10)
  int target_fps = (fps > 0 ? fps : (orig_fps > 0 ? orig_fps : 10));
  auto tick = milliseconds(std::max(10, 1000 / target_fps));
  auto t_next = steady_clock::now() + tick; // first send in ~1 tick
  // Set a conservative send timeout to avoid blocking indefinitely on a stalled
  // link
  {
    int snd_timeout_ms = std::max(200, (1000 / std::max(1, target_fps)) * 2);
    timeval sto;
    sto.tv_sec = snd_timeout_ms / 1000;
    sto.tv_usec = (snd_timeout_ms % 1000) * 1000;
    ::setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &sto, sizeof(sto));
  }

  while (true) {
    // Schedule the next deadline one tick ahead; avoid catch-up bursts
    t_next += tick;
    auto now = steady_clock::now();
    if (now > t_next)
      t_next = now + tick; // if we slipped, reset phase

    // Try to wait for a fresh frame; if none arrives by the deadline, reuse
    // last
    bool have_new = false;
    for (int i = 0; i < 200; ++i) { // up to ~1s
      uint32_t seq = global_jpeg[ch]->frame_seq.load();
      if (seq != last_seq) {
        last_seq = seq;
        have_new = true;
        break;
      }
      global_jpeg[ch]->request();
      auto now_check = steady_clock::now();
      if (now_check >= t_next)
        break;
      std::this_thread::sleep_for(milliseconds(5));
    }

    img.clear();
    if (is_privacy_active_for_jpeg(ch)) {
      img = privacy_placeholder_jpeg();
    } else if (have_new) {
      if (!get_snapshot_ch_local_http(ch, img) || img.empty()) {
        // reuse previous frame if capture failed
        img = last_img;
      }
    } else {
      // keep cadence with last successfully sent frame
      img = last_img;
    }

    if (img.empty()) {
      std::this_thread::sleep_for(milliseconds(5));
      continue;
    }

    last_img = img;

    char part_hdr[256];
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    int header_len =
        snprintf(part_hdr, sizeof(part_hdr),
                 "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: "
                 "%zu\r\nX-Timestamp: %ld.%06ld\r\n\r\n",
                 boundary.c_str(), img.size(), static_cast<long>(tv.tv_sec),
                 static_cast<long>(tv.tv_usec));

    if (header_len < 0 || header_len >= static_cast<int>(sizeof(part_hdr)) ||
        !write_full(cfd, part_hdr, static_cast<size_t>(header_len))) {
      break;
    }
    if (!write_chunked_paced(cfd, img.data(), img.size(),
                             static_cast<size_t>(chunk_sz), tick))
      break;
    if (!write_full(cfd, "\r\n", 2))
      break;

    // Inform producer we're still subscribed
    global_jpeg[ch]->request();
  }

  ::close(cfd);
}
