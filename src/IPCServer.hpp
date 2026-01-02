#ifndef IPC_SERVER_HPP
#define IPC_SERVER_HPP

#include <atomic>
#include <string>
#include <thread>

class IPCServer {
public:
  IPCServer();
  ~IPCServer();

  // Launch background server thread (no-op if already running)
  void start();
  // Signal shutdown and join
  void stop();

  // Configure HTTP JSON API listener (call before start)
  void configure_http(int port, bool enabled);

private:
  void server_loop();
  void http_loop();
  int handle_client(int fd);
  int handle_http_client(int fd);

  std::thread th_;
  std::thread http_th_;
  std::atomic<bool> running_{false};
  std::atomic<bool> http_running_{false};
  int http_port_{0};
  bool http_enabled_{false};
  int http_listen_fd_{-1};
};

#endif // IPC_SERVER_HPP
