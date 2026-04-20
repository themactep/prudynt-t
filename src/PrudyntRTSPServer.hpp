#ifndef PRUDYNT_RTSP_SERVER_HPP
#define PRUDYNT_RTSP_SERVER_HPP

#include "RTSPServer.hh"

class PrudyntRTSPServer : public RTSPServer {
public:
  static PrudyntRTSPServer *
  createNew(UsageEnvironment &env, Port ourPort = 554,
            UserAuthenticationDatabase *authDatabase = nullptr,
            unsigned reclamationSeconds = 65);

protected:
  PrudyntRTSPServer(UsageEnvironment &env, int ourSocketIPv4, int ourSocketIPv6,
                    Port ourPort, UserAuthenticationDatabase *authDatabase,
                    unsigned reclamationSeconds);

  class PrudyntRTSPClientConnection : public RTSPServer::RTSPClientConnection {
  public:
    PrudyntRTSPClientConnection(RTSPServer &ourServer, int clientSocket,
                                struct sockaddr_storage const &clientAddr);

  protected:
    virtual void handleCmd_DESCRIBE(char const *urlPreSuffix,
                                    char const *urlSuffix,
                                    char const *fullRequestStr) override;
    virtual void
    handleCmd_DESCRIBE_afterLookup(ServerMediaSession *session) override;

  private:
    static bool requestHasBackchannelRequire(char const *fullRequestStr);

    bool fDescribeBackchannelRequested;
  };

protected: // redefined virtual function:
  virtual ClientConnection *
  createNewClientConnection(int clientSocket,
                            struct sockaddr_storage const &clientAddr) override;
};

#endif // PRUDYNT_RTSP_SERVER_HPP
