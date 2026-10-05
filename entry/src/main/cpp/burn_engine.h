/**
 * 烧录引擎：解码 -> 水印合成 -> 编码 -> 封装。
 *
 * 管线（全程缓冲区模式，PTS 显式传递）：
 *   1. AVSource/AVDemuxer 读取源视频轨
 *   2. VideoDecoder 输出 NV12 -> 转 RGBA -> CPU 合成水印 -> 转回 NV12
 *   3. VideoEncoder 重编码（H.264）
 *   4. AVMuxer 封装：新视频轨 + 源音频轨直通（不重编码）
 *
 * 单线程轮询式驱动（QueryInput/OutputBuffer），逻辑确定性好调试。
 */
#ifndef OSC_BURN_ENGINE_H
#define OSC_BURN_ENGINE_H

#include <functional>
#include <string>
#include "overlay_layout.h"

namespace osc {

using ProgressFn = std::function<void(int pct)>;

class BurnEngine {
public:
    /**
     * @return 0 成功；负数为错误码
     */
    int Run(const std::string &srcPath, const std::string &dstPath,
        const BurnCfg &cfg, const ProgressFn &onProgress);

private:
    static void ReportProgress(const ProgressFn &cb, int pct);
};

} // namespace osc

#endif // OSC_BURN_ENGINE_H
