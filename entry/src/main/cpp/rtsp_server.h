/**
 * RTSP 服务器（阶段3）：把实时管线的 H.264 流以 RTSP/RTP 发布到局域网。
 *
 * 设计：
 * - 单客户端 MVP：TCP 监听（默认 8554），interleaved RTP over TCP（免 UDP 打洞，VLC 默认支持）
 * - RTSP 子集：OPTIONS / DESCRIBE / SETUP / PLAY / TEARDOWN
 * - H.264 打包：RFC 6184（单 NAL 直发 + FU-A 分片），90kHz 时钟
 * - 起流时机：新客户端从下一个关键帧开始（缓存 SPS/PPS，等待 IDR）
 * - 帧源：RecordStream 编码输出回调 OnFrame(data, size, ptsUs, flags)
 *   （数据可能是 annex-B 或 AVCC 长度前缀，NAL 切分器两种都认）
 */
#ifndef OSC_RTSP_SERVER_H
#define OSC_RTSP_SERVER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace osc {

class RtspServer {
public:
  // 编码帧回调：data/size 编码缓冲，ptsUs 相对时间戳，isKey 关键帧
  using FrameSink = std::function<void(const uint8_t *data, size_t size, int64_t ptsUs, bool isKey)>;

  static RtspServer &Instance();

  /** 启动监听（幂等）；返回 0 成功 */
  int Start(int port);

  void Stop();

  bool IsRunning() const;

  int GetPort() const;

  /** 是否有客户端在拉流 */
  bool HasClient() const;

  /** 管线帧入口（编码输出回调侧调用；内部异步分发到会话） */
  void OnFrame(const uint8_t *data, size_t size, int64_t ptsUs, bool isKey);

 private:
  RtspServer() = default;
  ~RtspServer();
  struct Impl;
  Impl *impl_ = nullptr;
};

} // namespace osc

#endif // OSC_RTSP_SERVER_H
