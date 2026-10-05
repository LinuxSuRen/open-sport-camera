/**
 * 烧录引擎核心宿主机单元测试（不依赖 OHOS SDK，直接 clang++ 编译运行）。
 *
 * 覆盖：
 * 1. FormatTimerText 计时格式与上限
 * 2. mini_json 配置解析（含嵌套/数组/转义）
 * 3. BuildFrameOverlays 锚点与旋转坐标变换（0°/90°）、计圈行随时间切换
 * 4. BlitQuads 混合与旋转贴图
 * 5. NV12 <-> RGBA 往返误差
 *
 * 运行：bash entry/src/main/cpp/tests/run_host_tests.sh
 */
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "blitter.h"
#include "mini_json.h"
#include "overlay_layout.h"

using namespace osc;

static int g_failed = 0;
static int g_total = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        g_total++;                                                            \
        if (!(cond)) {                                                        \
            g_failed++;                                                       \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                     \
    } while (0)

// 生成统一色块字形（N x N，白色不透明）
static GlyphMeta SolidGlyph(const std::string &ch, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    GlyphMeta meta;
    meta.ch = ch;
    meta.width = w;
    meta.height = h;
    uint8_t *buf = new uint8_t[static_cast<size_t>(w) * h * 4];
    for (int i = 0; i < w * h; i++) {
        buf[i * 4] = r;
        buf[i * 4 + 1] = g;
        buf[i * 4 + 2] = b;
        buf[i * 4 + 3] = 255;
    }
    meta.rgba = buf;
    return meta;
}

static void FreeGlyphs(std::vector<GlyphMeta> &glyphs)
{
    for (auto &g : glyphs) {
        delete[] g.rgba;
        g.rgba = nullptr;
    }
}

static void TestFormatTimer()
{
    printf("[FormatTimerText]\n");
    CHECK(FormatTimerText(0) == "00:00.00");
    CHECK(FormatTimerText(75432) == "01:15.43");
    CHECK(FormatTimerText(5999990) == "99:59.99");
    CHECK(FormatTimerText(6000000) == "99:59.99"); // 封顶
    CHECK(FormatTimerText(-5) == "00:00.00");
}

static void TestMiniJson()
{
    printf("[mini_json]\n");
    const char *cfg = R"({"videoWidth":1920,"videoHeight":1080,"rotation":90,
      "timer":{"enabled":true,"anchor":0,"marginXPx":16.5,"marginYPx":32,
        "glyphHeightPx":104,"chars":"0123456789:. ","lapLabelIndex":12,
        "showLap":true,"startOffsetMs":1234.5,
        "laps":[{"index":1,"totalMs":30000,"lapMs":30000},{"index":2,"totalMs":61000,"lapMs":31000}]},
      "glyphs":[{"char":"0","width":60,"height":104,"bufferIndex":0}],
      "statics":[{"anchor":5,"marginXPx":10,"marginYPx":10,"width":200,"height":40,
        "displayHeightPx":80,"opacity":0.9,"bufferIndex":1}]})";
    JsonValue root;
    CHECK(MiniJson::Parse(cfg, root));
    CHECK(root.IsObject());
    CHECK(root.Int("videoWidth", 0) == 1920);
    CHECK(root.Int("rotation", 0) == 90);
    const JsonValue *timer = root.Get("timer");
    CHECK(timer != nullptr && timer->IsObject());
    CHECK(timer->Bool("enabled", false));
    CHECK(std::fabs(timer->Num("marginXPx", 0) - 16.5) < 1e-6);
    CHECK(std::fabs(timer->Num("startOffsetMs", 0) - 1234.5) < 1e-6);
    CHECK(timer->Int("lapLabelIndex", -1) == 12);
    const JsonValue *laps = timer->Get("laps");
    CHECK(laps != nullptr && laps->IsArray() && laps->arr.size() == 2);
    CHECK(laps->arr[1].Num("lapMs", 0) == 31000);
    const JsonValue *glyphs = root.Get("glyphs");
    CHECK(glyphs != nullptr && glyphs->arr.size() == 1);
    CHECK(glyphs->arr[0].Str("char", "") == "0");
    CHECK(glyphs->arr[0].Int("bufferIndex", -1) == 0);
    const JsonValue *statics = root.Get("statics");
    CHECK(statics != nullptr && statics->arr.size() == 1);
    CHECK(std::fabs(statics->arr[0].Num("opacity", 0) - 0.9) < 1e-6);

    // 非法输入
    JsonValue bad;
    CHECK(!MiniJson::Parse("{not json", bad));
}

