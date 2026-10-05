#include "onvif_server.h"

#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

#include "hilog/log.h"

namespace osc {

static constexpr unsigned int ON_LOG_DOMAIN = 0x0015;
static constexpr const char *ON_LOG_TAG = "OnvifServer";
#define ON_LOG(fmt, ...)                                                                        \
    OH_LOG_Print(LOG_APP, LOG_INFO, ON_LOG_DOMAIN, ON_LOG_TAG, "%{public}s: " fmt, __func__,    \
        ##__VA_ARGS__)
#define ON_ERR(fmt, ...)                                                                        \
    OH_LOG_Print(LOG_APP, LOG_ERROR, ON_LOG_DOMAIN, ON_LOG_TAG, "%{public}s: " fmt, __func__,   \
        ##__VA_ARGS__)

// 取 wlan 口 IPv4（XAddrs 用）
static std::string GetLocalIp()
{
  struct ifaddrs *ifas = nullptr;
  if (getifaddrs(&ifas) != 0) {
    return "";
  }
  std::string ip;
  for (auto *ifa = ifas; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    std::string name = ifa->ifa_name != nullptr ? ifa->ifa_name : "";
    if (name.rfind("wlan", 0) == 0) {
      char buf[INET_ADDRSTRLEN] = {0};
      inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(ifa->ifa_addr)->sin_addr, buf,
          sizeof(buf));
      std::string candidate(buf);
      if (candidate.rfind("127.", 0) != 0) {
        ip = candidate;
        break;
      }
    }
  }
  freeifaddrs(ifas);
  return ip;
}

struct OnvifServer::Impl {
  std::atomic<bool> running{false};
  int httpPort = 8000;
  int rtspPort = 8554;
  std::string deviceUuid = "urn:uuid:6f2a1c30-osc4-4b71-9a2c-opencamera01";
  int httpFd = -1;
  std::thread httpThread;
  std::thread discoveryThread;

  void HttpLoop();
  void DiscoveryLoop();
  void HandleHttp(int fd);
  std::string HandleSoap(const std::string &req);
};

OnvifServer &OnvifServer::Instance()
{
  static OnvifServer inst;
  return inst;
}

OnvifServer::~OnvifServer()
{
  Stop();
}

bool OnvifServer::IsRunning() const
{
  return impl_ != nullptr && impl_->running.load();
}

// ---------------- SOAP 应答 ----------------
std::string OnvifServer::Impl::HandleSoap(const std::string &req)
{
  auto envOpen = [](const std::string &body) {
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
      "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
      "xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
      "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\">"
      "<s:Body>") + body + "</s:Body></s:Envelope>";
  };
  std::string ip = GetLocalIp();

  if (req.find("GetSystemDateAndTime") != std::string::npos) {
    time_t now = time(nullptr);
    struct tm t = {};
    gmtime_r(&now, &t);
    char buf[256] = {0};
    snprintf(buf, sizeof(buf),
      "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime>"
      "<tt:DateTimeType>UTC</tt:DateTimeType>"
      "<tt:UTCDateTime><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day>"
      "<tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:UTCDateTime>"
      "</tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>",
      t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return envOpen(buf);
  }
  if (req.find("GetDeviceInformation") != std::string::npos) {
    return envOpen(
      "<tds:GetDeviceInformationResponse>"
      "<tds:Manufacturer>linuxsuren</tds:Manufacturer>"
      "<tds:Model>OpenSportCamera</tds:Model>"
      "<tds:FirmwareVersion>0.2.0</tds:FirmwareVersion>"
      "<tds:SerialNumber>OSC0001</tds:SerialNumber>"
      "<tds:HardwareId>OSC-HW1</tds:HardwareId>"
      "</tds:GetDeviceInformationResponse>");
  }
  if (req.find("GetCapabilities") != std::string::npos) {
    char media[256] = {0};
    snprintf(media, sizeof(media),
      "<tt:Media><tt:XAddr>http://%s:%d/onvif/media_service</tt:XAddr>"
      "<tt:RTPMulticast>false</tt:RTPMulticast><tt:Streaming><tt:RTP_USERTCP>false</tt:RTP_USERTCP>"
      "</tt:Streaming></tt:Media>", ip.c_str(), httpPort);
    char dev[256] = {0};
    snprintf(dev, sizeof(dev),
      "<tt:Device><tt:XAddr>http://%s:%d/onvif/device_service</tt:XAddr></tt:Device>",
      ip.c_str(), httpPort);
    return envOpen(std::string("<tds:GetCapabilitiesResponse><tds:Capabilities>") + dev + media +
      "</tds:Capabilities></tds:GetCapabilitiesResponse>");
  }
  if (req.find("GetServices") != std::string::npos) {
    char svc[512] = {0};
    snprintf(svc, sizeof(svc),
      "<tds:GetServicesResponse>"
      "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace>"
      "<tds:XAddr>http://%s:%d/onvif/device_service</tds:XAddr></tds:Service>"
      "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace>"
      "<tds:XAddr>http://%s:%d/onvif/media_service</tds:XAddr></tds:Service>"
      "</tds:GetServicesResponse>", ip.c_str(), httpPort, ip.c_str(), httpPort);
    return envOpen(svc);
  }
  if (req.find("GetProfiles") != std::string::npos) {
    return envOpen(
      "<trt:GetProfilesResponse>"
      "<trt:Profiles fixed=\"true\" token=\"profile_1_h264\">"
      "<tt:Name>main_h264</tt:Name>"
      "<tt:VideoSourceConfiguration token=\"vsc_1\">"
      "<tt:Name>vsc</tt:Name><tt:SourceToken>src_1</tt:SourceToken>"
      "<tt:Bounds x=\"0\" y=\"0\" width=\"1920\" height=\"1080\"/>"
      "</tt:VideoSourceConfiguration>"
      "<tt:VideoEncoderConfiguration token=\"vec_1\">"
      "<tt:Name>h264_1080p</tt:Name>"
      "<tt:Encoding>H264</tt:Encoding>"
      "<tt:Resolution><tt:Width>1920</tt:Width><tt:Height>1080</tt:Height></tt:Resolution>"
      "<tt:RateControl><tt:FrameRateLimit>30</tt:FrameRateLimit>"
      "<tt:BitrateLimit>20000</tt:BitrateLimit></tt:RateControl>"
      "</tt:VideoEncoderConfiguration>"
      "</trt:Profiles>"
      "</trt:GetProfilesResponse>");
  }
  if (req.find("GetStreamUri") != std::string::npos) {
    char uri[128] = {0};
    snprintf(uri, sizeof(uri), "rtsp://%s:%d/live", ip.c_str(), rtspPort);
    char body[512] = {0};
    snprintf(body, sizeof(body),
      "<trt:GetStreamUriResponse><trt:MediaUri>"
      "<tt:Uri>%s</tt:Uri>"
      "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
      "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
      "<tt:Timeout>PT60S</tt:Timeout>"
      "</trt:MediaUri></trt:GetStreamUriResponse>", uri);
    ON_LOG("GetStreamUri -> %{public}s", uri);
    return envOpen(body);
  }
  if (req.find("GetHostname") != std::string::npos) {
    char body[256] = {0};
    snprintf(body, sizeof(body),
      "<tds:GetHostnameResponse><tds:HostnameInformation>"
      "<tt:Name>%s</tt:Name></tds:HostnameInformation></tds:GetHostnameResponse>", ip.c_str());
    return envOpen(body);
  }
  // 未知动作：空成功应答
  return envOpen("");
}

void OnvifServer::Impl::HandleHttp(int fd)
{
  // 超时保护：3秒未收到完整请求则断开（防连接堆积假死）
  struct timeval tv = {3, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  char buf[8192] = {0};
  std::string req;
  while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      close(fd);
      return;
    }
    req.append(buf, static_cast<size_t>(n));
  }
  std::string soap = HandleSoap(req);
  char head[256] = {0};
  snprintf(head, sizeof(head),
    "HTTP/1.1 200 OK\r\nContent-Type: application/soap+xml; charset=utf-8\r\n"
    "Content-Length: %zu\r\nConnection: close\r\n\r\n", soap.size());
  send(fd, head, strlen(head), MSG_NOSIGNAL);
  send(fd, soap.data(), soap.size(), MSG_NOSIGNAL);
  close(fd);
}

void OnvifServer::Impl::HttpLoop()
{
  while (running.load()) {
    sockaddr_in peer = {};
    socklen_t len = sizeof(peer);
    int fd = accept(httpFd, reinterpret_cast<sockaddr *>(&peer), &len);
    if (fd < 0) {
      if (!running.load()) {
        break;
      }
      continue;
    }
    HandleHttp(fd);
  }
}

// ---------------- WS-Discovery 应答 ----------------
void OnvifServer::Impl::DiscoveryLoop()
{
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    ON_ERR("discovery socket failed");
    return;
  }
  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(3702);
  if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    ON_ERR("discovery bind 3702 failed");
    close(fd);
    return;
  }
  ip_mreq mreq = {};
  mreq.imr_multiaddr.s_addr = inet_addr("239.255.255.250");
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
  ON_LOG("ws-discovery responder on 239.255.255.250:3702");

  char buf[8192] = {0};
  while (running.load()) {
    sockaddr_in from = {};
    socklen_t flen = sizeof(from);
    struct timeval tv = {1, 0};
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    int r = select(fd + 1, &rfds, nullptr, nullptr, &tv);
    if (r <= 0 || !running.load()) {
      continue;
    }
    ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr *>(&from), &flen);
    if (n <= 0) {
      continue;
    }
    buf[n] = 0;
    std::string msg(buf);
    if (msg.find("Probe") == std::string::npos) {
      continue;
    }
    // 过滤目标类型（NetworkVideoTransmitter / Device 均应答）
    bool match = true;
    if (msg.find("Types") != std::string::npos) {
      match = msg.find("NetworkVideoTransmitter") != std::string::npos ||
        msg.find("dp:Device") != std::string::npos || msg.find("tn:Device") != std::string::npos ||
        msg.find("d:Device") != std::string::npos;
    }
    if (!match) {
      continue;
    }
    // 提取 MessageID / RelatesTo
    std::string relatesTo;
    size_t midPos = msg.find("<a:MessageID>");
    if (midPos != std::string::npos) {
      size_t s = midPos + 13;
      size_t e = msg.find("</a:MessageID>", s);
      if (e != std::string::npos) {
        relatesTo = msg.substr(s, e - s);
      }
    }
    std::string ip = GetLocalIp();
    if (ip.empty()) {
      continue;
    }
    char xaddrs[160] = {0};
    snprintf(xaddrs, sizeof(xaddrs), "http://%s:%d/onvif/device_service", ip.c_str(), httpPort);
    char probeMatch[4096] = {0};
    snprintf(probeMatch, sizeof(probeMatch),
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
      "xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
      "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
      "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
      "<s:Header>"
      "<a:MessageId>urn:uuid:osc-%lld</a:MessageId>"
      "<a:RelatesTo>%s</a:RelatesTo>"
      "<a:To>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</a:To>"
      "<a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</a:Action>"
      "<d:AppSequence InstanceId=\"1\" MessageNumber=\"%lld\"/>"
      "</s:Header>"
      "<s:Body>"
      "<d:ProbeMatches>"
      "<d:ProbeMatch>"
      "<a:EndpointReference><a:Address>%s</a:Address></a:EndpointReference>"
      "<d:Types>dn:NetworkVideoTransmitter</d:Types>"
      "<d:Scopes>onvif://www.onvif.org/type/video_encoder/h264 onvif://www.linuxsuren.com/osc</d:Scopes>"
      "<d:XAddrs>%s</d:XAddrs>"
      "<d:MetadataVersion>1</d:MetadataVersion>"
      "</d:ProbeMatch>"
      "</d:ProbeMatches>"
      "</s:Body>"
      "</s:Envelope>",
      static_cast<long long>(time(nullptr) * 1000), relatesTo.c_str(),
      static_cast<long long>(time(nullptr)), deviceUuid.c_str(), xaddrs);
    sendto(fd, probeMatch, strlen(probeMatch), 0, reinterpret_cast<sockaddr *>(&from), flen);
    ON_LOG("ProbeMatch sent to %{public}s:%d", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
  }
  close(fd);
}

