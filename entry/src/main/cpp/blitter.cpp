#include "blitter.h"

namespace osc {

// 源坐标（含旋转步进）映射：dst 内偏移(u,v) -> src 坐标
static inline void MapUV(int u, int v, int dw, int dh, int sw, int sh,
    int rotSteps, int &sx, int &sy)
{
    // dst 尺寸已在布局层完成奇偶交换，这里 dw/dh 对应旋转后的宽高
    switch (rotSteps) {
        case 1: // 源顺时针90：dst(u,v) <- src(v, sh-1-u)
            sx = v * (sw - 1) / (dh > 1 ? dh - 1 : 1);
            sy = (sh - 1) - u * (sh - 1) / (dw > 1 ? dw - 1 : 1);
            break;
        case 2:
            sx = (sw - 1) - u * (sw - 1) / (dw > 1 ? dw - 1 : 1);
            sy = (sh - 1) - v * (sh - 1) / (dh > 1 ? dh - 1 : 1);
            break;
        case 3: // 源逆时针90：dst(u,v) <- src(sw-1-v, u)
            sx = (sw - 1) - v * (sw - 1) / (dh > 1 ? dh - 1 : 1);
            sy = u * (sh - 1) / (dw > 1 ? dw - 1 : 1);
            break;
        default:
            sx = u * (sw - 1) / (dw > 1 ? dw - 1 : 1);
            sy = v * (sh - 1) / (dh > 1 ? dh - 1 : 1);
            break;
    }
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx >= sw) sx = sw - 1;
    if (sy >= sh) sy = sh - 1;
}

void BlitQuads(uint8_t *frame, int frameW, int frameH, const std::vector<DrawQuad> &quads)
{
    for (const auto &q : quads) {
        if (q.src == nullptr || q.dstW <= 0 || q.dstH <= 0 || q.srcW <= 0 || q.srcH <= 0) {
            continue;
        }
        int x0 = q.dstX < 0 ? 0 : q.dstX;
        int y0 = q.dstY < 0 ? 0 : q.dstY;
        int x1 = q.dstX + q.dstW > frameW ? frameW : q.dstX + q.dstW;
        int y1 = q.dstY + q.dstH > frameH ? frameH : q.dstY + q.dstH;
        for (int y = y0; y < y1; y++) {
            int v = y - q.dstY;
            uint8_t *drow = frame + (static_cast<size_t>(y) * frameW + x0) * 4;
            for (int x = x0; x < x1; x++) {
                int u = x - q.dstX;
                int sx = 0;
                int sy = 0;
                MapUV(u, v, q.dstW, q.dstH, q.srcW, q.srcH, q.rotSteps, sx, sy);
                const uint8_t *s = q.src + (static_cast<size_t>(sy) * q.srcW + sx) * 4;
                float a = (s[3] / 255.0f) * q.opacity;
                if (a <= 0.0f) {
                    drow += 4;
                    continue;
                }
                if (a >= 1.0f) {
                    drow[0] = s[0];
                    drow[1] = s[1];
                    drow[2] = s[2];
                    drow[3] = 255;
                } else {
                    drow[0] = static_cast<uint8_t>(s[0] * a + drow[0] * (1 - a));
                    drow[1] = static_cast<uint8_t>(s[1] * a + drow[1] * (1 - a));
                    drow[2] = static_cast<uint8_t>(s[2] * a + drow[2] * (1 - a));
                    drow[3] = 255;
                }
                drow += 4;
            }
        }
    }
}

// BT.601 limited range
static inline uint8_t Clamp8(int v)
{
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

void Nv12ToRgba(const uint8_t *nv12, int w, int h, uint8_t *rgba, bool nv21)
{
    const uint8_t *yPlane = nv12;
    const uint8_t *uvPlane = nv12 + static_cast<size_t>(w) * h;
    for (int j = 0; j < h; j++) {
        const uint8_t *yrow = yPlane + static_cast<size_t>(j) * w;
        const uint8_t *uvrow = uvPlane + static_cast<size_t>(j / 2) * w;
        uint8_t *drow = rgba + static_cast<size_t>(j) * w * 4;
        for (int i = 0; i < w; i++) {
            int Y = yrow[i] - 16;
            int U = uvrow[(i & ~1)] - 128;
            int V = uvrow[(i & ~1) + 1] - 128;
            if (nv21) {
                int t = U;
                U = V;
                V = t;
            }
            int r = (298 * Y + 409 * V + 128) >> 8;
            int g = (298 * Y - 100 * U - 208 * V + 128) >> 8;
            int b = (298 * Y + 516 * U + 128) >> 8;
            drow[i * 4] = Clamp8(r);
            drow[i * 4 + 1] = Clamp8(g);
            drow[i * 4 + 2] = Clamp8(b);
            drow[i * 4 + 3] = 255;
        }
    }
}

void RgbaToNv12(const uint8_t *rgba, int w, int h, uint8_t *nv12)
{
    uint8_t *yPlane = nv12;
    uint8_t *uvPlane = nv12 + static_cast<size_t>(w) * h;
    for (int j = 0; j < h; j++) {
        const uint8_t *srow = rgba + static_cast<size_t>(j) * w * 4;
        uint8_t *drow = yPlane + static_cast<size_t>(j) * w;
        for (int i = 0; i < w; i++) {
            int r = srow[i * 4];
            int g = srow[i * 4 + 1];
            int b = srow[i * 4 + 2];
            drow[i] = Clamp8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    for (int j = 0; j < h / 2; j++) {
        uint8_t *duv = uvPlane + static_cast<size_t>(j) * w;
        for (int i = 0; i < w / 2; i++) {
            int r = 0, g = 0, b = 0;
            // 2x2 采样
            for (int dy = 0; dy < 2; dy++) {
                const uint8_t *srow = rgba + static_cast<size_t>(j * 2 + dy) * w * 4;
                for (int dx = 0; dx < 2; dx++) {
                    int idx = (i * 2 + dx) * 4;
                    r += srow[idx];
                    g += srow[idx + 1];
                    b += srow[idx + 2];
                }
            }
            r /= 4;
            g /= 4;
            b /= 4;
            duv[i * 2] = Clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            duv[i * 2 + 1] = Clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

} // namespace osc
