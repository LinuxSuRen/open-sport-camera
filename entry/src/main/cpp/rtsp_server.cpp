#include "rtsp_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "hilog/log.h"
#include "codec_common.h"

namespace osc {

static constexpr unsigned int RT_LOG_DOMAIN = 0x0014;
static constexpr const char *RT_LOG_TAG = "RtspServer";
#define RT_LOG(fmt, ...)                                                                        \
    OH_LOG_Print(LOG_APP, LOG_INFO, RT_LOG_DOMAIN, RT_LOG_TAG, "%{public}s: " fmt, __func__,    \
        ##__VA_ARGS__)
#define RT_ERR(fmt, ...)                                                                        \
    OH_LOG_Print(LOG_APP, LOG_ERROR, RT_LOG_DOMAIN, RT_LOG_TAG, "%{public}s: " fmt, __func__,   \
        ##__VA_ARGS__)

// ---------- Base64（sprop-parameter-sets 用） ----------
static std::string B64Encode(const uint8_t *data, size_t len)
{
  static const char *table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((len + 2) / 3 * 4);
  size_t i = 0;
  while (i + 2 < len) {
    uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out += table[(v >> 18) & 0x3F];
    out += table[(v >> 12) & 0x3F];
    out += table[(v >> 6) & 0x3F];
    out += table[v & 0x3F];
    i += 3;
  }
  if (i + 1 == len) {
    uint32_t v = data[i] << 16;
    out += table[(v >> 18) & 0x3F];
    out += table[(v >> 12) & 0x3F];
    out += "==";
  } else if (i + 2 == len) {
    uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
    out += table[(v >> 18) & 0x3F];
    out += table[(v >> 12) & 0x3F];
    out += table[(v >> 6) & 0x3F];
    out += '=';
  }
  return out;
}

// ---------- NAL 切分（annex-B 与 AVCC 长度前缀都支持） ----------
struct NalView {
  const uint8_t *data;
  size_t size;
};

static bool IsAnnexB(const uint8_t *buf, size_t size)
{
  for (size_t i = 0; i + 3 < size && i < 16; i++) {
    if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
      return true;
    }
  }
  return false;
}

static std::vector<NalView> SplitNals(const uint8_t *buf, size_t size)
{
  std::vector<NalView> nals;
  if (size < 5) {
    return nals;
  }
  if (IsAnnexB(buf, size)) {
    size_t i = 0;
    while (i + 3 < size) {
      size_t scLen = 0;
      if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
        scLen = 3;
      } else if (i + 4 < size && buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1) {
        scLen = 4;
      }
      if (scLen == 0) {
        i++;
        continue;
      }
      size_t start = i + scLen;
      size_t j = start + 1;
      while (j + 3 < size) {
        if (buf[j] == 0 && buf[j + 1] == 0 && buf[j + 2] == 1) {
          break;
        }
        if (buf[j] == 0 && buf[j + 1] == 0 && buf[j + 2] == 0 && buf[j + 3] == 1) {
          break;
        }
        j++;
      }
      size_t end = (j + 3 >= size) ? size : j;
      if (end > start) {
        nals.push_back({buf + start, end - start});
      }
      i = end;
    }
    return nals;
  }
  // AVCC
  size_t i = 0;
  while (i + 4 < size) {
    uint32_t len = (static_cast<uint32_t>(buf[i]) << 24) | (static_cast<uint32_t>(buf[i + 1]) << 16) |
      (static_cast<uint32_t>(buf[i + 2]) << 8) | static_cast<uint32_t>(buf[i + 3]);
    if (len == 0 || i + 4 + len > size) {
      break;
    }
    nals.push_back({buf + i + 4, len});
    i += 4 + len;
  }
  return nals;
}

// SPS/PPS 持久缓存（跨服务器启停保留，DESCRIBE 不再因冷缓存 503）
static std::mutex gCsdMtx;
static std::vector<uint8_t> gSps;
static std::vector<uint8_t> gPps;

struct RtspServer::Impl {
  std::atomic<bool> running{false};
  int listenFd = -1;
  int port = 8554;
  std::thread acceptThread;

  // 会话（单客户端 MVP）
  std::mutex sessionMtx;
  int clientFd = -1;
  bool playing = false;
  bool needKey = true; // 每客户端起流等关键帧（PLAY 时置位）
  uint64_t sentPackets = 0;
  uint64_t sentBytes = 0;
  uint16_t seq = 0;
  uint32_t ssrc = 0x4F534300; // "OSC\0"
  uint32_t videoClockRate = 90000;

