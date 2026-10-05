/**
 * 实时录制管线：相机帧 → GL（OES 纹理）→ 硬编码（surface 模式）→ MP4。
 *
 * 设计要点（对应真机踩坑结论）：
 * - 视频流在相机会话创建时永久挂载（本模块返回 surfaceId 给 ArkTS 建 VideoOutput），
 *   录制启停只控制编码器与渲染循环，相机拓扑永不变 → 根治动态重配引发的
 *   预览冻结与录制花屏
 * - 帧节奏由相机驱动：onFrameAvailable 回调驱动渲染，PTS 取相机帧时间戳
 *   （OH_NativeImage_GetTimestamp, ns）经 eglPresentationTimeANDROID 透传编码器
 * - 编码器 surface 模式（回调接口）：onNeedInputParameter 必须 PushInputParameter；
 *   输出经 onNewOutputBuffer 收取，CSD 用 avcC 组装（codec_common）
 * - 阶段2 水印：SetWatermarkTexture 预留（GL 合成位）
 */
#ifndef OSC_RECORD_STREAM_H
#define OSC_RECORD_STREAM_H

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace osc {

struct RecordStats {
  int64_t durationMs = 0;
  uint64_t frames = 0;
  int code = 0;
};

class RecordStream {
public:
  /**
   * 准备管线：创建 GL/OES/NativeImage/编码器（不启动编码）。
   * 返回相机侧 VideoOutput 应绑定的 surfaceId。
   * 相机会话应在本阶段就把视频流挂载好——之后拓扑永不变。
   */
  int Prepare(int width, int height, int fps, int bitrate, int rotation,
    uint64_t &cameraSurfaceId);

  /** 开始录制：启动编码器与渲染循环，输出到 outPath */
  int Begin(const std::string &outPath);

  /** 停止编码管线（推流和录制都结束时调用） */
  RecordStats Stop();

  /** 挂载 muxer 开始录文件（编码管线须已在运行，推流不受影响） */
  int StartRecording(const std::string &outPath);

  /** 卸载 muxer 停止录文件（编码管线继续运行，推流不断） */
  RecordStats StopRecording();

  /** 释放全部资源（相机关闭/退出时调用） */
  void Release();

  bool IsPrepared() const;
  bool IsRecording() const;

  /**
   * 阶段2：设置水印资产（录制前调用）。
   * cfgJson 与烧录管线同构（glyphs/statics/timer），buffers 为 RGBA 缓冲，
   * 调用时拷贝（JS 侧无需保活）。
   */
  int SetWatermarkAssets(const std::string &cfgJson, const std::vector<const uint8_t *> &buffers);

  /** 录制中更新计圈（计圈点击时调用） */
  int UpdateLaps(const std::string &lapsJson);

  /** 运行时设置水平镜像（切换前后摄像头时调用） */
  void SetFlipX(bool flip);

  ~RecordStream();

  struct Impl;
private:
  Impl *impl_ = nullptr;
};

} // namespace osc

#endif // OSC_RECORD_STREAM_H
