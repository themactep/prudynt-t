#include "IPCServer.hpp"
#include "Logger.hpp"
#include "WS.hpp"  // for JSON processing callbacks
#include "globals.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <vector>

// Forward decls from WS.cpp we can reuse
extern bool get_snapshot_ch(int ch, std::vector<unsigned char> &image);

// Provide a thin wrapper to reuse JSON handlers implemented in WS.cpp
static bool process_json_with_ws(const std::string &req, std::string &resp) {
    return WS::process_json(req, resp);
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
        handle_client(c);
        close(c);
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
        if (process_json_with_ws(json, resp)) {
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
            size_t start = pos; while (pos < req.size() && isdigit(req[pos])) pos++;
            if (start == pos) return -999999; return std::stoi(req.substr(start, pos-start));
        };
        int v;
        v = find_kv("ch"); if (v != -999999) ch = v;
        v = find_kv("q");  if (v != -999999) q = v;

        // optional: quality override (like HTTP path)
        if (ch >= 0 && ch < NUM_VIDEO_CHANNELS && global_jpeg[ch]) {
            if (q >= 1 && q <= 100) global_jpeg[ch]->quality_override = q;
        }

        std::vector<unsigned char> img;
        if (get_snapshot_ch(ch, img) && img.size() > 0) {
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

    const char *err = "ERR unknown_command\n";
    write(fd, err, strlen(err));
    return 0;
}

