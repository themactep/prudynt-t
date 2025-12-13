#include "HTTPMJPEG.hpp"

#include "Logger.hpp"
#include "globals.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
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
bool write_chunked_paced(int fd, const unsigned char *data, size_t len, size_t chunk_sz, milliseconds total) {
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
    auto target_us = static_cast<uint64_t>(sent) * duration_cast<microseconds>(total).count() / static_cast<uint64_t>(len);
    auto elapsed_us = static_cast<uint64_t>(duration_cast<microseconds>(elapsed).count());
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
bool get_snapshot_ch_local(int ch, std::vector<unsigned char> &image) {
  std::unique_lock lck(mutex_main);
  if (ch < 0 || ch >= NUM_VIDEO_CHANNELS)
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
        return qs.substr(v0, amp == std::string::npos ? std::string::npos : amp - v0);
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

void HTTPMJPEG::start(int port) {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true))
    return; // already running
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
  LOG_INFO("HTTPMJPEG: listening on port " << port << " (/mjpg?ch=0|1&f=&q=&w=&h=)");

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
  int sndbuf = 16 * 1024; // smaller to avoid kernel bursts
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
  int ch = 0, q = -1, fps = -1, w = -1, h = -1;
  int chunk_sz = 1024;      // default paced chunk size (bytes)
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

  if (path != "/mjpg" && path != "/x/mjpg" && path != "/mjpeg" && path != "/x/mjpeg") {
    const char *resp = "HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nnot found\n";
    (void)write_full(cfd, resp, strlen(resp));
    ::close(cfd);
    return;
  }

  // Extract params
  auto p = get_param(qs, "ch");
  if (!p.empty())
    ch = std::max(0, std::min(1, atoi(p.c_str())));
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

  if (ch < 0 || ch >= NUM_VIDEO_CHANNELS || !global_jpeg[ch]) {
    const char *resp = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nbad channel\n";
    (void)write_full(cfd, resp, strlen(resp));
    ::close(cfd);
    return;
  }

  // Quantize w/h to multiples of 16 and cap to source size
  if (w > 0 && h > 0) {
    auto src_w = (global_jpeg[ch]->streamChn == 0) ? cfg->stream0.width : cfg->stream1.width;
    auto src_h = (global_jpeg[ch]->streamChn == 0) ? cfg->stream0.height : cfg->stream1.height;
    if (w > src_w)
      w = src_w;
    if (h > src_h)
      h = src_h;
    w = (w + 15) & ~15;
    h = (h + 15) & ~15;
  }

  // Apply quality override and request reconfig
  if (q >= 1 && q <= 100)
    global_jpeg[ch]->quality_override = q;

  int orig_w = global_jpeg[ch]->stream->width;
  int orig_h = global_jpeg[ch]->stream->height;
  int orig_fps = global_jpeg[ch]->stream->fps;

  if (w > 0 && h > 0) {
    global_jpeg[ch]->req_width = w;
    global_jpeg[ch]->req_height = h;
  }
  if (fps > 0) {
    global_jpeg[ch]->req_fps = fps;
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

  global_jpeg[ch]->reconfig = true;
  global_jpeg[ch]->request();

  // Wait briefly for reconfig to apply
  int wait_ms = 500;
  while (global_jpeg[ch]->reconfig.load() && wait_ms > 0) {
    usleep(10 * 1000);
    wait_ms -= 10;
  }

  // Apply optional socket overrides from query
  if (tos_override >= 0) {
    ::setsockopt(cfd, IPPROTO_IP, IP_TOS, &tos_override, sizeof(tos_override));
  }
  if (sndbuf_override > 0) {
    ::setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf_override, sizeof(sndbuf_override));
  }

  // Headers
  std::string hdr;
  hdr.reserve(256);
  hdr += "HTTP/1.0 200 OK\r\n";
  hdr += "Content-Type: multipart/x-mixed-replace; boundary=";
  hdr += boundary;
  hdr += "\r\nCache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n";
  hdr += "Pragma: no-cache\r\n";
  hdr += "Connection: close\r\n\r\n";
  if (!write_full(cfd, hdr.data(), hdr.size())) {
    ::close(cfd);
    return;
  }

  // Stream loop: send only on new frames to keep cadence steady
  std::vector<unsigned char> img;
  uint32_t last_seq = global_jpeg[ch]->frame_seq.load();
  // Determine pacing from requested/original fps (fallback to 10)
  int target_fps = (fps > 0 ? fps : (orig_fps > 0 ? orig_fps : 10));
  auto tick = milliseconds(std::max(10, 1000 / target_fps));
  auto t_next = steady_clock::now() + tick; // first send in ~1 tick
  // Set a conservative send timeout to avoid blocking indefinitely on a stalled link
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

    // Try to wait for a fresh frame; if none arrives by the deadline, reuse last
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

    if (!have_new) {
      global_jpeg[ch]->request();
    }

    if (!get_snapshot_ch_local(ch, img) || img.empty()) {
      std::this_thread::sleep_for(milliseconds(5));
      continue;
    }

    std::string part;
    part.reserve(128);
    part += "--";
    part += boundary;
    part += "\r\nContent-Type: image/jpeg\r\nContent-Length: ";
    part += std::to_string(img.size());
    part += "\r\n\r\n";

    if (!write_full(cfd, part.data(), part.size()))
      break;

    auto remaining = std::chrono::duration_cast<milliseconds>(t_next - steady_clock::now());
    milliseconds send_budget = remaining.count() > 0 ? remaining : milliseconds(0);
    milliseconds pacing = std::max(send_budget, milliseconds(10));

    if (!write_chunked_paced(cfd, img.data(), img.size(), static_cast<size_t>(chunk_sz), pacing))
      break;
    if (!write_full(cfd, "\r\n", 2))
      break;
  }

  // Restore original settings
  global_jpeg[ch]->req_width = orig_w;
  global_jpeg[ch]->req_height = orig_h;
  global_jpeg[ch]->req_fps = orig_fps;
  global_jpeg[ch]->reconfig = true;
  global_jpeg[ch]->request();

  ::close(cfd);
}
