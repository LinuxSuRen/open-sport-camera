#include "record_stream.h"

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include "hilog/log.h"
#include "multimedia/player_framework/native_avbuffer.h"
#include "multimedia/player_framework/native_avcodec_base.h"
#include "multimedia/player_framework/native_avcodec_videoencoder.h"
#include "multimedia/player_framework/native_avformat.h"
#include "multimedia/player_framework/native_avmuxer.h"
#include "native_image/native_image.h"
#include "native_window/external_window.h"

#include "blitter.h"
#include "codec_common.h"

namespace osc {

static void TeardownOf(RecordStream::Impl &impl);

static constexpr unsigned int RS_LOG_DOMAIN = 0x0013;
static constexpr const char *RS_LOG_TAG = "RecordStream";
#define RS_LOG(fmt, ...)                                                                         \
    OH_LOG_Print(LOG_APP, LOG_INFO, RS_LOG_DOMAIN, RS_LOG_TAG, "%{public}s: " fmt, __func__,     \
        ##__VA_ARGS__)
#define RS_ERR(fmt, ...)                                                                         \
    OH_LOG_Print(LOG_APP, LOG_ERROR, RS_LOG_DOMAIN, RS_LOG_TAG, "%{public}s: " fmt, __func__,    \
        ##__VA_ARGS__)

// ---------------- 着色器：OES 外部纹理全屏直通（阶段2 叠加水纹路） ----------------
static const char *VERT_SRC = R"(#version 300 es
layout(location=0) in vec2 aPos;
out vec2 vTex;
void main() {
    vTex = vec2(aPos.x * 0.5 + 0.5, 0.5 - aPos.y * 0.5);
    gl_Position = vec4(aPos, 0.0, 1.0);
})";

static const char *FRAG_SRC = R"(#version 300 es
#extension GL_OES_EGL_image_external : require
precision mediump float;
in vec2 vTex;
uniform samplerExternalOES uTex;
out vec4 outColor;
void main() {
    outColor = texture(uTex, vTex);
})";

struct RecordStream::Impl {
  // 生命周期
  std::atomic<bool> running{false};

  // GL
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext context = EGL_NO_CONTEXT;
  EGLConfig config = nullptr;
  EGLSurface encSurface = EGL_NO_SURFACE;
  EGLSurface pbuffer = EGL_NO_SURFACE;
  GLuint program = 0;
  GLuint vao = 0;
  GLuint vbo = 0;
  GLuint oesTex = 0;
  GLint uTexLoc = -1;

  // 相机输入
  OH_NativeImage *nativeImage = nullptr;
  OHNativeWindow *camWindow = nullptr;

  // 编码器（surface 模式）
  OH_AVCodec *encoder = nullptr;
  OHNativeWindow *encWindow = nullptr;
  std::mutex encOutMtx;
  std::vector<uint8_t> spsNal;
  std::vector<uint8_t> ppsNal;

  // 封装
  int outFd = -1;
  OH_AVMuxer *muxer = nullptr;
  bool muxerStarted = false;
  int32_t videoTrack = -1;

  // 渲染线程（常驻：Prepare 起，空闲排水保持流不塞；录制时绘制到编码面）
  std::atomic<bool> loopRunning{false};
  std::atomic<bool> drainRunning{false};
  std::thread drainThread;
  std::mutex frameMtx;
  std::condition_variable frameCv;
  bool framePending = false;
  std::thread renderThread;
  std::atomic<uint64_t> frameCount{0};
  uint64_t idleCount = 0;
  int64_t firstTs = -1;
  int64_t lastTs = -1;

  // 参数
  int width = 0;
  int height = 0;
  int fps = 30;
  int rotation = 0;

