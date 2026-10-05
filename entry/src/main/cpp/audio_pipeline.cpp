#include "audio_pipeline.h"

#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "hilog/log.h"
#include "multimedia/player_framework/native_avbuffer.h"
#include "multimedia/player_framework/native_avcodec_audioencoder.h"
#include "multimedia/player_framework/native_avcodec_base.h"
#include "multimedia/player_framework/native_avformat.h"
#include "ohaudio/native_audiocapturer.h"
#include "multimedia/player_framework/native_avmemory.h"
#include "ohaudio/native_audio_common.h"
#include "ohaudio/native_audiostreambuilder.h"

namespace osc {

static constexpr unsigned int AU_LOG_DOMAIN = 0x0016;
static constexpr const char *AU_LOG_TAG = "AudioPipeline";
#define AU_LOG(fmt, ...)                                                                        \
    OH_LOG_Print(LOG_APP, LOG_INFO, AU_LOG_DOMAIN, AU_LOG_TAG, "%{public}s: " fmt, __func__,    \
        ##__VA_ARGS__)
#define AU_ERR(fmt, ...)                                                                        \
    OH_LOG_Print(LOG_APP, LOG_ERROR, AU_LOG_DOMAIN, AU_LOG_TAG, "%{public}s: " fmt, __func__,   \
        ##__VA_ARGS__)

static constexpr int kSampleRate = 48000;
static constexpr int kChannels = 2; // stereo


struct AudioPipeline::Impl {
  std::atomic<bool> running{false};
  AacSink sink;

  // 采集
  OH_AudioStreamBuilder *builder = nullptr;
  OH_AudioCapturer *capturer = nullptr;

  // PCM 队列（采集回调线程 → 编码线程）
  std::mutex pcmMtx;
  std::deque<std::vector<uint8_t>> pcmQueue;
  uint64_t totalSamples = 0; // 每声道累计采样数
  std::atomic<bool> hasCsd{false};

  // 编码（回调模式）
  OH_AVCodec *encoder = nullptr;
};

static void OnReadData(OH_AudioCapturer *capturer, void *userData, void *audioData, int32_t size)
{
  (void)capturer;
  auto *impl = static_cast<AudioPipeline::Impl *>(userData);
  if (!impl->running.load()) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl->pcmMtx);
  // 上限保护：编码跟不上时丢最旧（约 2 秒）
  size_t queued = 0;
  for (auto &b : impl->pcmQueue) {
    queued += b.size();
  }
  if (queued > static_cast<size_t>(kSampleRate * kChannels * 2 * 2)) {
    impl->pcmQueue.pop_front();
  }
  impl->pcmQueue.emplace_back(static_cast<uint8_t *>(audioData),
      static_cast<uint8_t *>(audioData) + size);
}

// ---------- 编码器回调（回调模式：API 与视频编码器不同，无缓冲轮询） ----------
static void OnAudioEncError(OH_AVCodec *codec, int32_t errorCode, void *userData)
{
    (void)codec;
    auto *impl = static_cast<AudioPipeline::Impl *>(userData);
    AU_ERR("aac encoder error %{public}d", errorCode);
    (void)impl;
}

static void OnAudioEncStreamChanged(OH_AVCodec *codec, OH_AVFormat *format, void *userData)
{
    (void)codec;
    (void)format;
    (void)userData;
}

static void OnAudioNeedInput(OH_AVCodec *codec, uint32_t index, OH_AVMemory *data, void *userData)
{
    auto *impl = static_cast<AudioPipeline::Impl *>(userData);
    std::vector<uint8_t> pcm;
    {
        std::lock_guard<std::mutex> lock(impl->pcmMtx);
        if (!impl->pcmQueue.empty()) {
            pcm = std::move(impl->pcmQueue.front());
            impl->pcmQueue.pop_front();
        }
    }
    OH_AVCodecBufferAttr attr = {};
    if (pcm.empty() || data == nullptr) {
        attr.size = 0;
        OH_AudioEncoder_PushInputData(codec, index, attr);
        return;
    }
    uint8_t *dst = OH_AVMemory_GetAddr(data);
    size_t copy = pcm.size(); // 采集回调块大小由框架分配，天然匹配输入缓冲
    if (dst != nullptr && copy > 0) {
        memcpy(dst, pcm.data(), copy);
    }
    attr.size = static_cast<int32_t>(copy);
    attr.pts = static_cast<int64_t>(impl->totalSamples * 1000000ULL / kSampleRate);
    impl->totalSamples += copy / (kChannels * 2);
    OH_AudioEncoder_PushInputData(codec, index, attr);
}

static void OnAudioNewOutput(OH_AVCodec *codec, uint32_t index, OH_AVMemory *data,
    OH_AVCodecBufferAttr *attr, void *userData)
{
    auto *impl = static_cast<AudioPipeline::Impl *>(userData);
    if (data != nullptr && attr != nullptr) {
        uint8_t *buffer = OH_AVMemory_GetAddr(data);
        if (buffer != nullptr && attr->size > 0 && impl->sink) {
            bool isCsd = (attr->flags & AVCODEC_BUFFER_FLAGS_CODEC_DATA) != 0;
            if (isCsd && !impl->hasCsd.load()) {
                impl->sink(buffer, attr->size, 0, true);
                impl->hasCsd.store(true);
            } else if (!isCsd) {
                if (!impl->hasCsd.load()) {
                    OH_AVFormat *desc = OH_AudioEncoder_GetOutputDescription(codec);
                    if (desc != nullptr) {
                        uint8_t *csd = nullptr;
                        size_t csdSize = 0;
                        if (OH_AVFormat_GetBuffer(desc, OH_MD_KEY_CODEC_CONFIG, &csd, &csdSize) &&
                            csd != nullptr && csdSize > 0) {
                            impl->sink(csd, csdSize, 0, true);
                        }
                        OH_AVFormat_Destroy(desc);
                    }
                    impl->hasCsd.store(true);
                }
                impl->sink(buffer, attr->size, attr->pts, false);
            }
        }
    }
    OH_AudioEncoder_FreeOutputData(codec, index);
}


AudioPipeline &AudioPipeline::Instance()
{
  static AudioPipeline inst;
  return inst;
}

AudioPipeline::~AudioPipeline()
{
  Stop();
}

bool AudioPipeline::IsRunning() const
{
  return impl_ != nullptr && impl_->running.load();
}

bool AudioPipeline::HasCsd() const
{
  return impl_ != nullptr && impl_->hasCsd.load();
}

int AudioPipeline::Start(const AacSink &sink)
{
  if (IsRunning()) {
    return 0;
  }
  if (impl_ == nullptr) {
    impl_ = new Impl();
  }
  impl_->sink = sink;
  impl_->totalSamples = 0;
  impl_->hasCsd.store(false);

  // 1) 采集器
  if (OH_AudioStreamBuilder_Create(&impl_->builder, AUDIOSTREAM_TYPE_CAPTURER) !=
      AUDIOSTREAM_SUCCESS) {
    AU_ERR("builder create failed");
    return -1;
  }
  OH_AudioStreamBuilder_SetCapturerInfo(impl_->builder, AUDIOSTREAM_SOURCE_TYPE_MIC);
  OH_AudioStreamBuilder_SetSamplingRate(impl_->builder, kSampleRate);
  OH_AudioStreamBuilder_SetChannelCount(impl_->builder, kChannels);
  OH_AudioStreamBuilder_SetSampleFormat(impl_->builder, AUDIOSTREAM_SAMPLE_S16LE);
  OH_AudioStreamBuilder_SetEncodingType(impl_->builder, AUDIOSTREAM_ENCODING_TYPE_RAW);
  OH_AudioStreamBuilder_SetCapturerReadDataCallback(impl_->builder, OnReadData, impl_);
  if (OH_AudioStreamBuilder_GenerateCapturer(impl_->builder, &impl_->capturer) !=
      AUDIOSTREAM_SUCCESS) {
    AU_ERR("generate capturer failed");
    return -2;
  }

  // 2) AAC 编码器（缓冲+同步）
  impl_->encoder = OH_AudioEncoder_CreateByMime("audio/mp4a-latm");
  if (impl_->encoder == nullptr) {
    AU_ERR("create aac encoder failed");
    return -3;
  }
  OH_AVFormat *fmt = OH_AVFormat_CreateAudioFormat("audio/mp4a-latm", kSampleRate, kChannels);
  OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_BITRATE, 96000);
  OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_AUDIO_SAMPLE_FORMAT, 0); // S16LE(采样格式枚举 0)
  OH_AVFormat_SetLongValue(fmt, OH_MD_KEY_MAX_INPUT_SIZE, 48000 * 2 * 2 / 2); // 10ms 帧
  OH_AVErrCode cfgCode = OH_AudioEncoder_Configure(impl_->encoder, fmt);
  OH_AVFormat_Destroy(fmt);
  if (cfgCode != AV_ERR_OK) {
    AU_ERR("aac configure failed, code=%{public}d", cfgCode);
    return -4;
  }
  OH_AVFormat_Destroy(fmt);
  OH_AVCodecAsyncCallback cb = {};
  cb.onError = OnAudioEncError;
  cb.onStreamChanged = OnAudioEncStreamChanged;
  cb.onNeedInputData = OnAudioNeedInput;
  cb.onNeedOutputData = OnAudioNewOutput;
  if (OH_AudioEncoder_SetCallback(impl_->encoder, cb, impl_) != AV_ERR_OK) {
    AU_ERR("aac set callback failed");
    return -5;
  }
  if (OH_AudioEncoder_Start(impl_->encoder) != AV_ERR_OK) {
    AU_ERR("aac start failed");
    return -6;
  }

  impl_->running.store(true);
  OH_AudioCapturer_Start(impl_->capturer);
  AU_LOG("started: %{public}Hz %{public}ch AAC", kSampleRate, kChannels);
  return 0;
}



void AudioPipeline::Stop()
{
  if (impl_ == nullptr) {
    return;
  }
  if (impl_->running.load()) {
    impl_->running.store(false);
    if (impl_->capturer != nullptr) {
      OH_AudioCapturer_Stop(impl_->capturer);
      OH_AudioCapturer_Release(impl_->capturer);
      impl_->capturer = nullptr;
    }
    if (impl_->encoder != nullptr) {
      OH_AudioEncoder_Stop(impl_->encoder);
      OH_AudioEncoder_Destroy(impl_->encoder);
      impl_->encoder = nullptr;
    }
    if (impl_->builder != nullptr) {
      OH_AudioStreamBuilder_Destroy(impl_->builder);
      impl_->builder = nullptr;
    }
    AU_LOG("stopped");
  }
}

} // namespace osc