  // 流描述（SPS/PPS 缓存，建 SDP 与起流用）
  std::mutex csdMtx;
  std::vector<uint8_t> sps;
  std::vector<uint8_t> pps;

  void AcceptLoop();
  void HandleClient(int fd);
  void SendRtpNal(const uint8_t *nal, size_t size, int64_t ptsUs, bool marker);
  bool WriteAll(int fd, const void *buf, size_t len);
};

RtspServer &RtspServer::Instance()
{
  static RtspServer inst;
  return inst;
}

RtspServer::~RtspServer()
{
  Stop();
}

bool RtspServer::IsRunning() const
{
  return impl_ != nullptr && impl_->running.load();
}

int RtspServer::GetPort() const
{
  return impl_ != nullptr ? impl_->port : 0;
}

bool RtspServer::HasClient() const
{
  if (impl_ == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->sessionMtx);
  return impl_->playing;
}

int RtspServer::Start(int port)
{
  if (IsRunning()) {
    return 0;
  }
  if (impl_ == nullptr) {
    impl_ = new Impl();
  }
  impl_->port = port;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    RT_ERR("socket failed");
    return -1;
  }
  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    RT_ERR("bind :%{public}d failed", port);
    close(fd);
    return -2;
  }
  if (listen(fd, 2) != 0) {
    RT_ERR("listen failed");
    close(fd);
    return -3;
  }
  impl_->listenFd = fd;
  impl_->running.store(true);
  impl_->acceptThread = std::thread(&Impl::AcceptLoop, impl_);
  RT_LOG("listening on :%{public}d", port);
  return 0;
}

void RtspServer::Stop()
{
  if (impl_ == nullptr) {
    return;
  }
  impl_->running.store(false);
  if (impl_->listenFd >= 0) {
    close(impl_->listenFd);
    impl_->listenFd = -1;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->sessionMtx);
    if (impl_->clientFd >= 0) {
      close(impl_->clientFd);
      impl_->clientFd = -1;
    }
    impl_->playing = false;
  }
  if (impl_->acceptThread.joinable()) {
    impl_->acceptThread.join();
  }
  delete impl_;
  impl_ = nullptr;
  RT_LOG("stopped");
}

