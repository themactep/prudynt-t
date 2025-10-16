#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

static const char *SOCK_PATH = "/run/prudynt/prudynt.sock";

static int connect_sock() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un sa{}; sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, SOCK_PATH, sizeof(sa.sun_path)-1);
    if (connect(fd, (sockaddr*)&sa, sizeof(sa)) < 0) {
        close(fd); return -1;
    }
    return fd;
}

static std::string read_all(int fd) {
    std::string out; char buf[4096]; ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) out.append(buf, buf + n);
    return out;
}

static int cmd_json(int argc, char **argv) {
    std::string payload;
    if (argc >= 1 && strcmp(argv[0], "-") == 0) {
        // read from stdin
        char buf[4096]; ssize_t n;
        while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) payload.append(buf, buf + n);
    } else if (argc >= 1) {
        payload = argv[0];
    } else {
        // also read stdin if no args
        char buf[4096]; ssize_t n;
        while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) payload.append(buf, buf + n);
    }

    // trim trailing newlines
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r')) payload.pop_back();

    int fd = connect_sock();
    if (fd < 0) { fprintf(stderr, "prudyntctl: connect %s failed: %s\n", SOCK_PATH, strerror(errno)); return 2; }

    // Allow bare JSON body or prefixed with JSON
    if (!payload.empty() && payload[0] == '{') {
        // write JSON directly
        (void)write(fd, payload.c_str(), payload.size());
    } else {
        std::string line = "JSON "; line += payload;
        line += "\n";
        (void)write(fd, line.c_str(), line.size());
    }
    // Important: signal EOF on the write side so server stops reading and processes
    shutdown(fd, SHUT_WR);

    std::string resp = read_all(fd);
    close(fd);
    // print as-is
    write(STDOUT_FILENO, resp.data(), resp.size());
    return 0;
}

static int cmd_mjpeg(int argc, char **argv) {
    int ch = 0, q = -1, fps = 5, w = -1, h = -1;
    const char *boundary = "prudyntmjpegboundary";
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "-c") == 0 && i+1 < argc) { ch = atoi(argv[++i]); }
        else if (strcmp(argv[i], "-q") == 0 && i+1 < argc) { q = atoi(argv[++i]); }
        else if (strcmp(argv[i], "-f") == 0 && i+1 < argc) { fps = atoi(argv[++i]); if (fps < 1) fps = 1; if (fps > 30) fps = 30; }
        else if (strcmp(argv[i], "-w") == 0 && i+1 < argc) { w = atoi(argv[++i]); }
        else if (strcmp(argv[i], "-h") == 0 && i+1 < argc) { h = atoi(argv[++i]); }
        else if (strcmp(argv[i], "-b") == 0 && i+1 < argc) { boundary = argv[++i]; }
    }

    int fd = connect_sock();
    if (fd < 0) { fprintf(stderr, "prudyntctl: connect %s failed: %s\n", SOCK_PATH, strerror(errno)); return 2; }

    char line[256];
    // Send server-side streaming request
    int n = snprintf(line, sizeof(line), "MJPEG ch=%d f=%d boundary=%s", ch, fps, boundary);
    // Optionals
    if (q >= 1 && q <= 100) n += snprintf(line+n, sizeof(line)-n, " q=%d", q);
    if (w > 0 && h > 0) n += snprintf(line+n, sizeof(line)-n, " w=%d h=%d", w, h);
    line[n++]='\n'; line[n]='\0';
    (void)write(fd, line, strlen(line));
    shutdown(fd, SHUT_WR);

    // Pass-through: read everything from server and write to stdout until EOF
    std::vector<char> buf(8192);
    while (1) {
        ssize_t r = read(fd, buf.data(), buf.size());
        if (r <= 0) break;
        write(STDOUT_FILENO, buf.data(), r);
    }
    close(fd);
    return 0;
}


static int cmd_snapshot(int argc, char **argv) {
    int ch = 0, q = -1;
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "-c") == 0 && i+1 < argc) { ch = atoi(argv[++i]); }
        else if (strcmp(argv[i], "-q") == 0 && i+1 < argc) { q = atoi(argv[++i]); }
    }

    int fd = connect_sock();
    if (fd < 0) { fprintf(stderr, "prudyntctl: connect %s failed: %s\n", SOCK_PATH, strerror(errno)); return 2; }

    char line[128];
    if (q >= 1 && q <= 100)
        snprintf(line, sizeof(line), "SNAPSHOT ch=%d q=%d\n", ch, q);
    else
        snprintf(line, sizeof(line), "SNAPSHOT ch=%d\n", ch);
    (void)write(fd, line, strlen(line));
    // Signal EOF on write side so server proceeds
    shutdown(fd, SHUT_WR);

    // Read header: OK <len>\n
    std::string hdr; char c;
    while (read(fd, &c, 1) == 1) { hdr.push_back(c); if (c == '\n') break; }
    int len = -1;
    if (sscanf(hdr.c_str(), "OK %d", &len) != 1 || len < 0) {
        // Print whatever we got (likely ERR ...)
        write(STDOUT_FILENO, hdr.data(), hdr.size());
        // drain rest just in case
        std::string rest = read_all(fd);
        write(STDOUT_FILENO, rest.data(), rest.size());
        close(fd);
        return 1;
    }

    // Stream exactly len bytes to stdout (server now sends clean JPEG starting at SOI)
    std::vector<char> buf(8192);
    int remaining = len;
    while (remaining > 0) {
        ssize_t r = read(fd, buf.data(), remaining > (int)buf.size() ? buf.size() : remaining);
        if (r <= 0) break;
        write(STDOUT_FILENO, buf.data(), r);
        remaining -= (int)r;
    }
    close(fd);
    return remaining == 0 ? 0 : 1;
}

static int cmd_events() {
    int fd = connect_sock();
    if (fd < 0) { fprintf(stderr, "prudyntctl: connect %s failed: %s\n", SOCK_PATH, strerror(errno)); return 2; }
    const char *hello = "EVENTS\n";
    (void)write(fd, hello, strlen(hello));
    // Signal EOF on write side so server starts streaming
    shutdown(fd, SHUT_WR);
    // Read and forward until server closes or error
    char buf[4096]; ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(STDOUT_FILENO, buf, n);
    }
    close(fd);
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s json <json-string>|-                    # read stdin with '-'\n"
        "  %s snapshot [-c CH] [-q Q]                 # writes a single JPEG to stdout\n"
        "  %s mjpeg    [-c CH] [-q Q] [-f F] [-w W] [-h H]  # multipart MJPEG (server-side)\n"
        "  %s events                                  # newline-delimited JSON events\n",
        prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    const char *cmd = argv[1];
    if (strcmp(cmd, "json") == 0) {
        return cmd_json(argc-2, argv+2);
    } else if (strcmp(cmd, "snapshot") == 0) {
        return cmd_snapshot(argc-2, argv+2);
    } else if (strcmp(cmd, "mjpeg") == 0) {
        return cmd_mjpeg(argc-2, argv+2);
    } else if (strcmp(cmd, "events") == 0) {
        return cmd_events();
    } else {
        usage(argv[0]); return 1;
    }
}

