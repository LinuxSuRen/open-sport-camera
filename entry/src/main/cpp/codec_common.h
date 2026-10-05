/**
 * 编解码公共工具：日志宏、AVC CSD 解析与组装。
 * burn_engine（录后烧录）与 record_stream（实时管线）共用。
 */
#ifndef OSC_CODEC_COMMON_H
#define OSC_CODEC_COMMON_H

#include <cstdint>
#include <vector>

namespace osc {

// 从编码输出中提取裸 SPS/PPS NAL（支持 annex-B 起始码与 AVCC 长度前缀）
void CollectAvcCsd(const uint8_t *buf, size_t size, std::vector<uint8_t> &spsNal,
    std::vector<uint8_t> &ppsNal);

// 组装 avcC 记录（ISO 14496-15）：muxer 的 CODEC_CONFIG 期望格式
std::vector<uint8_t> BuildAvcCsd(const std::vector<uint8_t> &spsNal, const std::vector<uint8_t> &ppsNal);

} // namespace osc

#endif // OSC_CODEC_COMMON_H
