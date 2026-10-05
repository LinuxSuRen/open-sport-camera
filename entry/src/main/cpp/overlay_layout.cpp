#include "overlay_layout.h"

namespace osc {

// 显示空间尺寸
static void DisplaySize(int rotation, int videoW, int videoH, int &dispW, int &dispH)
{
    if (rotation == 90 || rotation == 270) {
        dispW = videoH;
        dispH = videoW;
    } else {
        dispW = videoW;
        dispH = videoH;
    }
}

// 显示空间 rect -> 编码帧 rect（逆时针撤销显示旋转）
static void DispRectToEncoded(int rotation, int videoW, int videoH,
    float dx, float dy, float dw, float dh,
    int &ex, int &ey, int &ew, int &eh)
{
    if (rotation == 90) {
        // 显示=编码顺时针90：display(x,y) <- encoded(y, W_d-1-x)，W_d=videoH
        // 逆变换：encoded x = dy, y = H_enc - (dx+dw)，H_enc=videoH
        ex = static_cast<int>(dy + 0.5f);
        ey = static_cast<int>(videoH - (dx + dw) + 0.5f);
        ew = static_cast<int>(dh + 0.5f);
        eh = static_cast<int>(dw + 0.5f);
    } else if (rotation == 270) {
        // 显示=编码逆时针90：display(x,y) <- encoded(H_d-1-y, x)，H_d=videoW
        // 逆变换：encoded x = W_enc - (dy+dh)，W_enc=videoW
        ex = static_cast<int>(videoW - (dy + dh) + 0.5f);
        ey = static_cast<int>(dx + 0.5f);
        ew = static_cast<int>(dh + 0.5f);
        eh = static_cast<int>(dw + 0.5f);
    } else if (rotation == 180) {
        ex = static_cast<int>(videoW - (dx + dw) + 0.5f);
        ey = static_cast<int>(videoH - (dy + dh) + 0.5f);
        ew = static_cast<int>(dw + 0.5f);
        eh = static_cast<int>(dh + 0.5f);
    } else {
        ex = static_cast<int>(dx + 0.5f);
        ey = static_cast<int>(dy + 0.5f);
        ew = static_cast<int>(dw + 0.5f);
        eh = static_cast<int>(dh + 0.5f);
    }
}

// 源贴图需要顺时针旋转的步数，抵消播放器显示旋转
static int RotStepsForRotation(int rotation)
{
    // 显示时播放器顺时针转 rotation 度；贴图需预转 (4 - rotation/90) 步
    return (4 - (rotation / 90)) % 4;
}

// 九宫格锚点 -> 显示空间左上角
static void AnchorPos(int anchor, float marginX, float marginY,
    int dispW, int dispH, float w, float h, float &x, float &y)
{
    switch (anchor) {
        case 0: x = marginX; y = marginY; break;                       // 左上
        case 1: x = (dispW - w) / 2; y = marginY; break;               // 上中
        case 2: x = dispW - w - marginX; y = marginY; break;           // 右上
        case 3: x = marginX; y = dispH - h - marginY; break;           // 左下
        case 4: x = (dispW - w) / 2; y = dispH - h - marginY; break;   // 下中
        default: x = dispW - w - marginX; y = dispH - h - marginY;     // 右下
    }
}

std::string FormatTimerText(double ms)
{
    if (ms < 0) {
        ms = 0;
    }
    if (ms > (99 * 60 + 59) * 1000 + 990) {
        ms = (99 * 60 + 59) * 1000 + 990;
    }
    long long total = static_cast<long long>(ms);
    int cs = static_cast<int>((total % 1000) / 10);
    int s = static_cast<int>((total / 1000) % 60);
    int m = static_cast<int>(total / 60000);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d.%02d", m, s, cs);
    return std::string(buf);
}

std::vector<DrawQuad> BuildFrameOverlays(const BurnCfg &cfg, int64_t ptsUs, int videoW, int videoH)
{
    std::vector<DrawQuad> quads;
    int dispW = 0;
    int dispH = 0;
    DisplaySize(cfg.rotation, videoW, videoH, dispW, dispH);
    const int rotSteps = RotStepsForRotation(cfg.rotation);

    // 1. 静态水印（底层）
    for (const auto &st : cfg.statics) {
        if (st.rgba == nullptr || st.width <= 0 || st.height <= 0 || st.displayHeightPx <= 0) {
            continue;
        }
        float scale = st.displayHeightPx / static_cast<float>(st.height);
        float w = st.width * scale;
        float h = st.displayHeightPx;
        float x = 0;
        float y = 0;
        AnchorPos(st.anchor, st.marginXPx, st.marginYPx, dispW, dispH, w, h, x, y);
        DrawQuad q;
        q.src = st.rgba;
        q.srcW = st.width;
        q.srcH = st.height;
        q.opacity = st.opacity;
        q.rotSteps = rotSteps;
        DispRectToEncoded(cfg.rotation, videoW, videoH, x, y, w, h, q.dstX, q.dstY, q.dstW, q.dstH);
        quads.push_back(q);
    }

    // 2. 计时水印（顶层）
    if (!cfg.timer.enabled || cfg.glyphs.empty()) {
        return quads;
    }
    const double tMs = cfg.timer.startOffsetMs + static_cast<double>(ptsUs) / 1000.0;

    // 字形查找
    auto findGlyph = [&](const std::string &ch) -> const GlyphMeta * {
        for (const auto &g : cfg.glyphs) {
            if (g.ch == ch) {
                return &g;
            }
        }
        return nullptr;
    };

    const std::string totalStr = FormatTimerText(tMs);
    // 行高：以数字字高为基准
    const GlyphMeta *digit = findGlyph("0");
    float glyphH = digit != nullptr ? static_cast<float>(digit->height) : cfg.timer.glyphHeightPx;
    if (glyphH <= 0) {
        glyphH = cfg.timer.glyphHeightPx;
    }

    // lap 行文本：LAP nn mm:ss.cs（取 tMs 时刻已记录的最近一圈）
    std::string lapStr;
    const LapInfo *lastLap = nullptr;
    for (const auto &l : cfg.timer.laps) {
        if (l.totalMs <= tMs) {
            lastLap = &l;
        }
    }
    if (cfg.timer.showLap && lastLap != nullptr && cfg.timer.lapLabelIndex >= 0) {
        lapStr = "LAP " + std::to_string(lastLap->index) + " " + FormatTimerText(lastLap->lapMs);
        // 补零两位圈数
        if (lastLap->index < 10) {
            lapStr = "LAP 0" + std::to_string(lastLap->index) + " " + FormatTimerText(lastLap->lapMs);
        }
    }

    // 行文本宽度（显示空间）
    auto lineWidth = [&](const std::string &text, float heightRatio) -> float {
        float w = 0;
        for (char c : text) {
            const GlyphMeta *g = findGlyph(std::string(1, c));
            if (g != nullptr) {
                w += g->width * heightRatio;
            }
        }
        return w;
    };

    float totalH = glyphH;
    float lapH = 0;
    if (!lapStr.empty()) {
        lapH = glyphH * 0.62f;
    }
    float totalW = lineWidth(totalStr, 1.0f);
    float lapW = lapStr.empty() ? 0 : lineWidth(lapStr, 0.62f);
    float blockW = totalW > lapW ? totalW : lapW;
    float blockH = totalH + (lapH > 0 ? lapH * 1.25f : 0);

    float bx = 0;
    float by = 0;
    AnchorPos(cfg.timer.anchor, cfg.timer.marginXPx, cfg.timer.marginYPx,
        dispW, dispH, blockW, blockH, bx, by);

    // 逐字符生成 quad（显示空间横排 -> 编码空间）
    auto emitLine = [&](const std::string &text, float x0, float y0, float hRatio) {
        float x = x0;
        for (char c : text) {
            const GlyphMeta *g = findGlyph(std::string(1, c));
            if (g == nullptr) {
                continue;
            }
            float w = g->width * hRatio;
            float h = g->height * hRatio;
            DrawQuad q;
            q.src = g->rgba;
            q.srcW = g->width;
            q.srcH = g->height;
            q.opacity = 1.0f;
            q.rotSteps = rotSteps;
            DispRectToEncoded(cfg.rotation, videoW, videoH, x, y0, w, h,
                q.dstX, q.dstY, q.dstW, q.dstH);
            quads.push_back(q);
            x += w;
        }
    };

    // 总时长行（块内左对齐）
    emitLine(totalStr, bx, by, 1.0f);
    // 计圈行
    if (!lapStr.empty()) {
        // LAP 标签用整体贴块替换字符串前缀：拆分处理
        // 布局：[LAP块][空格][圈数2位][空格][时间]
        float ly = by + totalH * 1.25f;
        float x = bx;
        if (cfg.timer.lapLabelIndex < static_cast<int>(cfg.glyphs.size())) {
            const GlyphMeta *lab = &cfg.glyphs[cfg.timer.lapLabelIndex];
            float scale = (lapH / glyphH); // lap行高比例
            float w = lab->width * scale;
            DrawQuad q;
            q.src = lab->rgba;
            q.srcW = lab->width;
            q.srcH = lab->height;
            q.rotSteps = rotSteps;
            DispRectToEncoded(cfg.rotation, videoW, videoH, x, ly, w, lab->height * scale,
                q.dstX, q.dstY, q.dstW, q.dstH);
            quads.push_back(q);
            x += w;
            const GlyphMeta *sp = findGlyph(" ");
            if (sp != nullptr) {
                x += sp->width * scale;
            }
        }
        // 圈数（两位）
        char nbuf[4];
        snprintf(nbuf, sizeof(nbuf), "%02d", lastLap != nullptr ? lastLap->index : 0);
        for (int i = 0; i < 2; i++) {
            const GlyphMeta *g = findGlyph(std::string(1, nbuf[i]));
            if (g != nullptr) {
                float scale = lapH / glyphH;
                float w = g->width * scale;
                float h = g->height * scale;
                DrawQuad q;
                q.src = g->rgba;
                q.srcW = g->width;
                q.srcH = g->height;
                q.rotSteps = rotSteps;
                DispRectToEncoded(cfg.rotation, videoW, videoH, x, ly, w, h,
                    q.dstX, q.dstY, q.dstW, q.dstH);
                quads.push_back(q);
                x += w;
            }
        }
        const GlyphMeta *sp2 = findGlyph(" ");
        if (sp2 != nullptr) {
            x += sp2->width * (lapH / glyphH);
        }
        // 圈用时
        std::string lapTime = FormatTimerText(lastLap != nullptr ? lastLap->lapMs : 0);
        for (char c : lapTime) {
            const GlyphMeta *g = findGlyph(std::string(1, c));
            if (g == nullptr) {
                continue;
            }
            float scale = lapH / glyphH;
            float w = g->width * scale;
            float h = g->height * scale;
            DrawQuad q;
            q.src = g->rgba;
            q.srcW = g->width;
            q.srcH = g->height;
            q.rotSteps = rotSteps;
            DispRectToEncoded(cfg.rotation, videoW, videoH, x, ly, w, h,
                q.dstX, q.dstY, q.dstW, q.dstH);
            quads.push_back(q);
            x += w;
        }
    }

    return quads;
}

} // namespace osc