  void RenderLoop();
  void DrainLoop();
  bool SetupGraphics();
  bool InitGL(uint64_t &surfaceIdOut);
  bool InitEncoder(int bitrate);
  void Teardown();
};

// ---------------- 编码器回调（surface 模式） ----------------
static void OnEncodedOutput(RecordStream::Impl *impl, OH_AVCodec *codec, uint32_t index,
    OH_AVBuffer *buffer);
static void OnEncoderError(OH_AVCodec *codec, int32_t errorCode, void *userData)
{
  (void)codec;
  auto *impl = static_cast<RecordStream::Impl *>(userData);
  RS_ERR("encoder error %{public}d", errorCode);
  (void)impl;
}

static void OnEncoderStreamChanged(OH_AVCodec *codec, OH_AVFormat *format, void *userData)
{
  (void)codec;
  (void)format;
  (void)userData;
}

static void OnNeedInputParameter(OH_AVCodec *codec, uint32_t index, OH_AVFormat *parameter,
    void *userData)
{
  // surface 模式必须回推参数（可修改 stride 等，此处原样接受）
  (void)parameter;
  (void)userData;
  OH_VideoEncoder_PushInputParameter(codec, index);
}

static void OnEncodedOutput(RecordStream::Impl *impl, OH_AVCodec *codec, uint32_t index,
    OH_AVBuffer *buffer)
{
  OH_AVCodecBufferAttr attr = {};
  OH_AVBuffer_GetBufferAttr(buffer, &attr);
  const uint8_t *data = OH_AVBuffer_GetAddr(buffer);

  std::lock_guard<std::mutex> lock(impl->encOutMtx);
  if ((attr.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0) {
    // EOS：Stop 流程收尾
  } else if (data != nullptr && attr.size > 0) {
    if (impl->spsNal.empty() || impl->ppsNal.empty()) {
      CollectAvcCsd(data, attr.size, impl->spsNal, impl->ppsNal);
    }
    const bool isCodecData = (attr.flags & AVCODEC_BUFFER_FLAGS_CODEC_DATA) != 0;
    if (!impl->muxerStarted && !impl->spsNal.empty() && !impl->ppsNal.empty() && !isCodecData) {
      std::vector<uint8_t> csd = BuildAvcCsd(impl->spsNal, impl->ppsNal);
      OH_AVFormat *vfmt = OH_AVFormat_CreateVideoFormat("video/avc", impl->width, impl->height);
      OH_AVFormat_SetIntValue(vfmt, OH_MD_KEY_FRAME_RATE, impl->fps);
      OH_AVFormat_SetBuffer(vfmt, OH_MD_KEY_CODEC_CONFIG, csd.data(), csd.size());
      if (OH_AVMuxer_AddTrack(impl->muxer, &impl->videoTrack, vfmt) != AV_ERR_OK) {
        RS_ERR("add video track failed");
      }
      OH_AVFormat_Destroy(vfmt);
      if (OH_AVMuxer_Start(impl->muxer) != AV_ERR_OK) {
        RS_ERR("muxer start failed");
      } else {
        impl->muxerStarted = true;
        RS_LOG("muxer started, csd=%{public}d", static_cast<int>(csd.size()));
      }
    }
    if (impl->muxerStarted && !isCodecData) {
      OH_AVMuxer_WriteSampleBuffer(impl->muxer, static_cast<uint32_t>(impl->videoTrack), buffer);
    }
  }
  OH_VideoEncoder_FreeOutputBuffer(codec, index);
}

static void OnNewOutputBuffer(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer,
    void *userData)
{
  auto *impl = static_cast<RecordStream::Impl *>(userData);
  OnEncodedOutput(impl, codec, index, buffer);
}

// ---------------- 帧可用回调（相机驱动节奏） ----------------
static void OnFrameAvailable(void *context)
{
  auto *impl = static_cast<RecordStream::Impl *>(context);
  {
    std::lock_guard<std::mutex> lock(impl->frameMtx);
    impl->framePending = true;
  }
  impl->frameCv.notify_one();
}

bool RecordStream::Impl::InitGL(uint64_t &surfaceIdOut)
{
  display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (display == EGL_NO_DISPLAY) {
    RS_ERR("eglGetDisplay failed");
    return false;
  }
  EGLint major = 0;
  EGLint minor = 0;
  if (!eglInitialize(display, &major, &minor)) {
    RS_ERR("eglInitialize failed");
    return false;
  }
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint attribs[] = {
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_NONE
  };
  EGLint num = 0;
  if (!eglChooseConfig(display, attribs, &config, 1, &num) || num < 1) {
    RS_ERR("eglChooseConfig failed");
    return false;
  }
  const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctxAttribs);
  if (context == EGL_NO_CONTEXT) {
    RS_ERR("eglCreateContext failed");
    return false;
  }
  // pbuffer：资源创建 + 后续 readback 渲染目标（视频分辨率）
  const EGLint pbufAttribs[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
  pbuffer = eglCreatePbufferSurface(display, config, pbufAttribs);
  if (pbuffer == EGL_NO_SURFACE || !eglMakeCurrent(display, pbuffer, pbuffer, context)) {
    RS_ERR("pbuffer setup failed");
    return false;
  }

  // 相机输入：OES 纹理 + NativeImage
  glGenTextures(1, &oesTex);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, oesTex);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  nativeImage = OH_NativeImage_Create(oesTex, GL_TEXTURE_EXTERNAL_OES);
  if (nativeImage == nullptr) {
    RS_ERR("OH_NativeImage_Create failed");
    return false;
  }
  OH_OnFrameAvailableListener listener;
  listener.context = this;
  listener.onFrameAvailable = OnFrameAvailable;
  OH_NativeImage_SetOnFrameAvailableListener(nativeImage, listener);
  camWindow = OH_NativeImage_AcquireNativeWindow(nativeImage);
  if (camWindow == nullptr) {
    RS_ERR("AcquireNativeWindow failed");
    return false;
  }
  uint64_t sid = 0;
  if (OH_NativeWindow_GetSurfaceId(camWindow, &sid) != 0) {
    RS_ERR("GetSurfaceId failed");
    return false;
  }
  if (!SetupGraphics()) {
    return false;
  }
  // 释放主线程上下文（渲染线程需要接管；EGL 上下文同一时刻仅一线程可用）
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  surfaceIdOut = sid;
  return true;
}

bool RecordStream::Impl::InitEncoder(int bitrate)
{
  encoder = OH_VideoEncoder_CreateByMime("video/avc");
  if (encoder == nullptr) {
    RS_ERR("create encoder failed");
    return false;
  }
  OH_AVFormat *fmt = OH_AVFormat_CreateVideoFormat("video/avc", width, height);
  OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_BITRATE, bitrate);
  OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_FRAME_RATE, fps);
  OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_I_FRAME_INTERVAL, fps); // 1s GOP
  OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
  // 缓冲模式 + 同步模式：轮询收发（烧录引擎同款，真机已验证；
  // 本机 surface 模式 EGL 窗口缓冲分配 0x0 失败，弃用）
  OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_ENABLE_SYNC_MODE, 1);
  if (OH_VideoEncoder_Configure(encoder, fmt) != AV_ERR_OK) {
    OH_AVFormat_Destroy(fmt);
    RS_ERR("encoder configure failed");
    return false;
  }
  OH_AVFormat_Destroy(fmt);
  return true;
}