static BurnCfg MakeCfg(int rotation)
{
    BurnCfg cfg;
    cfg.videoWidth = 1920;
    cfg.videoHeight = 1080;
    cfg.rotation = rotation;
    cfg.timer.enabled = true;
    cfg.timer.anchor = 0; // 显示空间左上
    cfg.timer.marginXPx = 50;
    cfg.timer.marginYPx = 60;
    cfg.timer.glyphHeightPx = 104;
    cfg.timer.chars = "0123456789:. ";
    cfg.timer.lapLabelIndex = 13;
    cfg.timer.showLap = false;
    cfg.timer.startOffsetMs = 0;
    return cfg;
}

static std::vector<GlyphMeta> MakeAtlas()
{
    std::vector<GlyphMeta> glyphs;
    const std::string chars = "0123456789:. ";
    for (char c : chars) {
        glyphs.push_back(SolidGlyph(std::string(1, c), 60, 104, 255, 255, 255));
    }
    glyphs.push_back(SolidGlyph("LAP", 180, 104, 255, 255, 255)); // index 13
    return glyphs;
}

static void TestOverlayRotation0()
{
    printf("[overlay rotation=0 anchor=TL]\n");
    BurnCfg cfg = MakeCfg(0);
    std::vector<GlyphMeta> glyphs = MakeAtlas();
    cfg.glyphs = glyphs;
    cfg.timer.showLap = false;

    auto quads = BuildFrameOverlays(cfg, 0, 1920, 1080);
    // 8 个计时字形（00:00.00）
    CHECK(quads.size() == 8);
    CHECK(quads[0].rotSteps == 0);
    // 显示空间(50,60) == 编码空间(50,60)
    CHECK(quads[0].dstX == 50);
    CHECK(quads[0].dstY == 60);
    // 字形按显示 x 前进排列
    CHECK(quads[1].dstX == 50 + 60);
    CHECK(quads[1].dstY == 60);
    // 全部在帧内
    for (const auto &q : quads) {
        CHECK(q.dstX >= 0 && q.dstY >= 0);
        CHECK(q.dstX + q.dstW <= 1920 && q.dstY + q.dstH <= 1080);
    }
    FreeGlyphs(glyphs);
}

static void TestOverlayRotation90()
{
    printf("[overlay rotation=90 anchor=TL(display)]\n");
    BurnCfg cfg = MakeCfg(90);
    std::vector<GlyphMeta> glyphs = MakeAtlas();
    cfg.glyphs = glyphs;

    auto quads = BuildFrameOverlays(cfg, 0, 1920, 1080);
    // 贴图需预转 3 步（抵消播放器顺时针 90°）
    for (const auto &q : quads) {
        CHECK(q.rotSteps == 3);
        CHECK(q.dstX >= 0 && q.dstY >= 0);
        CHECK(q.dstX + q.dstW <= 1920 && q.dstY + q.dstH <= 1080);
    }
    // 显示空间 TL(50,60) -> 编码空间 x=dy=60，y=H-(dx+dw)=1080-50-60=970（dw=字形显示宽60）
    // 四角连续坐标验证：编码 quad(60,970,104x60) 经 CW90 恰好映射回显示 (50,60,60x104)
    CHECK(quads[0].dstX == 60);
    CHECK(quads[0].dstY == 1080 - 50 - 60);
    // 同一行字形的编码 x 相同，y 递减（显示 x 正向 = 编码 y 负向）
    CHECK(quads[1].dstX == 60);
    CHECK(quads[1].dstY == quads[0].dstY - 60);

    FreeGlyphs(glyphs);
}

static void TestLapLineSwitch()
{
    printf("[lap line switches by frame time]\n");
    BurnCfg cfg = MakeCfg(0);
    std::vector<GlyphMeta> glyphs = MakeAtlas();
    cfg.glyphs = glyphs;
    cfg.timer.showLap = true;
    LapInfo l1 = {1, 30000, 30000};
    LapInfo l2 = {2, 61000, 31000};
    cfg.timer.laps = {l1, l2};

    // t=0：无计圈行（8 字形）
    auto q0 = BuildFrameOverlays(cfg, 0, 1920, 1080);
    CHECK(q0.size() == 8);
    // t=31s：显示 LAP 01
    auto q1 = BuildFrameOverlays(cfg, 31000000, 1920, 1080);
    CHECK(q1.size() > 8);
    // t=62s：显示 LAP 02（字形数不变，但内容不同——用尺寸不变验证数量一致性）
    auto q2 = BuildFrameOverlays(cfg, 62000000, 1920, 1080);
    CHECK(q2.size() == q1.size());
    // 计圈行在总时长行下方（rotation 0：第二行 y 更大）
    const DrawQuad &lapFirst = q1[8];
    CHECK(lapFirst.dstY > q1[0].dstY);
    FreeGlyphs(glyphs);
}

