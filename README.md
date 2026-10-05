# Open Sport Camera

[![build](https://github.com/linuxsuren/open-sport-camera/actions/workflows/build.yml/badge.svg)](https://github.com/linuxsuren/open-sport-camera/actions/workflows/build.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

开源运动相机 App（HarmonyOS / ArkTS），优先服务于游泳视频拍摄。

## 核心特性（MVP）

- **自定义相机录制**：XComponent 预览 + AVRecorder 录制（H.264 1080p），启用相机 EIS 防抖
- **游泳计时水印**：录制联动秒表（10ms 精度、计圈），实时叠加在预览上，录制后烧录进视频文件
- **可扩展水印系统**：统一 `WatermarkItem` 模型（类型/位置/大小/样式），内置计时与文本水印，后续可扩展日期、Logo、心率等
- **NDK 烧录引擎**：录制完成后，解码 → 水印合成 → 硬编码，把水印真正写进视频文件，并保存到系统相册

## 工程结构

```
AppScope/                     应用级配置与资源
entry/src/main/ets/
  camera/CameraService.ets    相机会话、预览流、EIS 防抖选择
  record/RecorderService.ets  AVRecorder 录制封装
  timer/SwimTimer.ets         游泳秒表核心（开始/暂停/计圈/重置）
  watermark/                  水印模型、渲染组件、持久化
  burn/BurnService.ets        烧录任务调度（调 native）
  gallery/GalleryService.ets  相册保存
  pages/                      相机主页、水印设置
entry/src/main/cpp/           NDK 烧录引擎（avcodec 缓冲区模式管线）
```

## 构建

依赖 HarmonyOS Command Line Tools（hvigorw / ohpm / hdc），从[华为开发者下载中心](https://developer.huawei.com/consumer/cn/download/)安装：

```bash
ohpm install
hvigorw assembleHap --mode module -p product=default --no-daemon
```

CI 每次推送自动构建未签名 HAP（见 [build.yml](.github/workflows/build.yml)）。真机安装需要调试签名，流程见[贡献指南](CONTRIBUTING.md)。

## 参与贡献

欢迎 Issue / PR：水印类型扩展、防抖增强、录制参数、UI 打磨都是好方向。
开发环境与规范见 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 许可

[Apache-2.0](LICENSE)