bool RecordStream::Impl::SetupGraphics()
{
  auto compile = [](GLenum type, const char *src) -> GLuint {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[512] = {0};
      glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
      RS_ERR("shader compile: %{public}s", log);
      glDeleteShader(sh);
      return 0;
    }
    return sh;
  };
  GLuint vs = compile(GL_VERTEX_SHADER, VERT_SRC);
  GLuint fs = compile(GL_FRAGMENT_SHADER, FRAG_SRC);
  if (vs == 0 || fs == 0) {
    return false;
  }
  program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint linked = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked) {
    RS_ERR("program link failed");
    return false;
  }
  uTexLoc = glGetUniformLocation(program, "uTex");
  const float verts[] = {
    -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f,
  };
  glGenVertexArrays(1, &vao);
  glGenBuffers(1, &vbo);
  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  RS_LOG("graphics ready");
  return true;
}


// 编码输出轮询收取（同步模式）
void RecordStream::Impl::DrainLoop()
{
  RS_LOG("drain loop start");
  while (drainRunning.load()) {
    uint32_t idx = 0;
    if (OH_VideoEncoder_QueryOutputBuffer(encoder, &idx, 500000) != AV_ERR_OK) {
      continue;
    }
    OH_AVBuffer *buf = OH_VideoEncoder_GetOutputBuffer(encoder, idx);
    if (buf == nullptr) {
      continue;
    }
    OnEncodedOutput(this, encoder, idx, buf);
  }
  RS_LOG("drain loop exit");
}

