#include "IPCServer.hpp"
#include "Logger.hpp"
#include "JsonAPI.hpp"  // JSON processing via jct
#include "globals.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <vector>
#include <sys/stat.h>
#include <cctype>


// Local snapshot helper: return a clean JPEG starting from SOI
bool get_snapshot_ch_local(int ch, std::vector<unsigned char> &image) {
    std::unique_lock lck(mutex_main);
    if (ch < 0 || ch >= NUM_VIDEO_CHANNELS || !global_jpeg[ch]) return false;
    auto &buf = global_jpeg[ch]->snapshot_buf;
    if (buf.empty()) return false;

    // Find first JPEG SOI marker 0xFF 0xD8 and slice from there
    size_t soi = std::string::npos;
    for (size_t i = 0; i + 1 < buf.size(); ++i) {
        if (buf[i] == 0xFF && buf[i + 1] == 0xD8) { soi = i; break; }
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

static int set_cloexec(int fd) { return fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC); }

static const char *SOCK_PATH = "/run/prudynt/prudynt.sock";

IPCServer::IPCServer() {}
IPCServer::~IPCServer() { stop(); }

void IPCServer::start() {
    bool expected = false;
    if (running_.compare_exchange_strong(expected, true)) {
        th_ = std::thread(&IPCServer::server_loop, this);
    }
}

void IPCServer::stop() {
    bool expected = true;
    if (running_.compare_exchange_strong(expected, false)) {
        // Wake accept by connecting to our own socket
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            sockaddr_un sa{}; sa.sun_family = AF_UNIX; std::strncpy(sa.sun_path, SOCK_PATH, sizeof(sa.sun_path)-1);
            connect(fd, (sockaddr*)&sa, sizeof(sa));
            close(fd);
        }
        if (th_.joinable()) th_.join();
    }
}

void IPCServer::server_loop() {
    // Ensure directory exists
    {
        const char* dir = "/run/prudynt";
        ::mkdir(dir, 0775);
    }
    ::unlink(SOCK_PATH);

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { LOG_ERROR("IPC: socket() failed: " << strerror(errno)); return; }
    set_cloexec(s);

    sockaddr_un sa{}; sa.sun_family = AF_UNIX; std::strncpy(sa.sun_path, SOCK_PATH, sizeof(sa.sun_path)-1);
    if (bind(s, (sockaddr*)&sa, sizeof(sa)) < 0) {
        LOG_ERROR("IPC: bind(" << SOCK_PATH << ") failed: " << strerror(errno));
        close(s); return;
    }
    // permissions: allow root+www/cgi
    ::chmod(SOCK_PATH, 0660);

    if (listen(s, 4) < 0) { LOG_ERROR("IPC: listen failed: " << strerror(errno)); close(s); return; }

    LOG_INFO("IPC: listening on " << SOCK_PATH);

    while (running_) {
        int c = accept(s, nullptr, nullptr);
        if (c < 0) {
            if (errno == EINTR) continue;
            if (!running_) break;
            LOG_ERROR("IPC: accept failed: " << strerror(errno));
            continue;
        }
        set_cloexec(c);
        // Handle each client in a detached thread so EVENTS can stream without blocking accept
        std::thread([this](int client_fd){
            this->handle_client(client_fd);
            ::close(client_fd);
        }, c).detach();
    }

    close(s);
    ::unlink(SOCK_PATH);
}

static bool starts_with(const std::string &s, const char *pfx) {
    return s.rfind(pfx, 0) == 0; // prefix match
}