int OnvifServer::Start(int httpPort, int rtspPort)
{
  if (IsRunning()) {
    return 0;
  }
  if (impl_ == nullptr) {
    impl_ = new Impl();
  }
  impl_->httpPort = httpPort;
  impl_->rtspPort = rtspPort;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(httpPort));
  if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    ON_ERR("bind http :%{public}d failed", httpPort);
    close(fd);
    return -2;
  }
  if (listen(fd, 4) != 0) {
    close(fd);
    return -3;
  }
  impl_->httpFd = fd;
  impl_->running.store(true);
  impl_->httpThread = std::thread(&Impl::HttpLoop, impl_);
  impl_->discoveryThread = std::thread(&Impl::DiscoveryLoop, impl_);
  ON_LOG("onvif service on :%{public}d (rtsp=%{public}d)", httpPort, rtspPort);
  return 0;
}

void OnvifServer::Stop()
{
  if (impl_ == nullptr) {
    return;
  }
  impl_->running.store(false);
  if (impl_->httpFd >= 0) {
    close(impl_->httpFd);
    impl_->httpFd = -1;
  }
  if (impl_->httpThread.joinable()) {
    impl_->httpThread.join();
  }
  if (impl_->discoveryThread.joinable()) {
    impl_->discoveryThread.join();
  }
  delete impl_;
  impl_ = nullptr;
  ON_LOG("stopped");
}

} // namespace osc
