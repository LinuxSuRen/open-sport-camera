# Open Sport Camera

[![build](https://github.com/linuxsuren/open-sport-camera/actions/workflows/build.yml/badge.svg)](https://github.com/linuxsuren/open-sport-camera/actions/workflows/build.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

开源运动相机 App（HarmonyOS / ArkTS），优先服务于游泳视频拍摄。

## 核心特性（0.2.0）

- **实时录制管线**：相机流开机常挂（拓扑永不变，根治预览冻结/录制花屏）→ GL 合成 → 硬编码，**停止即出片零等待**
- **水印实时入流**：游泳计时（百分秒/计圈）+ 文本水印边录边写，所见即所得
- **RTSP 推流**：局域网实时观看（VLC/ffplay 打开 `rtsp://<手机IP>:8554/live`）
- **ONVIF 自动发现**：WS-Discovery + GetStreamUri，NVR/发现工具直取推流地址，无需手输 IP
- **多摄支持**：主摄/超广/长焦识别与一键切换（排除深度摄像头）
- **EIS 防抖**：设备支持时自动启用

## 工程结构

```
entry/src/main/ets/
  camera/CameraService.ets    相机会话/多摄镜头/EIS/常挂视频流
  record/StreamRecorderService.ets  实时管线调度（四段式）
  record/RtspService.ets      RTSP + ONVIF 推流控制
  timer/SwimTimer.ets         游泳秒表（录制联动/计圈）
  watermark/                  水印模型、预览叠加、字形栅格化、设置面板
  gallery/GalleryService.ets  相册保存（SaveButton 手势）
  pages/Index.ets             拍摄主页
entry/src/main/cpp/
  record_stream.cpp           实时管线（GL 合成 + 硬编码 + MP4）
  rtsp_server.cpp             RTSP/RTP 服务器
  onvif_server.cpp            WS-Discovery + SOAP 设备/媒体服务
  burn_engine.cpp             录后烧录引擎（旧管线兼容保留）
  codec_common.cpp            CSD 解析/avcC 组装
```

## 构建

依赖 HarmonyOS Command Line Tools（hvigorw / ohpm / hdc），从[华为开发者下载中心](https://developer.huawei.com/consumer/cn/download/)安装：

```bash
ohpm install
hvigorw assembleHap --mode module -p product=default --no-daemon
```

CI 每次推送自动构建未签名 HAP（见 [build.yml](.github/workflows/build.yml)）。真机安装需要调试签名，流程见[贡献指南](CONTRIBUTING.md)。

## 常见问题

开发与真机调试的常见问题见 [FAQ.md](FAQ.md)。

## 参与贡献

欢迎 Issue / PR：水印类型扩展、防抖增强、录制参数、UI 打磨都是好方向。
开发环境与规范见 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 许可

[Apache-2.0](LICENSE)
