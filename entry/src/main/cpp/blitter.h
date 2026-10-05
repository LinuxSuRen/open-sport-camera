/**
 * CPU 像素合成：RGBA 帧上的水印混合（支持旋转/缩放/透明度）+ NV12↔RGBA 转换。
 * 1080p 下单帧毫秒级，满足录后烧录的吞吐需求。
 */
#ifndef OSC_BLITTER_H
#define OSC_BLITTER_H

#include <vector>
#include <cstdint>
#include "overlay_layout.h"

namespace osc {

/** 把 quad 列表以 src-over 混合到 RGBA 帧 */
void BlitQuads(uint8_t *rgbaFrame, int frameW, int frameH, const std::vector<DrawQuad> &quads);

/** NV12/NV21 -> RGBA8888（BT.601 limited range） */
void Nv12ToRgba(const uint8_t *nv12, int w, int h, uint8_t *rgba, bool nv21);

/** RGBA8888 -> NV12（BT.601 limited range），输出与输入同尺寸 */
void RgbaToNv12(const uint8_t *rgba, int w, int h, uint8_t *nv12);

} // namespace osc

#endif // OSC_BLITTER_H
