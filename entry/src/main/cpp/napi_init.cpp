/**
 * libburn.so napi 绑定：
 * startBurn(src: string, dst: string, cfgJson: string, buffers: ArrayBuffer[],
 *           onProgress: (pct: number) => void): Promise<number>
 */
#include <pthread.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "napi/native_api.h"

#include "burn_engine.h"
#include "record_stream.h"
#include "rtsp_server.h"
#include "onvif_server.h"
#include "mini_json.h"
#include "overlay_layout.h"

namespace {

struct BurnContext {
    std::string src;
    std::string dst;
    std::string cfgJson;
    osc::BurnCfg cfg;
    std::vector<uint8_t *> bufferPtrs;
    std::vector<napi_ref> bufferRefs;
    napi_threadsafe_function progressFn = nullptr;
    napi_deferred deferred = nullptr;
    napi_env env = nullptr;
    napi_async_work work = nullptr;
    int resultCode = 0;
};

// 解析配置 JSON（buffer 内容由 napi 层传入，这里只解析元信息+bufferIndex）
bool ParseConfig(const std::string &json, const std::vector<uint8_t *> &buffers, osc::BurnCfg &cfg)
{
    osc::JsonValue root;
    if (!osc::MiniJson::Parse(json, root) || !root.IsObject()) {
        return false;
    }
    cfg.videoWidth = root.Int("videoWidth", 0);
    cfg.videoHeight = root.Int("videoHeight", 0);
    cfg.rotation = root.Int("rotation", 0);

    const osc::JsonValue *timer = root.Get("timer");
    if (timer != nullptr && timer->IsObject()) {
        cfg.timer.enabled = timer->Bool("enabled", false);
        cfg.timer.anchor = timer->Int("anchor", 0);
        cfg.timer.marginXPx = static_cast<float>(timer->Num("marginXPx", 0));
        cfg.timer.marginYPx = static_cast<float>(timer->Num("marginYPx", 0));
        cfg.timer.glyphHeightPx = static_cast<float>(timer->Num("glyphHeightPx", 0));
        cfg.timer.chars = timer->Str("chars", "");
        cfg.timer.lapLabelIndex = timer->Int("lapLabelIndex", -1);
        cfg.timer.showLap = timer->Bool("showLap", false);
        cfg.timer.startOffsetMs = timer->Num("startOffsetMs", 0);
        const osc::JsonValue *laps = timer->Get("laps");
        if (laps != nullptr && laps->IsArray()) {
            for (const auto &l : laps->arr) {
                osc::LapInfo info;
                info.index = l.Int("index", 0);
                info.totalMs = l.Num("totalMs", 0);
                info.lapMs = l.Num("lapMs", 0);
                cfg.timer.laps.push_back(info);
            }
        }
    }

    const osc::JsonValue *glyphs = root.Get("glyphs");
    if (glyphs != nullptr && glyphs->IsArray()) {
        for (const auto &g : glyphs->arr) {
            osc::GlyphMeta meta;
            meta.ch = g.Str("char", "");
            meta.width = g.Int("width", 0);
            meta.height = g.Int("height", 0);
            int bi = g.Int("bufferIndex", -1);
            if (bi >= 0 && bi < static_cast<int>(buffers.size())) {
                meta.rgba = buffers[bi];
            }
            if (meta.rgba != nullptr && meta.width > 0 && meta.height > 0) {
                cfg.glyphs.push_back(meta);
            }
        }
    }

    const osc::JsonValue *statics = root.Get("statics");
    if (statics != nullptr && statics->IsArray()) {
        for (const auto &s : statics->arr) {
            osc::StaticMeta meta;
            meta.anchor = s.Int("anchor", 0);
            meta.marginXPx = static_cast<float>(s.Num("marginXPx", 0));
            meta.marginYPx = static_cast<float>(s.Num("marginYPx", 0));
            meta.width = s.Int("width", 0);
            meta.height = s.Int("height", 0);
            meta.displayHeightPx = static_cast<float>(s.Num("displayHeightPx", 0));
            meta.opacity = static_cast<float>(s.Num("opacity", 1.0));
            int bi = s.Int("bufferIndex", -1);
            if (bi >= 0 && bi < static_cast<int>(buffers.size())) {
                meta.rgba = buffers[bi];
            }
            if (meta.rgba != nullptr && meta.width > 0 && meta.height > 0) {
                cfg.statics.push_back(meta);
            }
        }
    }
    return true;
}

void ProgressCallJs(napi_env env, napi_value jsCallback, void *context, void *data)
{
    int *pct = static_cast<int *>(data);
    if (env != nullptr && jsCallback != nullptr && pct != nullptr) {
        napi_value undefined = nullptr;
        napi_get_undefined(env, &undefined);
        napi_value arg = nullptr;
        napi_create_int32(env, *pct, &arg);
        napi_call_function(env, undefined, jsCallback, 1, &arg, nullptr);
        delete pct;
    }
}

void ExecuteWork(napi_env env, void *data)
{
    auto *ctx = static_cast<BurnContext *>(data);
    osc::BurnEngine engine;
    ctx->resultCode = engine.Run(ctx->src, ctx->dst, ctx->cfg,
        [ctx](int pct) {
            if (ctx->progressFn != nullptr) {
                int *heap = new int(pct);
                napi_call_threadsafe_function(ctx->progressFn, heap, napi_tsfn_blocking);
            }
        });
}

void CompleteWork(napi_env env, napi_status status, void *data)
{
    auto *ctx = static_cast<BurnContext *>(data);
    if (ctx->progressFn != nullptr) {
        napi_release_threadsafe_function(ctx->progressFn, napi_tsfn_release);
    }
    napi_value result = nullptr;
    napi_create_int32(env, ctx->resultCode, &result);
    napi_resolve_deferred(env, ctx->deferred, result);
    for (auto &ref : ctx->bufferRefs) {
        napi_delete_reference(env, ref);
    }
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

napi_value StartBurn(napi_env env, napi_callback_info info)
{
    size_t argc = 5;
    napi_value args[5] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 5) {
        napi_throw_error(env, nullptr, "startBurn 需要 5 个参数");
        return nullptr;
    }

    auto *ctx = new BurnContext();
    ctx->env = env;

    // 字符串参数（路径长度远小于缓冲上限）
    char buf[4096] = {0};
    size_t len = 0;
    napi_get_value_string_utf8(env, args[0], buf, sizeof(buf), &len);
    ctx->src = buf;
    napi_get_value_string_utf8(env, args[1], buf, sizeof(buf), &len);
    ctx->dst = buf;
    napi_get_value_string_utf8(env, args[2], buf, sizeof(buf), &len);
    ctx->cfgJson = buf;

    // buffers：提取指针并持有引用防止 GC
    bool isArray = false;
    napi_is_array(env, args[3], &isArray);
    if (isArray) {
        uint32_t count = 0;
        napi_get_array_length(env, args[3], &count);
        for (uint32_t i = 0; i < count; i++) {
            napi_value elem = nullptr;
            napi_get_element(env, args[3], i, &elem);
            void *data = nullptr;
            size_t byteLen = 0;
            bool ok = false;
            napi_is_arraybuffer(env, elem, &ok);
            if (ok && napi_get_arraybuffer_info(env, elem, &data, &byteLen) == napi_ok && data != nullptr) {
                ctx->bufferPtrs.push_back(static_cast<uint8_t *>(data));
                napi_ref ref = nullptr;
                napi_create_reference(env, elem, 1, &ref);
                ctx->bufferRefs.push_back(ref);
            } else {
                ctx->bufferPtrs.push_back(nullptr);
            }
        }
    }

    if (!ParseConfig(ctx->cfgJson, ctx->bufferPtrs, ctx->cfg)) {
        for (auto &ref : ctx->bufferRefs) {
            napi_delete_reference(env, ref);
        }
        delete ctx;
        napi_throw_error(env, nullptr, "烧录配置解析失败");
        return nullptr;
    }

    // progress threadsafe function
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "oscBurn", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_threadsafe_function(env, args[4], nullptr, resourceName, 1, 1,
        nullptr, nullptr, nullptr, ProgressCallJs, &ctx->progressFn);

    // promise + async work（引擎在 work 线程执行）
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_create_async_work(env, nullptr, resourceName, ExecuteWork, CompleteWork,
        ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);
    return promise;
}


// ---------------- 实时录制管线（阶段1） ----------------
static osc::RecordStream g_recordStream;

static napi_value NapiPrepareRecord(napi_env env, napi_callback_info info)
{
    size_t argc = 5;
    napi_value args[5] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 5) {
        napi_throw_error(env, nullptr, "prepareRecord 需要 5 个参数");
        return nullptr;
    }
    int32_t w = 0, h = 0, fps = 30, bitrate = 20000000, rotation = 90;
    napi_get_value_int32(env, args[0], &w);
    napi_get_value_int32(env, args[1], &h);
    napi_get_value_int32(env, args[2], &fps);
    napi_get_value_int32(env, args[3], &bitrate);
    napi_get_value_int32(env, args[4], &rotation);
    uint64_t surfaceId = 0;
    int code = g_recordStream.Prepare(w, h, fps, bitrate, rotation, surfaceId);
    if (code != 0) {
        napi_throw_error(env, nullptr, "prepareRecord 失败");
        return nullptr;
    }
    napi_value result = nullptr;
    napi_create_string_utf8(env, std::to_string(surfaceId).c_str(), NAPI_AUTO_LENGTH, &result);
    return result;
}

