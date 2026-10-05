#include "burn_engine.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cstring>
#include <vector>

#include "hilog/log.h"

#include "blitter.h"
#include "multimedia/player_framework/native_avbuffer.h"
#include "multimedia/player_framework/native_avcodec_base.h"
#include "multimedia/player_framework/native_avcodec_videodecoder.h"
#include "multimedia/player_framework/native_avcodec_videoencoder.h"
#include "multimedia/player_framework/native_avdemuxer.h"
#include "multimedia/player_framework/native_avformat.h"
#include "multimedia/player_framework/native_avmuxer.h"
#include "multimedia/player_framework/native_avsource.h"

namespace osc {

// 轮询超时：足够长以容忍编解码器内部排队，又不至于卡死
static constexpr int64_t POLL_TIMEOUT_US = 3000000; // 3s

// 引擎日志域（真机排查烧录链路主要手段）
static constexpr unsigned int OSC_LOG_DOMAIN = 0x0012;
static constexpr const char *OSC_LOG_TAG = "BurnEngine";

#define MEDIA_LOG_INFO(fmt, ...)                                                               \
    OH_LOG_Print(LOG_APP, LOG_INFO, OSC_LOG_DOMAIN, OSC_LOG_TAG, "%{public}s: " fmt, __func__, ##__VA_ARGS__)
#define MEDIA_LOG_ERROR(fmt, ...)                                                              \
    OH_LOG_Print(LOG_APP, LOG_ERROR, OSC_LOG_DOMAIN, OSC_LOG_TAG, "%{public}s: " fmt, __func__, ##__VA_ARGS__)

// 失败即记日志并跳出主 do-while，进入统一清理段
#define OSC_CHECK(expr)                                                        \
    if (true) {                                                                \
        OH_AVErrCode _c = (expr);                                              \
        if (_c != AV_ERR_OK) {                                                 \
            MEDIA_LOG_ERROR("call " #expr " failed, code=%{public}d", _c);     \
            ret = -20;                                                         \
            break;                                                             \
        }                                                                      \
    } else                                                                     \
        (void)0

void BurnEngine::ReportProgress(const ProgressFn &cb, int pct)
{
    if (cb) {
        cb(pct);
    }
}

int BurnEngine::Run(const std::string &srcPath, const std::string &dstPath,
    const BurnCfg &cfg, const ProgressFn &onProgress)
{
    int ret = 0;
    int srcFd = -1;
    int dstFd = -1;
    OH_AVSource *source = nullptr;
    OH_AVDemuxer *demuxer = nullptr;
    OH_AVCodec *decoder = nullptr;
    OH_AVCodec *encoder = nullptr;
    OH_AVMuxer *muxer = nullptr;
    OH_AVBuffer *videoInBuf = nullptr;
    OH_AVBuffer *audioInBuf = nullptr;

    ReportProgress(onProgress, 0);

    do {
        // ---------- 打开源 ----------
        srcFd = open(srcPath.c_str(), O_RDONLY);
        if (srcFd < 0) {
            ret = -1;
            break;
        }
        struct stat st = {};
        if (fstat(srcFd, &st) != 0 || st.st_size <= 0) {
            ret = -2;
            break;
        }
        source = OH_AVSource_CreateWithFD(srcFd, 0, st.st_size);
        if (source == nullptr) {
            MEDIA_LOG_ERROR("create source failed");
            ret = -3;
            break;
        }

        // 扫描轨道：确定视频/音频轨与参数
        int32_t videoTrackIdx = -1;
        int32_t audioTrackIdx = -1;
        std::string videoMime;
        int32_t videoW = cfg.videoWidth;
        int32_t videoH = cfg.videoHeight;
        int32_t videoMaxInput = 0;
        int64_t durationUs = -1;
        {
            OH_AVFormat *srcFmt = OH_AVSource_GetSourceFormat(source);
            int32_t trackCount = 0;
            if (srcFmt == nullptr || !OH_AVFormat_GetIntValue(srcFmt, OH_MD_KEY_TRACK_COUNT, &trackCount)) {
                ret = -4;
                if (srcFmt != nullptr) {
                    OH_AVFormat_Destroy(srcFmt);
                }
                break;
            }
            int32_t durMs = 0;
            if (OH_AVFormat_GetIntValue(srcFmt, OH_MD_KEY_DURATION, &durMs) && durMs > 0) {
                durationUs = static_cast<int64_t>(durMs) * 1000;
            }
            for (int32_t i = 0; i < trackCount; i++) {
                OH_AVFormat *tf = OH_AVSource_GetTrackFormat(source, static_cast<uint32_t>(i));
                if (tf == nullptr) {
                    continue;
                }
                const char *mime = nullptr;
                if (OH_AVFormat_GetStringValue(tf, OH_MD_KEY_CODEC_MIME, &mime) && mime != nullptr) {
                    std::string m = mime;
                    if (m.rfind("video/", 0) == 0 && videoTrackIdx < 0) {
                        videoTrackIdx = i;
                        videoMime = m;
                        OH_AVFormat_GetIntValue(tf, OH_MD_KEY_WIDTH, &videoW);
                        OH_AVFormat_GetIntValue(tf, OH_MD_KEY_HEIGHT, &videoH);
                        OH_AVFormat_GetIntValue(tf, OH_MD_KEY_MAX_INPUT_SIZE, &videoMaxInput);
                    } else if (m.rfind("audio/", 0) == 0 && audioTrackIdx < 0) {
                        audioTrackIdx = i;
                    }
                }
                OH_AVFormat_Destroy(tf);
            }
            OH_AVFormat_Destroy(srcFmt);
        }
        if (videoTrackIdx < 0) {
            MEDIA_LOG_ERROR("no video track found");
            ret = -5;
            break;
        }
        MEDIA_LOG_INFO("tracks: video=%{public}d audio=%{public}d %{public}dx%{public}d mime=%{public}s",
            videoTrackIdx, audioTrackIdx, videoW, videoH, videoMime.c_str());
        if (videoW <= 0 || videoH <= 0) {
            ret = -6;
            break;
        }

        demuxer = OH_AVDemuxer_CreateWithSource(source);
        if (demuxer == nullptr) {
            ret = -7;
            break;
        }
        if (OH_AVDemuxer_SelectTrackByID(demuxer, static_cast<uint32_t>(videoTrackIdx)) != AV_ERR_OK) {
            ret = -8;
            break;
        }
        if (audioTrackIdx >= 0) {
            OH_AVDemuxer_SelectTrackByID(demuxer, static_cast<uint32_t>(audioTrackIdx));
        }

        if (videoMaxInput < static_cast<int32_t>(videoW * videoH)) {
            videoMaxInput = videoW * videoH; // AVC 码流单帧上限经验值
        }
        videoInBuf = OH_AVBuffer_Create(videoMaxInput);
        if (videoInBuf == nullptr) {
            ret = -9;
            break;
        }

        // ---------- 解码器 ----------
        decoder = OH_VideoDecoder_CreateByMime(videoMime.c_str());
        if (decoder == nullptr) {
            ret = -10;
            break;
        }
        {
            OH_AVFormat *decFmt = OH_AVFormat_Create();
            OH_AVFormat_SetIntValue(decFmt, OH_MD_KEY_WIDTH, videoW);
            OH_AVFormat_SetIntValue(decFmt, OH_MD_KEY_HEIGHT, videoH);
            OH_AVFormat_SetIntValue(decFmt, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
            // 轮询式 Query 接口要求开启同步模式（API20+）
            OH_AVFormat_SetIntValue(decFmt, OH_MD_KEY_ENABLE_SYNC_MODE, 1);
            OSC_CHECK(OH_VideoDecoder_Configure(decoder, decFmt));
            OH_AVFormat_Destroy(decFmt);
        }
        OSC_CHECK(OH_VideoDecoder_Start(decoder));
        int32_t decStride = videoW;
        {
            OH_AVFormat *desc = OH_VideoDecoder_GetOutputDescription(decoder);
            if (desc != nullptr) {
                int32_t st = 0;
                if (OH_AVFormat_GetIntValue(desc, OH_MD_KEY_VIDEO_STRIDE, &st) && st > 0) {
                    decStride = st;
                }
                OH_AVFormat_Destroy(desc);
            }
        }
        MEDIA_LOG_INFO("decoder started, stride=%{public}d", decStride);

        // ---------- 编码器 ----------
        encoder = OH_VideoEncoder_CreateByMime("video/avc");
        if (encoder == nullptr) {
            ret = -12;
            break;
        }
        {
            OH_AVFormat *encFmt = OH_AVFormat_CreateVideoFormat("video/avc", videoW, videoH);
            OH_AVFormat_SetIntValue(encFmt, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
            OH_AVFormat_SetIntValue(encFmt, OH_MD_KEY_ENABLE_SYNC_MODE, 1);
            OH_AVFormat_SetIntValue(encFmt, OH_MD_KEY_BITRATE, 20000000);
            OH_AVFormat_SetIntValue(encFmt, OH_MD_KEY_FRAME_RATE, 30);
            OH_AVFormat_SetIntValue(encFmt, OH_MD_KEY_I_FRAME_INTERVAL, 30);
            OSC_CHECK(OH_VideoEncoder_Configure(encoder, encFmt));
            OH_AVFormat_Destroy(encFmt);
        }
        OSC_CHECK(OH_VideoEncoder_Start(encoder));
        int32_t encStride = videoW;
        int32_t encUvOffset = videoW * videoH; // 先按紧凑布局，首个输入缓冲按容量校正
        bool encLayoutResolved = false;
        {
            OH_AVFormat *desc = OH_VideoEncoder_GetInputDescription(encoder);
            if (desc != nullptr) {
                int32_t st = 0;
                if (OH_AVFormat_GetIntValue(desc, OH_MD_KEY_VIDEO_STRIDE, &st) && st > 0) {
                    encStride = st;
                }
                OH_AVFormat_Destroy(desc);
            }
        }
        MEDIA_LOG_INFO("encoder started, stride=%{public}d uvOff=%{public}d", encStride, encUvOffset);

        // ---------- 输出 ----------
        dstFd = open(dstPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (dstFd < 0) {
            ret = -14;
            break;
        }
        muxer = OH_AVMuxer_Create(dstFd, AV_OUTPUT_FORMAT_MPEG_4);
        if (muxer == nullptr) {
            ret = -15;
            break;
        }
        OH_AVMuxer_SetRotation(muxer, cfg.rotation);

        std::vector<uint8_t> rgbaFrame(static_cast<size_t>(videoW) * videoH * 4);
        std::vector<uint8_t> nv12Frame(static_cast<size_t>(encUvOffset) +
            static_cast<size_t>(encStride) * videoH / 2);
        bool muxerStarted = false;
        int32_t muxVideoTrack = -1;
        int32_t muxAudioTrack = -1;
        std::vector<uint8_t> csd; // 编码器输出的 codec specific data
        bool csdDone = false;
        bool demuxEos = false;
        bool decEos = false;
        bool encEosNotified = false;
        bool encEosSeen = false;
        uint64_t decodedFrames = 0;
        uint64_t encPushed = 0;
        uint64_t encOutputs = 0;

        // ---------- 主循环 ----------
        while (!(demuxEos && decEos && encEosNotified && encEosSeen)) {
            // 1) 喂解码器
            if (!demuxEos) {
                uint32_t inIdx = 0;
                if (OH_VideoDecoder_QueryInputBuffer(decoder, &inIdx, 0) == AV_ERR_OK) {
                    OH_AVBuffer *decIn = OH_VideoDecoder_GetInputBuffer(decoder, inIdx);
                    if (decIn != nullptr) {
                        OH_AVErrCode r = OH_AVDemuxer_ReadSampleBuffer(demuxer,
                            static_cast<uint32_t>(videoTrackIdx), videoInBuf);
                        OH_AVCodecBufferAttr attr = {};
                        OH_AVBuffer_GetBufferAttr(videoInBuf, &attr);
                        if (r != AV_ERR_OK || (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0) {
                            OH_AVCodecBufferAttr eosAttr = {};
                            eosAttr.flags = AVCODEC_BUFFER_FLAGS_EOS;
                            OH_AVBuffer_SetBufferAttr(decIn, &eosAttr);
                            OH_VideoDecoder_PushInputBuffer(decoder, inIdx);
                            demuxEos = true;
                        } else {
                            uint8_t *dst = OH_AVBuffer_GetAddr(decIn);
                            int32_t cap = OH_AVBuffer_GetCapacity(decIn);
                            size_t copy = static_cast<size_t>(attr.size);
                            if (copy > static_cast<size_t>(cap)) {
                                copy = static_cast<size_t>(cap);
                            }
                            if (dst != nullptr && copy > 0) {
                                memcpy(dst, OH_AVBuffer_GetAddr(videoInBuf), copy);
                            }
                            OH_AVBuffer_SetBufferAttr(decIn, &attr);
                            OH_VideoDecoder_PushInputBuffer(decoder, inIdx);
                        }
                    }
                }
            }

            // 2) 解码帧 -> 合成 -> 编码
            if (!decEos) {
                uint32_t outIdx = 0;
                if (OH_VideoDecoder_QueryOutputBuffer(decoder, &outIdx, 0) == AV_ERR_OK) {
                    OH_AVBuffer *decOut = OH_VideoDecoder_GetOutputBuffer(decoder, outIdx);
                    if (decOut != nullptr) {
                        OH_AVCodecBufferAttr attr = {};
                        OH_AVBuffer_GetBufferAttr(decOut, &attr);
                        if ((attr.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0) {
                            decEos = true;
                        } else if (OH_AVBuffer_GetAddr(decOut) != nullptr && attr.size > 0) {
                            // 解码输出布局：UV 偏移由缓冲实际尺寸反推（size = uvOff + stride*H/2）
                            int decUvOffset = decStride * videoH;
                            const int minTight = decStride * videoH + decStride * videoH / 2;
                            if (attr.size >= minTight) {
                                decUvOffset = attr.size - decStride * videoH / 2;
                            }
                            Nv12ToRgbaStride(OH_AVBuffer_GetAddr(decOut), decStride, decUvOffset,
                                videoW, videoH, rgbaFrame.data(), false);
                            std::vector<DrawQuad> quads = BuildFrameOverlays(cfg, attr.pts, videoW, videoH);
                            BlitQuads(rgbaFrame.data(), videoW, videoH, quads);
                            RgbaToNv12Stride(rgbaFrame.data(), videoW, videoH, nv12Frame.data(),
                                encStride, encUvOffset);

                            uint32_t encInIdx = 0;
                            if (decodedFrames % 60 == 0) {
                                MEDIA_LOG_INFO("progress: decoded=%{public}llu pushed=%{public}llu out=%{public}llu",
                                    decodedFrames, encPushed, encOutputs);
                            }
                            decodedFrames++;
                            if (OH_VideoEncoder_QueryInputBuffer(encoder, &encInIdx,
                                POLL_TIMEOUT_US) == AV_ERR_OK) {
                                OH_AVBuffer *encIn = OH_VideoEncoder_GetInputBuffer(encoder, encInIdx);
                                if (encIn != nullptr) {
                                    uint8_t *dst = OH_AVBuffer_GetAddr(encIn);
                                    int32_t cap = OH_AVBuffer_GetCapacity(encIn);
                                    if (!encLayoutResolved) {
                                        // 首个输入缓冲：容量反推 UV 偏移（cap = uvOff + stride*H/2）
                                        int32_t tight = encStride * videoH + encStride * videoH / 2;
                                        if (cap >= tight) {
                                            encUvOffset = cap - encStride * videoH / 2;
                                        }
                                        encLayoutResolved = true;
                                        MEDIA_LOG_INFO("enc input layout: cap=%{public}d stride=%{public}d uvOff=%{public}d",
                                            cap, encStride, encUvOffset);
                                    }
                                    size_t need = static_cast<size_t>(encUvOffset) +
                                        static_cast<size_t>(encStride) * videoH / 2;
                                    if (dst != nullptr && cap >= static_cast<int32_t>(need)) {
                                        memcpy(dst, nv12Frame.data(), need);
                                        OH_AVCodecBufferAttr inAttr = {};
                                        inAttr.pts = attr.pts;
                                        inAttr.size = static_cast<int32_t>(need);
                                        inAttr.flags = AVCODEC_BUFFER_FLAGS_NONE;
                                        OH_AVBuffer_SetBufferAttr(encIn, &inAttr);
                                        OH_VideoEncoder_PushInputBuffer(encoder, encInIdx);
                                        encPushed++;
                                    }
                                }
                            }
                            if (durationUs > 0) {
                                int pct = static_cast<int>(attr.pts * 80 / durationUs);
                                if (pct > 80) {
                                    pct = 80;
                                }
                                ReportProgress(onProgress, pct);
                            }
                        }
                        OH_VideoDecoder_FreeOutputBuffer(decoder, outIdx);
                    }
                }
                if (decEos && !encEosNotified) {
                    uint32_t eosIdx = 0;
                    if (OH_VideoEncoder_QueryInputBuffer(encoder, &eosIdx,
                        POLL_TIMEOUT_US) == AV_ERR_OK) {
                        OH_AVBuffer *eosIn = OH_VideoEncoder_GetInputBuffer(encoder, eosIdx);
                        if (eosIn != nullptr) {
                            OH_AVCodecBufferAttr eosAttr = {};
                            eosAttr.flags = AVCODEC_BUFFER_FLAGS_EOS;
                            OH_AVBuffer_SetBufferAttr(eosIn, &eosAttr);
                            OH_VideoEncoder_PushInputBuffer(encoder, eosIdx);
                            MEDIA_LOG_INFO("encoder EOS pushed (sync mode)");
                        }
                    }
                    encEosNotified = true;
                    MEDIA_LOG_INFO("decoder EOS reached: decoded=%{public}llu pushed=%{public}llu out=%{public}llu",
                        decodedFrames, encPushed, encOutputs);
                }
            }

            // 3) 编码输出 -> muxer
            if (!encEosSeen) {
                uint32_t encOutIdx = 0;
                if (OH_VideoEncoder_QueryOutputBuffer(encoder, &encOutIdx, 0) == AV_ERR_OK) {
                    OH_AVBuffer *encOut = OH_VideoEncoder_GetOutputBuffer(encoder, encOutIdx);
                    if (encOut != nullptr) {
                        OH_AVCodecBufferAttr attr = {};
                        OH_AVBuffer_GetBufferAttr(encOut, &attr);
                        encOutputs++;
                        if ((attr.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0) {
                            encEosSeen = true;
                        } else if (OH_AVBuffer_GetAddr(encOut) != nullptr && attr.size > 0) {
                            const uint8_t *d = OH_AVBuffer_GetAddr(encOut);
                            if ((attr.flags & AVCODEC_BUFFER_FLAGS_CODEC_DATA) != 0) {
                                csd.insert(csd.end(), d, d + attr.size);
                            } else if (!csdDone && csd.empty() &&
                                (attr.flags & AVCODEC_BUFFER_FLAGS_SYNC_FRAME) != 0) {
                                // 编码器未单独输出 CSD 时，首关键帧作 CSD（并照常写入样本）
                                csd.insert(csd.end(), d, d + attr.size);
                                csdDone = true;
                            }
                            if (!muxerStarted && (csdDone || !csd.empty())) {
                                if (!csdDone && !csd.empty()) {
                                    csdDone = true;
                                }
                                OH_AVFormat *vfmt = OH_AVFormat_CreateVideoFormat("video/avc", videoW, videoH);
                                OH_AVFormat_SetIntValue(vfmt, OH_MD_KEY_FRAME_RATE, 30);
                                OH_AVFormat_SetBuffer(vfmt, OH_MD_KEY_CODEC_CONFIG, csd.data(), csd.size());
                                if (OH_AVMuxer_AddTrack(muxer, &muxVideoTrack, vfmt) != AV_ERR_OK) {
                                    OH_AVFormat_Destroy(vfmt);
                                    ret = -30;
                                    break;
                                }
                                OH_AVFormat_Destroy(vfmt);
                                if (audioTrackIdx >= 0) {
                                    OH_AVFormat *afmt = OH_AVSource_GetTrackFormat(source,
                                        static_cast<uint32_t>(audioTrackIdx));
                                    if (afmt != nullptr) {
                                        if (OH_AVMuxer_AddTrack(muxer, &muxAudioTrack, afmt) != AV_ERR_OK) {
                                            muxAudioTrack = -1;
                                        }
                                        OH_AVFormat_Destroy(afmt);
                                    }
                                }
                                OSC_CHECK(OH_AVMuxer_Start(muxer));
                                muxerStarted = true;
                            }
                            if (muxerStarted && (attr.flags & AVCODEC_BUFFER_FLAGS_CODEC_DATA) == 0) {
                                if (OH_AVMuxer_WriteSampleBuffer(muxer,
                                    static_cast<uint32_t>(muxVideoTrack), encOut) != AV_ERR_OK) {
                                    ret = -31;
                                    break;
                                }
                            }
                        }
                        OH_VideoEncoder_FreeOutputBuffer(encoder, encOutIdx);
                    }
                }
            }

            // 让出 CPU（2ms），避免全速忙转
            struct timespec ts = {0, 2000000};
            nanosleep(&ts, nullptr);
        }
        if (ret != 0) {
            break;
        }

        // ---------- 音频直通 ----------
        if (audioTrackIdx >= 0 && muxAudioTrack >= 0 && muxerStarted) {
            ReportProgress(onProgress, 82);
            int32_t audioMax = 1 << 20; // 1MB
            {
                OH_AVFormat *afmt = OH_AVSource_GetTrackFormat(source,
                    static_cast<uint32_t>(audioTrackIdx));
                if (afmt != nullptr) {
                    int32_t mi = 0;
                    if (OH_AVFormat_GetIntValue(afmt, OH_MD_KEY_MAX_INPUT_SIZE, &mi) && mi > audioMax) {
                        audioMax = mi;
                    }
                    OH_AVFormat_Destroy(afmt);
                }
            }
            audioInBuf = OH_AVBuffer_Create(audioMax);
            while (audioInBuf != nullptr) {
                OH_AVErrCode r = OH_AVDemuxer_ReadSampleBuffer(demuxer,
                    static_cast<uint32_t>(audioTrackIdx), audioInBuf);
                OH_AVCodecBufferAttr attr = {};
                OH_AVBuffer_GetBufferAttr(audioInBuf, &attr);
                if (r != AV_ERR_OK || (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0) {
                    break;
                }
                if (attr.size > 0) {
                    if (OH_AVMuxer_WriteSampleBuffer(muxer,
                        static_cast<uint32_t>(muxAudioTrack), audioInBuf) != AV_ERR_OK) {
                        break;
                    }
                }
            }
        }

        MEDIA_LOG_INFO("video pass done, muxer stopping");
        ReportProgress(onProgress, 96);
        OH_AVMuxer_Stop(muxer);
        ReportProgress(onProgress, 100);
        MEDIA_LOG_INFO("burn finished, ret=%{public}d", ret);
    } while (0);

    // ---------- 清理 ----------
    if (videoInBuf != nullptr) {
        OH_AVBuffer_Destroy(videoInBuf);
    }
    if (audioInBuf != nullptr) {
        OH_AVBuffer_Destroy(audioInBuf);
    }
    if (muxer != nullptr) {
        OH_AVMuxer_Destroy(muxer);
    }
    if (encoder != nullptr) {
        OH_VideoEncoder_Stop(encoder);
        OH_VideoEncoder_Destroy(encoder);
    }
    if (decoder != nullptr) {
        OH_VideoDecoder_Stop(decoder);
        OH_VideoDecoder_Destroy(decoder);
    }
    if (demuxer != nullptr) {
        OH_AVDemuxer_Destroy(demuxer);
    }
    if (source != nullptr) {
        OH_AVSource_Destroy(source);
    }
    if (srcFd >= 0) {
        close(srcFd);
    }
    if (dstFd >= 0) {
        close(dstFd);
    }
    return ret;
}

} // namespace osc
