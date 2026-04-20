#include "PrudyntRTSPServer.hpp"

#include <GroupsockHelper.hh>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

static constexpr char kBackchannelRequireTag[] =
    "www.onvif.org/ver20/backchannel";
static constexpr char kBackchannelRequireEnvVar[] =
    "PRUDYNT_RTSP_DESCRIBE_REQUIRE_BACKCHANNEL";

PrudyntRTSPServer *
PrudyntRTSPServer::createNew(UsageEnvironment &env, Port ourPort,
                             UserAuthenticationDatabase *authDatabase,
                             unsigned reclamationSeconds) {
  int ourSocketIPv4 = setUpOurSocket(env, ourPort, AF_INET);
  int ourSocketIPv6 = setUpOurSocket(env, ourPort, AF_INET6);
  if (ourSocketIPv4 < 0 && ourSocketIPv6 < 0)
    return nullptr;

  return new PrudyntRTSPServer(env, ourSocketIPv4, ourSocketIPv6, ourPort,
                               authDatabase, reclamationSeconds);
}

PrudyntRTSPServer::PrudyntRTSPServer(UsageEnvironment &env, int ourSocketIPv4,
                                     int ourSocketIPv6, Port ourPort,
                                     UserAuthenticationDatabase *authDatabase,
                                     unsigned reclamationSeconds)
    : RTSPServer(env, ourSocketIPv4, ourSocketIPv6, ourPort, authDatabase,
                 reclamationSeconds) {
}

PrudyntRTSPServer::PrudyntRTSPClientConnection::PrudyntRTSPClientConnection(
    RTSPServer &ourServer, int clientSocket,
    struct sockaddr_storage const &clientAddr)
    : RTSPClientConnection(ourServer, clientSocket, clientAddr, False),
      fDescribeBackchannelRequested(false) {
}

bool PrudyntRTSPServer::PrudyntRTSPClientConnection::
    requestHasBackchannelRequire(char const *fullRequestStr) {
  if (fullRequestStr == nullptr)
    return false;

  std::string lowered(fullRequestStr);
  std::transform(
      lowered.begin(), lowered.end(), lowered.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  size_t searchPos = 0;
  while ((searchPos = lowered.find("require:", searchPos)) !=
         std::string::npos) {
    if (searchPos != 0 && lowered[searchPos - 1] != '\n' &&
        lowered[searchPos - 1] != '\r') {
      searchPos += 8;
      continue;
    }

    size_t lineEnd = lowered.find('\n', searchPos);
    std::string requireLine = lowered.substr(searchPos, lineEnd - searchPos);
    if (requireLine.find(kBackchannelRequireTag) != std::string::npos)
      return true;

    if (lineEnd == std::string::npos)
      break;
    searchPos = lineEnd + 1;
  }

  return false;
}

void PrudyntRTSPServer::PrudyntRTSPClientConnection::handleCmd_DESCRIBE(
    char const *urlPreSuffix, char const *urlSuffix,
    char const *fullRequestStr) {
  fDescribeBackchannelRequested = requestHasBackchannelRequire(fullRequestStr);
  RTSPServer::RTSPClientConnection::handleCmd_DESCRIBE(urlPreSuffix, urlSuffix,
                                                       fullRequestStr);
}

void PrudyntRTSPServer::PrudyntRTSPClientConnection::
    handleCmd_DESCRIBE_afterLookup(ServerMediaSession *session) {
  const char *oldState = getenv(kBackchannelRequireEnvVar);
  const bool hadOldState = oldState != nullptr;
  const std::string oldStateValue = hadOldState ? oldState : "";

  if (fDescribeBackchannelRequested) {
    setenv(kBackchannelRequireEnvVar, "1", 1);
  } else {
    unsetenv(kBackchannelRequireEnvVar);
  }

  RTSPServer::RTSPClientConnection::handleCmd_DESCRIBE_afterLookup(session);

  if (hadOldState) {
    setenv(kBackchannelRequireEnvVar, oldStateValue.c_str(), 1);
  } else {
    unsetenv(kBackchannelRequireEnvVar);
  }
}

RTSPServer::ClientConnection *PrudyntRTSPServer::createNewClientConnection(
    int clientSocket, struct sockaddr_storage const &clientAddr) {
  return new PrudyntRTSPClientConnection(*this, clientSocket, clientAddr);
}
