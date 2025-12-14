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

private:
  void server_loop();
  int handle_client(int fd);

  std::thread th_;
  std::atomic<bool> running_{false};
};

#endif // IPC_SERVER_HPP
