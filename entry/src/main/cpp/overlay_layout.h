/**
 * 水印布局计算：把 ArkTS 侧配置（锚点/边距/字形图集/静态贴图）
 * 转换为“编码帧坐标系”下的绘制 quad 列表。
 *
 * 关键点：视频带旋转元数据（竖拍 rotation=90），播放器显示时顺时针旋转。
 * 水印位置/排版在“显示空间”（旋转后，即用户所见）计算，
 * 再逆变换回编码帧坐标；字形内容按需反向旋转，保证显示时文字正向。
 */
#ifndef OSC_OVERLAY_LAYOUT_H
#define OSC_OVERLAY_LAYOUT_H

#include <string>
#include <vector>
#include <cstdint>

namespace osc {

struct GlyphMeta {
    std::string ch;
    int width = 0;
    int height = 0;
    const uint8_t *rgba = nullptr; // RGBA8888
};

struct StaticMeta {
    int anchor = 0;
    float marginXPx = 0;
    float marginYPx = 0;
    int width = 0;
    int height = 0;
    float displayHeightPx = 0; // 显示空间期望高度，等比缩放
    float opacity = 1.0f;
    const uint8_t *rgba = nullptr;
};

struct LapInfo {
    int index = 0;
    double totalMs = 0;
    double lapMs = 0;
};

struct TimerCfg {
    bool enabled = false;
    int anchor = 0;
    float marginXPx = 0;
    float marginYPx = 0;
    float glyphHeightPx = 0;
    std::string chars;
    int lapLabelIndex = -1;
    bool showLap = false;
    double startOffsetMs = 0;
    std::vector<LapInfo> laps;
};

struct BurnCfg {
    int videoWidth = 0;
    int videoHeight = 0;
    int rotation = 0; // 0/90/180/270，编码帧顺时针旋转后显示
    TimerCfg timer;
    std::vector<GlyphMeta> glyphs;
    std::vector<StaticMeta> statics;
};

/** 一个待绘制贴图块 */
struct DrawQuad {
    const uint8_t *src = nullptr;
    int srcW = 0;
    int srcH = 0;
    // 编码帧坐标
    int dstX = 0;
    int dstY = 0;
    int dstW = 0;
    int dstH = 0;
    float opacity = 1.0f;
    int rotSteps = 0; // 源顺时针旋转 rotSteps*90 后贴入
};

/** 计时显示格式：mm:ss.cs（上限 99:59.99） */
std::string FormatTimerText(double ms);

/**
 * 生成某帧（pts 微秒）的全部水印 quad。
 * @param cfg 烧录配置
 * @param ptsUs 帧时间戳（微秒）
 * @param videoW 编码帧宽
 * @param videoH 编码帧高
 */
std::vector<DrawQuad> BuildFrameOverlays(const BurnCfg &cfg, int64_t ptsUs, int videoW, int videoH);

} // namespace osc

#endif // OSC_OVERLAY_LAYOUT_H