void RecordStream::Impl::RenderLoop()
{
  // 缓冲模式管线：相机帧 → GL 合成（阶段2加水印）→ readback → NV12 → 编码
  if (!eglMakeCurrent(display, pbuffer, pbuffer, context)) {
    RS_ERR("makeCurrent pbuffer failed");
    return;
  }
  // pbuffer 尺寸 = 视频尺寸（Configure 前 pbuffer 是 1x1；这里重建）
  // 注：pbuffer 表面尺寸固定，此处直接按 width/height 创建的 pbuffer 已在 Prepare 重建
  std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
  std::vector<uint8_t> nv12;
  int32_t encStride = width;
  int32_t encUvOffset = width * height;
  bool encLayoutResolved = false;
  uint64_t pushed = 0;
  uint64_t outputs = 0;

  while (loopRunning.load()) {
    std::unique_lock<std::mutex> lock(frameMtx);
    frameCv.wait_for(lock, std::chrono::milliseconds(200),
        [this] { return framePending || !loopRunning.load(); });
    if (!framePending) {
      // 空闲：仍轮询编码输出（录制停止瞬间可能有残留）
      lock.unlock();
      continue;
    }
    framePending = false;
    lock.unlock();

    if (OH_NativeImage_UpdateSurfaceImage(nativeImage) != 0) {
      continue;
    }
    int64_t ts = OH_NativeImage_GetTimestamp(nativeImage);
    idleCount++;
    if (idleCount % 150 == 0) {
      RS_LOG("drained %{public}llu", static_cast<unsigned long long>(idleCount));
    }
    if (!running.load()) {
      continue; // 空闲排水：仅消费，保持队列流转
    }

    // GL 合成到 pbuffer
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, oesTex);
    glUniform1i(uTexLoc, 0);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    if (firstTs < 0) {
      firstTs = ts;
      RS_LOG("first frame ts=%{public}lld", static_cast<long long>(ts));
    }
    lastTs = ts;

    // 推编码器（缓冲+同步）
    uint32_t inIdx = 0;
    if (OH_VideoEncoder_QueryInputBuffer(encoder, &inIdx, 3000000) != AV_ERR_OK) {
      RS_ERR("query input buffer timeout");
      continue;
    }
    OH_AVBuffer *inBuf = OH_VideoEncoder_GetInputBuffer(encoder, inIdx);
    if (inBuf == nullptr) {
      continue;
    }
    uint8_t *dst = OH_AVBuffer_GetAddr(inBuf);
    int32_t cap = OH_AVBuffer_GetCapacity(inBuf);
    if (!encLayoutResolved) {
      int32_t st = 0;
      OH_AVFormat *desc = OH_VideoEncoder_GetInputDescription(encoder);
      if (desc != nullptr) {
        if (OH_AVFormat_GetIntValue(desc, OH_MD_KEY_VIDEO_STRIDE, &st) && st > 0) {
          encStride = st;
        }
        OH_AVFormat_Destroy(desc);
      }
      int32_t tight = encStride * height + encStride * height / 2;
      if (cap >= tight) {
        encUvOffset = cap - encStride * height / 2;
      }
      nv12.resize(static_cast<size_t>(encUvOffset) + static_cast<size_t>(encStride) * height / 2);
      encLayoutResolved = true;
      RS_LOG("enc layout: cap=%{public}d stride=%{public}d uvOff=%{public}d", cap, encStride,
        encUvOffset);
    }
    RgbaToNv12Stride(rgba.data(), width, height, nv12.data(), encStride, encUvOffset);
    size_t need = static_cast<size_t>(encUvOffset) + static_cast<size_t>(encStride) * height / 2;
    if (dst != nullptr && cap >= static_cast<int32_t>(need)) {
      memcpy(dst, nv12.data(), need);
      OH_AVCodecBufferAttr attr = {};
      attr.pts = ts / 1000; // 编码器 PTS 单位 us，相机时间戳 ns
      attr.size = static_cast<int32_t>(need);
      OH_AVBuffer_SetBufferAttr(inBuf, &attr);
      OH_VideoEncoder_PushInputBuffer(encoder, inIdx);
      pushed++;
      if (frameCount.fetch_add(1) == 0) {
        RS_LOG("first frame pushed to encoder");
      }
    }

    // 收编码输出 → muxer
    uint32_t outIdx = 0;
    while (OH_VideoEncoder_QueryOutputBuffer(encoder, &outIdx, 0) == AV_ERR_OK) {
      OH_AVBuffer *outBuf = OH_VideoEncoder_GetOutputBuffer(encoder, outIdx);
      if (outBuf != nullptr) {
        OnEncodedOutput(this, encoder, outIdx, outBuf);
        outputs++;
      }
    }
  }
  RS_LOG("loop exit: pushed=%{public}llu outputs=%{public}llu",
    static_cast<unsigned long long>(pushed), static_cast<unsigned long long>(outputs));
}

