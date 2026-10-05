/**
 * AAC 音频管线：麦克风 PCM（回调采集）→ AAC 硬编码（缓冲+同步）→ muxer。
 * 与 RecordStream 视频轨并行，PTS 按采样数推算。
 */
#ifndef OSC_AUDIO_PIPELINE_H
#define OSC_AUDIO_PIPELINE_H

#include <atomic>
#include <cstdint>
#include <functional>

namespace osc {

class AudioPipeline {
public:
  // AAC 帧：(数据, 大小, ptsUs, 是否 CSD)
  using AacSink = std::function<void(const uint8_t *data, size_t size, int64_t ptsUs, bool isCsd)>;

  static AudioPipeline &Instance();

  /** 启动采集+编码；sink 在编码线程回调 */
  int Start(const AacSink &sink);

  void Stop();

  bool IsRunning() const;

  // CSD（AudioSpecificConfig）状态查询：muxer 建轨用
  bool HasCsd() const;

  struct Impl;
private:
  AudioPipeline() = default;
  ~AudioPipeline();
  Impl *impl_ = nullptr;
};

} // namespace osc

#endif // OSC_AUDIO_PIPELINE_H
