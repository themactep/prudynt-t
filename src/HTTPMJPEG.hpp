#ifndef HTTP_MJPEG_HPP
#define HTTP_MJPEG_HPP

#include <atomic>
#include <thread>

class HTTPMJPEG {
public:
  HTTPMJPEG();
  ~HTTPMJPEG();

  void start(int port = 8081, bool enable_mjpeg = true, bool enable_api = false, 
             bool auth_required = true, const char *username = "thingino", 
             const char *password = "thingino"); // no-op if already running
  void stop();                 // best-effort stop

private:
  void server_loop(int port);
  void handle_client(int cfd);

  std::thread th_;
  std::atomic<bool> running_{false};
  int listen_fd_{-1};
  bool mjpeg_enabled_{true};
  bool api_enabled_{false};
  bool auth_required_{true};
  const char *username_{"thingino"};
  const char *password_{"thingino"};
};

#endif // HTTP_MJPEG_HPP