static napi_value NapiBeginRecord(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        napi_throw_error(env, nullptr, "beginRecord 需要 1 个参数");
        return nullptr;
    }
    char buf[4096] = {0};
    size_t len = 0;
    napi_get_value_string_utf8(env, args[0], buf, sizeof(buf), &len);
    int code = g_recordStream.Begin(std::string(buf));
    napi_value result = nullptr;
    napi_create_int32(env, code, &result);
    return result;
}

// 停止录制：async work —— Execute 工作线程跑 Stop，Complete 回 JS 线程解决 Promise
struct StopRecordCtx {
    napi_deferred deferred = nullptr;
    napi_async_work work = nullptr;
    osc::RecordStats stats;
};

static void StopRecordExecute(napi_env env, void *data)
{
    (void)env;
    auto *ctx = static_cast<StopRecordCtx *>(data);
    ctx->stats = g_recordStream.Stop();
}

static void StopRecordComplete(napi_env env, napi_status status, void *data)
{
    (void)status;
    auto *ctx = static_cast<StopRecordCtx *>(data);
    napi_value result = nullptr;
    napi_create_object(env, &result);
    napi_value dur = nullptr;
    napi_create_int64(env, ctx->stats.durationMs, &dur);
    napi_set_named_property(env, result, "durationMs", dur);
    napi_value frames = nullptr;
    napi_create_int64(env, static_cast<int64_t>(ctx->stats.frames), &frames);
    napi_set_named_property(env, result, "frames", frames);
    napi_resolve_deferred(env, ctx->deferred, result);
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

static napi_value NapiStopRecord(napi_env env, napi_callback_info info)
{
    (void)info;
    auto *ctx = new StopRecordCtx();
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "oscStopRecord", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, nullptr, resourceName, StopRecordExecute, StopRecordComplete,
        ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);
    return promise;
}