int IPCServer::handle_client(int fd) {
    // Read all into buffer until EOF
    std::string req;
    char buf[2048];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) req.append(buf, buf + n);

    if (req.empty()) return 0;

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
        int ch = 0; int q = -1;
        // very small parser
        auto find_kv = [&](const char* key)->int{
            size_t pos = req.find(key);
            if (pos == std::string::npos) return -999999;
            pos += std::strlen(key);
            while (pos < req.size() && (req[pos] == ' ' || req[pos] == '=')) pos++;
            size_t start = pos;
            while (pos < req.size() && isdigit(static_cast<unsigned char>(req[pos]))) pos++;
            if (start == pos) return -999999;
            return std::stoi(req.substr(start, pos - start));
        };
        int v;
        v = find_kv("ch"); if (v != -999999) ch = v;
        v = find_kv("q");  if (v != -999999) q = v;

        // optional: quality override (like HTTP path)
        if (ch >= 0 && ch < NUM_VIDEO_CHANNELS && global_jpeg[ch]) {
            if (q >= 1 && q <= 100) global_jpeg[ch]->quality_override = q;
        }
        // Signal demand to speed up capture and wake JPEG worker if idle
        if (ch >= 0 && ch < NUM_VIDEO_CHANNELS && global_jpeg[ch]) {
            global_jpeg[ch]->request();
        }

        // Try to get a fresh snapshot, waiting briefly if not yet available
        std::vector<unsigned char> img;
        const int max_wait_ms = 250;
        int waited = 0;
        while (!(get_snapshot_ch_local(ch, img) && !img.empty()) && waited < max_wait_ms) {
            usleep(10 * 1000);
            waited += 10;
        }
        // Fallback: if requested channel isn't available, try ch0 to avoid client hangs
        if (img.empty() && ch != 0) {
            get_snapshot_ch_local(0, img);
        }
        if (!img.empty()) {
            // Protocol: "OK <len>\n<bytes>"
            char hdr[64];
            int len = (int)img.size();
            int m = snprintf(hdr, sizeof(hdr), "OK %d\n", len);
            write(fd, hdr, m);
            write(fd, (const char*)img.data(), len);
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
        auto find_kv = [&](const char* key)->int{
            size_t pos = req.find(key);
            if (pos == std::string::npos) return -999999;
            pos += std::strlen(key);
            while (pos < req.size() && (req[pos] == ' ' || req[pos] == '=')) pos++;
            size_t start = pos;
            while (pos < req.size() && isdigit(static_cast<unsigned char>(req[pos]))) pos++;
            if (start == pos) return -999999;
            return std::stoi(req.substr(start, pos - start));
        };
        auto find_str = [&](const char* key)->std::string{
            size_t pos = req.find(key);
            if (pos == std::string::npos) return {};
            pos += std::strlen(key);
            while (pos < req.size() && (req[pos] == ' ' || req[pos] == '=')) pos++;
            size_t start = pos;
            while (pos < req.size() && !isspace(static_cast<unsigned char>(req[pos]))) pos++;
            return req.substr(start, pos - start);
        };
        int v;
        v = find_kv("ch"); if (v != -999999) ch = v;
        v = find_kv("w");  if (v != -999999) w = v;
        v = find_kv("h");  if (v != -999999) h = v;
        v = find_kv("f");  if (v != -999999) fps = v;
        v = find_kv("q");  if (v != -999999) q = v;
        std::string b = find_str("boundary"); if (!b.empty()) boundary = b;

        if (ch < 0 || ch >= NUM_VIDEO_CHANNELS || !global_jpeg[ch]) {
            const char *err = "ERR bad_ch\n";
            write(fd, err, strlen(err));
            return 0;
        }

        // Quantize w/h to multiples of 16 and cap to source size
        if (w > 0 && h > 0) {
            auto src_w = (global_jpeg[ch]->streamChn == 0) ? cfg->stream0.width : cfg->stream1.width;
            auto src_h = (global_jpeg[ch]->streamChn == 0) ? cfg->stream0.height : cfg->stream1.height;
            if (w > src_w) w = src_w;
            if (h > src_h) h = src_h;
            w = (w / 16) * 16; if (w < 16) w = 16;
            h = (h / 16) * 16; if (h < 16) h = 16;
        }
        if (fps > 0) {
            int max_fps = (cfg->sensor.fps > 0) ? cfg->sensor.fps : 30;
            if (fps > max_fps) fps = max_fps;
            if (fps < 1) fps = 1;
        }
        if (q >= 1 && q <= 100) global_jpeg[ch]->quality_override = q;

        // Remember originals to restore after disconnect
        int orig_w = global_jpeg[ch]->stream->width;
        int orig_h = global_jpeg[ch]->stream->height;
        int orig_fps = global_jpeg[ch]->stream->fps;

        // Request reconfiguration and wake worker
        if (w > 0 && h > 0) { global_jpeg[ch]->req_width = w; global_jpeg[ch]->req_height = h; }
        if (fps > 0) { global_jpeg[ch]->req_fps = fps; }
        global_jpeg[ch]->reconfig = true;
        global_jpeg[ch]->request();

        // Wait briefly for reconfig to apply
        int wait_ms = 500; while (global_jpeg[ch]->reconfig.load() && wait_ms > 0) { usleep(10*1000); wait_ms -= 10; }

        // Start streaming loop: multipart MJPEG parts
        std::vector<unsigned char> img;
        std::string hdr;
        while (1) {
            img.clear();
            if (!get_snapshot_ch_local(ch, img) || img.empty()) {
                // if no data yet, yield a bit
                usleep(10 * 1000);
                continue;
            }
            char ph[128];
            int len = (int)img.size();
            int m = snprintf(ph, sizeof(ph), "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", boundary.c_str(), len);
            if (write(fd, ph, m) < 0) break;
            if (write(fd, (const char*)img.data(), len) < 0) break;
            if (write(fd, "\r\n", 2) < 0) break;
            // Pace output
            int usec = 1000000 / (fps > 0 ? fps : orig_fps);
            usleep(usec);
        }

        // Restore FPS (and original size) after client disconnect
        global_jpeg[ch]->req_width = orig_w;
        global_jpeg[ch]->req_height = orig_h;
        global_jpeg[ch]->req_fps = orig_fps;
        global_jpeg[ch]->reconfig = true;
        global_jpeg[ch]->request();
        return 0;
    }

    if (starts_with(req, "EVENTS")) {
        // Stream newline-delimited JSON events until client closes
        while (running_) {
            // Build a compact stats object
            char line[256];
            int fps0 = cfg->stream0.stats.fps;
            int bps0 = cfg->stream0.stats.bps;
            int fps1 = cfg->stream1.stats.fps;
            int bps1 = cfg->stream1.stats.bps;
            int fps2 = cfg->stream2.stats.fps;
            int bps2 = cfg->stream2.stats.bps;
            int n = snprintf(line, sizeof(line),
                "{\"ts\":%ld,\"stats\":{\"stream0\":{\"fps\":%d,\"Bps\":%d},\"stream1\":{\"fps\":%d,\"Bps\":%d},\"stream2\":{\"fps\":%d,\"Bps\":%d}}}\n",
                (long)time(NULL), fps0, bps0, fps1, bps1, fps2, bps2);
            ssize_t w = write(fd, line, n);
            if (w <= 0) break; // client closed
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        return 0;
    }

    const char *err = "ERR unknown_command\n";
    write(fd, err, strlen(err));
    return 0;
}

