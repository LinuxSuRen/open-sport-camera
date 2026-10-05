/**
 * ONVIF 服务（阶段3扩展）：让手机成为可自动发现的标准 IP 摄像机。
 *
 * 能力子集：
 * - WS-Discovery：UDP 组播 239.255.255.250:3702，应答 Probe → ProbeMatch
 * - 设备服务（SOAP/HTTP）：GetSystemDateAndTime / GetDeviceInformation /
 *   GetCapabilities / GetServices
 * - 媒体服务：GetProfiles / GetStreamUri（返回 RTSP 推流地址）
 *
 * NVR / VLC / ONVIF Device Manager 可自动发现并拿到 rtsp://<ip>:8554/live
 */
#ifndef OSC_ONVIF_SERVER_H
#define OSC_ONVIF_SERVER_H

#include <atomic>
#include <cstdint>

namespace osc {

class OnvifServer {
public:
  static OnvifServer &Instance();

  /** 启动（httpPort ONVIF SOAP 端口；rtspPort 用于 GetStreamUri 返回值） */
  int Start(int httpPort, int rtspPort);

  void Stop();

  bool IsRunning() const;

private:
  OnvifServer() = default;
  ~OnvifServer();
  struct Impl;
  Impl *impl_ = nullptr;
};

} // namespace osc

#endif // OSC_ONVIF_SERVER_H