static napi_value NapiReleaseRecord(napi_env env, napi_callback_info info)
{
    (void)env;
    (void)info;
    g_recordStream.Release();
    return nullptr;
}


static napi_value NapiSetWatermarkAssets(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) {
        napi_throw_error(env, nullptr, "setWatermarkAssets 需要 2 个参数");
        return nullptr;
    }
    char buf[8192] = {0};
    size_t len = 0;
    napi_get_value_string_utf8(env, args[0], buf, sizeof(buf), &len);
    std::vector<const uint8_t *> buffers;
    bool isArray = false;
    napi_is_array(env, args[1], &isArray);
    if (isArray) {
        uint32_t count = 0;
        napi_get_array_length(env, args[1], &count);
        for (uint32_t i = 0; i < count; i++) {
            napi_value elem = nullptr;
            napi_get_element(env, args[1], i, &elem);
            void *data = nullptr;
            size_t byteLen = 0;
            bool ok = false;
            napi_is_arraybuffer(env, elem, &ok);
            if (ok && napi_get_arraybuffer_info(env, elem, &data, &byteLen) == napi_ok && data != nullptr) {
                buffers.push_back(static_cast<const uint8_t *>(data));
            } else {
                buffers.push_back(nullptr);
            }
        }
    }
    int code = g_recordStream.SetWatermarkAssets(std::string(buf), buffers);
    napi_value result = nullptr;
    napi_create_int32(env, code, &result);
    return result;
}