int RecordStream::Prepare(int width, int height, int fps, int bitrate, int rotation,
    uint64_t &cameraSurfaceId)
{
  if (impl_ != nullptr) {
    return -100;
  }
  impl_ = new Impl();
  impl_->width = width;
  impl_->height = height;
  impl_->fps = fps;
  impl_->rotation = rotation;
  if (!impl_->InitGL(cameraSurfaceId)) {
    TeardownOf(*impl_);
    delete impl_;
    impl_ = nullptr;
    return -101;
  }
  if (!impl_->InitEncoder(bitrate)) {
    TeardownOf(*impl_);
    delete impl_;
    impl_ = nullptr;
    return -104;
  }
  impl_->loopRunning.store(true);
  impl_->renderThread = std::thread(&Impl::RenderLoop, impl_);
  RS_LOG("prepared %{public}dx%{public}d@%{public}d rotation=%{public}d", width, height, fps,
    rotation);
  return 0;
}

int RecordStream::Begin(const std::string &outPath)
{
  if (impl_ == nullptr || impl_->running.load()) {
    return -110;
  }
  impl_->outFd = open(outPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (impl_->outFd < 0) {
    return -102;
  }
  impl_->muxer = OH_AVMuxer_Create(impl_->outFd, AV_OUTPUT_FORMAT_MPEG_4);
  if (impl_->muxer == nullptr) {
    close(impl_->outFd);
    impl_->outFd = -1;
    return -103;
  }
  OH_AVMuxer_SetRotation(impl_->muxer, impl_->rotation);
  {
    std::lock_guard<std::mutex> lock(impl_->encOutMtx);
    impl_->muxerStarted = false;
    impl_->videoTrack = -1;
    impl_->spsNal.clear();
    impl_->ppsNal.clear();
  }
  impl_->firstTs = -1;
  impl_->lastTs = -1;
  impl_->frameCount.store(0);
  if (OH_VideoEncoder_Start(impl_->encoder) != AV_ERR_OK) {
    RS_ERR("encoder start failed");
    OH_AVMuxer_Destroy(impl_->muxer);
    impl_->muxer = nullptr;
    close(impl_->outFd);
    impl_->outFd = -1;
    return -105;
  }
  impl_->running.store(true);
  impl_->frameCv.notify_all();
  RS_LOG("begin: %{public}s", outPath.c_str());
  return 0;
}

RecordStats RecordStream::Stop()
{
  RecordStats stats;
  if (impl_ == nullptr || !impl_->running.load()) {
    stats.code = -111;
    return stats;
  }
  impl_->running.store(false);
  impl_->frameCv.notify_all();
  // 缓冲+同步模式 EOS：空缓冲+标记推送
  {
    uint32_t eosIdx = 0;
    if (OH_VideoEncoder_QueryInputBuffer(impl_->encoder, &eosIdx, 3000000) == AV_ERR_OK) {
      OH_AVBuffer *eosIn = OH_VideoEncoder_GetInputBuffer(impl_->encoder, eosIdx);
      if (eosIn != nullptr) {
        OH_AVCodecBufferAttr eosAttr = {};
        eosAttr.flags = AVCODEC_BUFFER_FLAGS_EOS;
        OH_AVBuffer_SetBufferAttr(eosIn, &eosAttr);
        OH_VideoEncoder_PushInputBuffer(impl_->encoder, eosIdx);
      }
    }
  }
  // 排空剩余输出（EOS 后编码器会吐出缓冲内全部帧）
  for (int i = 0; i < 300; i++) {
    uint32_t outIdx = 0;
    bool any = false;
    while (OH_VideoEncoder_QueryOutputBuffer(impl_->encoder, &outIdx, 200000) == AV_ERR_OK) {
      OH_AVBuffer *outBuf = OH_VideoEncoder_GetOutputBuffer(impl_->encoder, outIdx);
      if (outBuf != nullptr) {
        OnEncodedOutput(impl_, impl_->encoder, outIdx, outBuf);
        any = true;
      }
    }
    if (!any) {
      break;
    }
  }
  OH_VideoEncoder_Stop(impl_->encoder);
  {
    std::lock_guard<std::mutex> lock(impl_->encOutMtx);
    if (impl_->muxerStarted) {
      OH_AVMuxer_Stop(impl_->muxer);
    }
    impl_->muxerStarted = false;
  }
  OH_AVMuxer_Destroy(impl_->muxer);
  impl_->muxer = nullptr;
  close(impl_->outFd);
  impl_->outFd = -1;
  stats.frames = impl_->frameCount.load();
  if (impl_->firstTs >= 0 && impl_->lastTs >= impl_->firstTs) {
    stats.durationMs = (impl_->lastTs - impl_->firstTs) / 1000000;
  }
  RS_LOG("stopped frames=%{public}llu dur=%{public}lldms",
    static_cast<unsigned long long>(stats.frames), static_cast<long long>(stats.durationMs));
  return stats;
}

void RecordStream::Release()
{
  if (impl_ == nullptr) {
    return;
  }
  if (impl_->running.load()) {
    Stop();
  }
  impl_->loopRunning.store(false);
  impl_->frameCv.notify_all();
  if (impl_->renderThread.joinable()) {
    impl_->renderThread.join();
  }
  TeardownOf(*impl_);
  delete impl_;
  impl_ = nullptr;
  RS_LOG("released");
}

bool RecordStream::IsPrepared() const
{
  return impl_ != nullptr;
}

bool RecordStream::IsRecording() const
{
  return impl_ != nullptr && impl_->running.load();
}

int RecordStream::SetWatermark(const uint8_t *rgba, int texW, int texH, int x, int y, int w, int h)
{
  (void)rgba;
  (void)texW;
  (void)texH;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  return 0; // 阶段2实现
}

RecordStream::~RecordStream()
{
  Release();
}

// 统一清理（GL/编码器/封装/窗口）
static void TeardownOf(RecordStream::Impl &impl)
{
  if (impl.encoder != nullptr) {
    OH_VideoEncoder_Stop(impl.encoder);
    OH_VideoEncoder_Destroy(impl.encoder);
    impl.encoder = nullptr;
  }
  if (impl.muxer != nullptr) {
    OH_AVMuxer_Destroy(impl.muxer);
    impl.muxer = nullptr;
  }
  if (impl.outFd >= 0) {
    close(impl.outFd);
    impl.outFd = -1;
  }
  if (impl.program != 0) {
    glDeleteProgram(impl.program);
    impl.program = 0;
  }
  if (impl.vbo != 0) {
    glDeleteBuffers(1, &impl.vbo);
    impl.vbo = 0;
  }
  if (impl.vao != 0) {
    glDeleteVertexArrays(1, &impl.vao);
    impl.vao = 0;
  }
  if (impl.oesTex != 0) {
    glDeleteTextures(1, &impl.oesTex);
    impl.oesTex = 0;
  }
  if (impl.nativeImage != nullptr) {
    OH_NativeImage_Destroy(&impl.nativeImage);
    impl.nativeImage = nullptr;
  }
  if (impl.encSurface != EGL_NO_SURFACE) {
    eglDestroySurface(impl.display, impl.encSurface);
    impl.encSurface = EGL_NO_SURFACE;
  }
  if (impl.pbuffer != EGL_NO_SURFACE) {
    eglDestroySurface(impl.display, impl.pbuffer);
    impl.pbuffer = EGL_NO_SURFACE;
  }
  if (impl.context != EGL_NO_CONTEXT) {
    eglDestroyContext(impl.display, impl.context);
    impl.context = EGL_NO_CONTEXT;
  }
  if (impl.display != EGL_NO_DISPLAY) {
    eglTerminate(impl.display);
    impl.display = EGL_NO_DISPLAY;
  }
}

} // namespace osc
