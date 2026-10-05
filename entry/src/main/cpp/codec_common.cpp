#include "codec_common.h"

namespace osc {

void CollectAvcCsd(const uint8_t *buf, size_t size, std::vector<uint8_t> &spsNal,
    std::vector<uint8_t> &ppsNal)
{
    auto hasStart = [&]() {
        for (size_t i = 0; i + 3 < size; i++) {
            if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
                return true;
            }
        }
        return false;
    };
    auto tryNal = [&](const uint8_t *nal, size_t n) {
        if (n < 2) {
            return;
        }
        int t = nal[0] & 0x1F;
        if (t == 7 && spsNal.empty()) {
            spsNal.assign(nal, nal + n);
        } else if (t == 8 && ppsNal.empty()) {
            ppsNal.assign(nal, nal + n);
        }
    };
    if (hasStart()) {
        size_t i = 0;
        while (i + 4 < size) {
            size_t scLen = 0;
            if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
                scLen = 3;
            } else if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1) {
                scLen = 4;
            }
            if (scLen == 0) {
                i++;
                continue;
            }
            size_t nalStart = i + scLen;
            if (nalStart >= size) {
                break;
            }
            size_t j = nalStart + 1;
            while (j + 3 < size) {
                if (buf[j] == 0 && buf[j + 1] == 0 && buf[j + 2] == 1) {
                    break;
                }
                if (buf[j] == 0 && buf[j + 1] == 0 && buf[j + 2] == 0 && buf[j + 3] == 1) {
                    break;
                }
                j++;
            }
            size_t nalEnd = (j + 3 >= size) ? size : j;
            tryNal(buf + nalStart, nalEnd - nalStart);
            i = nalEnd;
        }
        return;
    }
    // AVCC 长度前缀
    size_t i = 0;
    while (i + 4 < size) {
        uint32_t len = (static_cast<uint32_t>(buf[i]) << 24) | (static_cast<uint32_t>(buf[i + 1]) << 16) |
            (static_cast<uint32_t>(buf[i + 2]) << 8) | static_cast<uint32_t>(buf[i + 3]);
        if (len == 0 || i + 4 + len > size) {
            break;
        }
        tryNal(buf + i + 4, len);
        i += 4 + len;
    }
}

std::vector<uint8_t> BuildAvcCsd(const std::vector<uint8_t> &spsNal, const std::vector<uint8_t> &ppsNal)
{
    std::vector<uint8_t> out;
    if (spsNal.size() < 4 || ppsNal.empty()) {
        return out;
    }
    out.push_back(0x01);
    out.push_back(spsNal[1]);
    out.push_back(spsNal[2]);
    out.push_back(spsNal[3]);
    out.push_back(0xFF);
    out.push_back(0xE1);
    out.push_back(static_cast<uint8_t>((spsNal.size() >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(spsNal.size() & 0xFF));
    out.insert(out.end(), spsNal.begin(), spsNal.end());
    out.push_back(0x01);
    out.push_back(static_cast<uint8_t>((ppsNal.size() >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(ppsNal.size() & 0xFF));
    out.insert(out.end(), ppsNal.begin(), ppsNal.end());
    return out;
}

} // namespace osc