bool RtspServer::Impl::WriteAll(int fd, const void *buf, size_t len)
{
  const uint8_t *p = static_cast<const uint8_t *>(buf);
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

// interleaved RTP：'$' + channel(1B) + length(2B) + payload
void RtspServer::Impl::SendRtpNal(const uint8_t *nal, size_t size, int64_t ptsUs, bool marker)
{
  if (clientFd < 0 || !playing) {
    return;
  }
  const size_t kMaxPacket = 1400; // MTU 安全值
  // 用系统单调时钟保持 RTP 时间戳连续稳定（ptsUs 在录制重启时会重置）
  static int64_t baseNs = -1;
  int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  if (baseNs < 0) {
    baseNs = nowNs;
  }
  uint32_t ts = static_cast<uint32_t>(((nowNs - baseNs) / 1000) * videoClockRate / 1000000);

  auto sendPacket = [&](const uint8_t *payload, size_t len, uint8_t nriType, bool last) {
    uint8_t header[12] = {0};
    header[0] = 0x80;
    (void)nriType;
    // RTP payload type 必须是 SDP 声明的动态类型 96（曾误填 NAL 类型导致解码端拒收）
    header[1] = 96 | (last && marker ? 0x80 : 0x00);
    header[2] = static_cast<uint8_t>((seq >> 8) & 0xFF);
    header[3] = static_cast<uint8_t>(seq & 0xFF);
    header[4] = static_cast<uint8_t>((ts >> 24) & 0xFF);
    header[5] = static_cast<uint8_t>((ts >> 16) & 0xFF);
    header[6] = static_cast<uint8_t>((ts >> 8) & 0xFF);
    header[7] = static_cast<uint8_t>(ts & 0xFF);
    header[8] = static_cast<uint8_t>((ssrc >> 24) & 0xFF);
    header[9] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
    header[10] = static_cast<uint8_t>((ssrc >> 8) & 0xFF);
    header[11] = static_cast<uint8_t>(ssrc & 0xFF);
    seq++;
    sentPackets++;
    sentBytes += len + 16;

    uint8_t frame[4] = {0x24, 0x00, static_cast<uint8_t>((len + 12) >> 8),
      static_cast<uint8_t>((len + 12) & 0xFF)};
    if (!WriteAll(clientFd, frame, 4) || !WriteAll(clientFd, header, 12) ||
        !WriteAll(clientFd, payload, len)) {
      playing = false;
      needKey = true;
      RT_LOG("client write timeout, disconnect (sent %{public}llu pkts %{public}llu bytes)",
        static_cast<unsigned long long>(sentPackets),
        static_cast<unsigned long long>(sentBytes));
      return; // 直接返回，由 AcceptLoop 关闭
    }
  };

  if (size <= kMaxPacket) {
    sendPacket(nal, size, 0, true);
    return;
  }
  // FU-A 分片
  uint8_t nri = nal[0] & 0x60;
  uint8_t type = nal[0] & 0x1F;
  size_t offset = 1;
  bool first = true;
  while (offset < size) {
    size_t chunk = size - offset;
    if (chunk > kMaxPacket - 2) {
      chunk = kMaxPacket - 2;
    }
    uint8_t fu[1400];
    fu[0] = nri | 28; // FU-A
    uint8_t fuHdr = static_cast<uint8_t>(type & 0x1F);
    if (first) {
      fuHdr |= 0x80;
    }
    bool last = (offset + chunk >= size);
    if (last) {
      fuHdr |= 0x40;
    }
    fu[1] = fuHdr;
    memcpy(fu + 2, nal + offset, chunk);
    sendPacket(fu, chunk + 2, 0, last);
    offset += chunk;
    first = false;
  }
}

void RtspServer::OnFrame(const uint8_t *data, size_t size, int64_t ptsUs, bool isKey)
{
  if (impl_ == nullptr || !impl_->running.load()) {
    return;
  }
  auto nals = SplitNals(data, size);
  if (nals.empty()) {
    return;
  }
  // 缓存 SPS/PPS（持久）
  {
    std::lock_guard<std::mutex> lock(gCsdMtx);
    for (const auto &n : nals) {
      if (n.size >= 2) {
        int t = n.data[0] & 0x1F;
        if (t == 7 && gSps.empty()) {
          gSps.assign(n.data, n.data + n.size);
        } else if (t == 8 && gPps.empty()) {
          gPps.assign(n.data, n.data + n.size);
        }
      }
    }
  }
  std::lock_guard<std::mutex> lock(impl_->sessionMtx);
  if (!impl_->playing || impl_->clientFd < 0) {
    return;
  }
  // 每客户端起流：等待 IDR；命中后先发 SPS/PPS 再发 IDR
  bool frameHasKey = false;
  for (const auto &n : nals) {
    if (n.size >= 2 && (n.data[0] & 0x1F) == 5) {
      frameHasKey = true;
      break;
    }
  }
  if (impl_->needKey) {
    if (!frameHasKey) {
      return; // 等待关键帧
    }
    impl_->needKey = false;
    {
      std::lock_guard<std::mutex> csdLock(gCsdMtx);
      if (!gSps.empty()) {
        impl_->SendRtpNal(gSps.data(), gSps.size(), ptsUs, false);
      }
      if (!gPps.empty()) {
        impl_->SendRtpNal(gPps.data(), gPps.size(), ptsUs, false);
      }
    }
  }
  for (size_t i = 0; i < nals.size(); i++) {
    bool last = (i == nals.size() - 1);
    impl_->SendRtpNal(nals[i].data, nals[i].size, ptsUs, last);
  }
  if (impl_->sentPackets % 300 == 0 && impl_->sentPackets > 0) {
    RT_LOG("streamed %{public}llu pkts %{public}llu bytes",
      static_cast<unsigned long long>(impl_->sentPackets),
      static_cast<unsigned long long>(impl_->sentBytes));
  }
  if (!impl_->playing) {
    impl_->needKey = true; // 客户端断开后重置
  }
}

void RtspServer::Impl::HandleClient(int fd)
{
  // TCP nodelay
  int nodelay = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

  char buf[4096];
  std::string request;
  std::string sdp;
  uint32_t session = 0;
  while (running.load()) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      break;
    }
    request.append(buf, static_cast<size_t>(n));
    size_t pos = request.find("\r\n\r\n");
    if (pos == std::string::npos) {
      continue;
    }
    std::string msg = request.substr(0, pos + 4);
    request.erase(0, pos + 4);

    // 解析方法与 CSeq
    std::string method = msg.substr(0, msg.find(' '));
    std::string cseq;
    size_t cseqPos = msg.find("CSeq:");
    if (cseqPos == std::string::npos) {
      cseqPos = msg.find("cseq:");
    }
    if (cseqPos != std::string::npos) {
      size_t vStart = msg.find_first_not_of(' ', cseqPos + 5);
      size_t vEnd = msg.find("\r\n", vStart);
      if (vStart != std::string::npos && vEnd != std::string::npos) {
        cseq = msg.substr(vStart, vEnd - vStart);
      }
    }
    std::string common = "CSeq: " + cseq + "\r\n";

    if (method == "OPTIONS") {
      std::string resp = "RTSP/1.0 200 OK\r\n" + common +
        "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n\r\n";
      WriteAll(fd, resp.data(), resp.size());
    } else if (method == "DESCRIBE") {
      std::lock_guard<std::mutex> lock(gCsdMtx);
      const auto &sps = gSps;
      const auto &pps = gPps;
      if (sps.empty() || pps.empty()) {
        std::string resp = "RTSP/1.0 503 Service Unavailable\r\n" + common + "\r\n";
        WriteAll(fd, resp.data(), resp.size());
        continue;
      }
      std::string profile = "64"; // baseline 默认，从 SPS 提取更准
      if (sps.size() > 4) {
        char p[8];
        snprintf(p, sizeof(p), "%02X%02X%02X", sps[1], sps[2], sps[3]);
        profile = p;
      }
      sdp = "v=0\r\n"
            "o=- 0 0 IN IP4 0.0.0.0\r\n"
            "s=OpenSportCamera\r\n"
            "t=0 0\r\n"
            "m=video 0 RTP/AVP 96\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "a=rtpmap:96 H264/90000\r\n"
            "a=fmtp:96 packetization-mode=1;profile-level-id=" + profile +
            ";sprop-parameter-sets=" + B64Encode(sps.data(), sps.size()) + "," +
            B64Encode(pps.data(), pps.size()) + "\r\n"
            "a=control:track0\r\n";
      std::string body = sdp;
      std::string resp = "RTSP/1.0 200 OK\r\n" + common +
        "Content-Type: application/sdp\r\n" +
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
      WriteAll(fd, resp.data(), resp.size());
    } else if (method == "SETUP") {
      session = 0x12345678;
      std::string resp = "RTSP/1.0 200 OK\r\n" + common +
        "Session: " + std::to_string(session) + "\r\n" +
        "Transport: RTP/AVP/TCP;interleaved=0-1\r\n\r\n";
      WriteAll(fd, resp.data(), resp.size());
      {
        std::lock_guard<std::mutex> lock(sessionMtx);
        clientFd = fd;
        playing = false;
        needKey = true;
        seq = 0;
      }
    } else if (method == "PLAY") {
      {
        std::lock_guard<std::mutex> lock(sessionMtx);
        playing = true;
        needKey = true; // 新起流：从关键帧开始
      }
      std::string resp = "RTSP/1.0 200 OK\r\n" + common +
        "Session: " + std::to_string(session) + "\r\n" +
        "RTP-Info: url=rtsp://localhost:8554/live/track0;seq=0\r\n\r\n";
      WriteAll(fd, resp.data(), resp.size());
      RT_LOG("client playing");
    } else if (method == "TEARDOWN") {
      std::string resp = "RTSP/1.0 200 OK\r\n" + common +
        "Session: " + std::to_string(session) + "\r\n\r\n";
      WriteAll(fd, resp.data(), resp.size());
      break;
    }
  }
  {
    std::lock_guard<std::mutex> lock(sessionMtx);
    if (clientFd == fd) {
      clientFd = -1;
      playing = false;
    }
  }
  close(fd);
  RT_LOG("client closed");
}

void RtspServer::Impl::AcceptLoop()
{
  while (running.load()) {
    sockaddr_in peer = {};
    socklen_t len = sizeof(peer);
    int fd = accept(listenFd, reinterpret_cast<sockaddr *>(&peer), &len);
    if (fd < 0) {
      if (!running.load()) {
        break;
      }
      continue;
    }
    char ip[32] = {0};
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    RT_LOG("client from %{public}s", ip);
    {
      std::lock_guard<std::mutex> lock(sessionMtx);
      if (clientFd >= 0) {
        close(clientFd); // MVP：新客户端顶替旧连接
        clientFd = -1;
        playing = false;
      }
    }
    HandleClient(fd);
  }
}

} // namespace osc