static napi_value NapiUpdateWatermarkLaps(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        napi_throw_error(env, nullptr, "updateWatermarkLaps 需要 1 个参数");
        return nullptr;
    }
    char buf[8192] = {0};
    size_t len = 0;
    napi_get_value_string_utf8(env, args[0], buf, sizeof(buf), &len);
    int code = g_recordStream.UpdateLaps(std::string(buf));
    napi_value result = nullptr;
    napi_create_int32(env, code, &result);
    return result;
}


static napi_value NapiStartRtsp(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t port = 8554;
    if (argc >= 1) {
        napi_get_value_int32(env, args[0], &port);
    }
    int code = osc::RtspServer::Instance().Start(port);
    if (code == 0) {
        // ONVIF 随推流启停：自动发现 + GetStreamUri 直达 RTSP 地址
        osc::OnvifServer::Instance().Start(8000, port);
    }
    napi_value result = nullptr;
    napi_create_int32(env, code, &result);
    return result;
}

static napi_value NapiStopRtsp(napi_env env, napi_callback_info info)
{
    (void)env;
    (void)info;
    osc::RtspServer::Instance().Stop();
    osc::OnvifServer::Instance().Stop();
    return nullptr;
}

static napi_value NapiRtspStatus(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value result = nullptr;
    napi_create_object(env, &result);
    napi_value running = nullptr;
    napi_get_boolean(env, osc::RtspServer::Instance().IsRunning(), &running);
    napi_set_named_property(env, result, "running", running);
    napi_value hasClient = nullptr;
    napi_get_boolean(env, osc::RtspServer::Instance().HasClient(), &hasClient);
    napi_set_named_property(env, result, "hasClient", hasClient);
    napi_value port = nullptr;
    napi_create_int32(env, osc::RtspServer::Instance().GetPort(), &port);
    napi_set_named_property(env, result, "port", port);
    return result;
}


static napi_value NapiSetCameraFlip(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        napi_throw_error(env, nullptr, "setCameraFlip 需要 1 个参数");
        return nullptr;
    }
    bool flip = false;
    napi_get_value_bool(env, args[0], &flip);
    g_recordStream.SetFlipX(flip);
    return nullptr;
}


static napi_value NapiStartRecordingFile(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        napi_throw_error(env, nullptr, "startRecordingFile 需要 1 个参数");
        return nullptr;
    }
    char buf[4096] = {0};
    size_t len = 0;
    napi_get_value_string_utf8(env, args[0], buf, sizeof(buf), &len);
    int code = g_recordStream.StartRecording(std::string(buf));
    napi_value result = nullptr;
    napi_create_int32(env, code, &result);
    return result;
}

static napi_value NapiStopRecordingFile(napi_env env, napi_callback_info info)
{
    (void)env;
    (void)info;
    osc::RecordStats stats = g_recordStream.StopRecording();
    napi_value result = nullptr;
    napi_create_object(env, &result);
    napi_value dur = nullptr;
    napi_create_int64(env, stats.durationMs, &dur);
    napi_set_named_property(env, result, "durationMs", dur);
    napi_value frames = nullptr;
    napi_create_int64(env, static_cast<int64_t>(stats.frames), &frames);
    napi_set_named_property(env, result, "frames", frames);
    return result;
}

} // namespace} // namespace

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        {"startBurn", nullptr, StartBurn, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"prepareRecord", nullptr, NapiPrepareRecord, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"beginRecord", nullptr, NapiBeginRecord, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopRecord", nullptr, NapiStopRecord, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"releaseRecord", nullptr, NapiReleaseRecord, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setWatermarkAssets", nullptr, NapiSetWatermarkAssets, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"updateWatermarkLaps", nullptr, NapiUpdateWatermarkLaps, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setCameraFlip", nullptr, NapiSetCameraFlip, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startRecordingFile", nullptr, NapiStartRecordingFile, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopRecordingFile", nullptr, NapiStopRecordingFile, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startRtsp", nullptr, NapiStartRtsp, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopRtsp", nullptr, NapiStopRtsp, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"rtspStatus", nullptr, NapiRtspStatus, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

static napi_module burnModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "burn",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterBurnModule(void)
{
    napi_module_register(&burnModule);
}