static void TestBlitter()
{
    printf("[blitter blend/rotation]\n");
    const int W = 100;
    const int H = 100;
    std::vector<uint8_t> frame(static_cast<size_t>(W) * H * 4, 0);

    // 半透明红色 10x10 贴到 (20,20)
    GlyphMeta red = SolidGlyph("x", 10, 10, 255, 0, 0);
    for (int i = 0; i < 10 * 10; i++) {
        const_cast<uint8_t *>(red.rgba)[i * 4 + 3] = 128; // 50%
    }
    DrawQuad q;
    q.src = red.rgba;
    q.srcW = 10;
    q.srcH = 10;
    q.dstX = 20;
    q.dstY = 20;
    q.dstW = 10;
    q.dstH = 10;
    q.rotSteps = 0;
    q.opacity = 1.0f;
    std::vector<DrawQuad> quads = {q};
    BlitQuads(frame.data(), W, H, quads);
    uint8_t r = frame[(25 * W + 25) * 4];
    CHECK(r > 120); // 50% 红 ≈ 127
    CHECK(frame[(25 * W + 25) * 4 + 1] == 0);
    CHECK(frame[(15 * W + 15) * 4] == 0); // 贴图外未受影响

    // 旋转 90°：源为 4x8 的“L”形，旋转后应变成 8x4（dst 尺寸由布局给定）
    std::vector<uint8_t> frame2(static_cast<size_t>(W) * H * 4, 0);
    GlyphMeta shape;
    shape.width = 4;
    shape.height = 8;
    uint8_t *s = new uint8_t[4 * 8 * 4];
    memset(s, 0, 4 * 8 * 4);
    // 源：左列全高（L 的竖），底行全宽
    for (int y = 0; y < 8; y++) {
        s[(y * 4 + 0) * 4 + 0] = 255;
        s[(y * 4 + 0) * 4 + 3] = 255;
    }
    for (int x = 0; x < 4; x++) {
        s[(7 * 4 + x) * 4 + 1] = 255;
        s[(7 * 4 + x) * 4 + 3] = 255;
    }
    shape.rgba = s;
    DrawQuad q2;
    q2.src = shape.rgba;
    q2.srcW = 4;
    q2.srcH = 8;
    q2.dstX = 50;
    q2.dstY = 50;
    q2.dstW = 8; // 旋转后宽=源高
    q2.dstH = 4;
    q2.rotSteps = 1;
    q2.opacity = 1.0f;
    std::vector<DrawQuad> quads2 = {q2};
    BlitQuads(frame2.data(), W, H, quads2);
    // 顺时针 90°：源左列（红） -> 目标顶行；源底行（绿） -> 目标左列
    CHECK(frame2[(50 * W + 54) * 4 + 0] == 255); // 顶行中部为红
    CHECK(frame2[(52 * W + 50) * 4 + 1] == 255); // 左列中部为绿
    delete[] s;
    delete[] red.rgba;
}

static void TestNv12Roundtrip()
{
    printf("[NV12<->RGBA roundtrip]\n");
    const int W = 64;
    const int H = 32;
    std::vector<uint8_t> rgba(static_cast<size_t>(W) * H * 4);
    // 渐变 + 灰
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            size_t i = (static_cast<size_t>(y) * W + x) * 4;
            if (x < W / 2) {
                rgba[i] = static_cast<uint8_t>(x * 4);
                rgba[i + 1] = static_cast<uint8_t>(y * 8);
                rgba[i + 2] = 128;
                rgba[i + 3] = 255;
            } else {
                rgba[i] = 100;
                rgba[i + 1] = 100;
                rgba[i + 2] = 100;
                rgba[i + 3] = 255;
            }
        }
    }
    std::vector<uint8_t> nv12(static_cast<size_t>(W) * H * 3 / 2);
    RgbaToNv12(rgba.data(), W, H, nv12.data());
    std::vector<uint8_t> back(static_cast<size_t>(W) * H * 4);
    Nv12ToRgba(nv12.data(), W, H, back.data(), false);
    int maxErr = 0;
    for (size_t i = 0; i < static_cast<size_t>(W) * H; i++) {
        for (int c = 0; c < 3; c++) {
            int err = std::abs(static_cast<int>(back[i * 4 + c]) - static_cast<int>(rgba[i * 4 + c]));
            if (err > maxErr) {
                maxErr = err;
            }
        }
    }
    printf("  max channel error = %d\n", maxErr);
    CHECK(maxErr <= 12); // limited-range 量化误差上限
}

int main()
{
    TestFormatTimer();
    TestMiniJson();
    TestOverlayRotation0();
    TestOverlayRotation90();
    TestLapLineSwitch();
    TestBlitter();
    TestNv12Roundtrip();
    printf("\n%d checks, %d failed\n", g_total, g_failed);
    return g_failed == 0 ? 0 : 1;
}
