// 渲染层可测部分：不创建窗口、不初始化 GDI+，只验证纯函数与跨层常量契约。
// 视觉正确性无法用断言替代（见 CONTRIBUTING 的 L3 视觉验收），但下面这些必须先绿。
#include <cstdio>
#include <cmath>
#include <memory>
#include <string>

#include "../core/activity.hpp"
#include "../src/win32/renderer.hpp"

namespace {

int g_run = 0;
int g_failed = 0;

void Check(bool cond, const char* expr, int line) {
    ++g_run;
    if (!cond) {
        ++g_failed;
        std::printf("FAIL line %d: %s\n", line, expr);
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

void TestParseProgress() {
    CHECK(diw::win32::ParseProgress(R"({"progress":0.62})") == 0.62);
    CHECK(diw::win32::ParseProgress(R"({"a":1,"progress":0.5,"b":2})") == 0.5);
    CHECK(diw::win32::ParseProgress(R"({"progress": 1})") == 1.0);
    CHECK(diw::win32::ParseProgress(R"({"progress": 0})") == 0.0);
    // 缺失、越界、非数字、空串都必须回落到 fallback，不能让渲染层拿到脏值。
    CHECK(diw::win32::ParseProgress(R"({"other":1})") == -1.0);
    CHECK(diw::win32::ParseProgress(R"({"progress":1.5})") == -1.0);
    CHECK(diw::win32::ParseProgress(R"({"progress":-0.2})") == -1.0);
    CHECK(diw::win32::ParseProgress(R"({"progress":"high"})") == -1.0);
    CHECK(diw::win32::ParseProgress("") == -1.0);
    CHECK(diw::win32::ParseProgress(R"({"progress":abc})", 0.25) == 0.25);
}

// 渲染层布局常量必须落在核心域声明的合法区间内，否则展开态会被设计约束悄悄破坏。
void TestLayoutContract() {
    CHECK(diw::IsValidExpandedHeight(diw::win32::Layout::kExpandedH));
    CHECK(diw::win32::Layout::kExpandedW > diw::win32::Layout::kCompactW);
    CHECK(diw::win32::Layout::kCompactH > 0.0f);
    CHECK(diw::win32::Layout::kDetachedSize > 0.0f);
    CHECK(diw::win32::Layout::kMargin * 2.0f < diw::win32::Layout::kCompactW);
    // 分离小胶囊的高度不能超过主胶囊，否则视觉上会「比主位还重」。
    CHECK(diw::win32::Layout::kDetachedSize <= diw::win32::Layout::kCompactH);
}

void TestWiden() {
    CHECK(diw::win32::Widen("").empty());
    CHECK(diw::win32::Widen("abc") == L"abc");
    // 中文必须能无损往返，否则标题会渲染成方块。
    const std::wstring zh = diw::win32::Widen("产品站会");
    CHECK(zh.size() == 4);
    CHECK(diw::win32::Widen("晴天") == L"晴天");
}

}  // namespace

// 玻璃底的可测部分。
//
// 方法：用「同一份内容 + 两块不同背景」做因果对照，而不是拿一条像素行的
// 统计量硬比 —— 胶囊里有图标、标题、进度条，它们对像素的贡献远大于背景，
// 任何基于「方差大小」的断言都会被这些固定元素污染（踩过一次：图标本身
// 就是高饱和色块，直接让"左侧比右侧红"恒成立）。
//
// 正确的因果链条是：
//   换背景 → 玻璃输出跟着变；不透明输出纹丝不动。
//   白底与黑底 → 玻璃输出差一大截，但白底仍然被压暗（60% 染色）。
void TestGlassBody() {
    using namespace diw::win32;
    // 本用例需要真实 GDI+ 合成：就地初始化，不起来整个窗口栈。
    Gdiplus::GdiplusStartupInput gdi_input;
    ULONG_PTR gdi_token = 0;
    if (Gdiplus::GdiplusStartup(&gdi_token, &gdi_input, nullptr) != Gdiplus::Ok) {
        Check(false, "GdiplusStartup failed", __LINE__);
        return;
    }

    // GDI+ 对象必须全部在本作用域内析构 —— 它们比 GdiplusShutdown 活得久会直接崩。
    {
    const int w = 400;
    const int h = 120;

    auto make_backdrop = [&](Gdiplus::Color color) {
        auto bmp = std::make_unique<Gdiplus::Bitmap>(w, h, PixelFormat32bppARGB);
        Gdiplus::Graphics bg(bmp.get());
        Gdiplus::SolidBrush brush(color);
        bg.FillRectangle(&brush, 0, 0, w, h);
        return bmp;
    };
    auto red = make_backdrop(Gdiplus::Color(255, 200, 40, 40));
    auto blue = make_backdrop(Gdiplus::Color(255, 40, 40, 200));
    auto white = make_backdrop(Gdiplus::Color(255, 255, 255, 255));
    auto black = make_backdrop(Gdiplus::Color(255, 0, 0, 0));

    CapsuleView view;
    view.alpha = 1.0;
    view.title = "test";

    auto render = [&](Gdiplus::Bitmap* backdrop, bool with_glass) {
        auto bmp = std::make_unique<Gdiplus::Bitmap>(
            static_cast<int>(Layout::kCompactW + Layout::kMargin * 2),
            static_cast<int>(Layout::kCompactH + Layout::kMargin * 2), PixelFormat32bppARGB);
        Gdiplus::Graphics g(bmp.get());
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        CapsuleRenderer renderer;
        GlassParams glass;
        if (with_glass) {
            glass.backdrop = backdrop;
            glass.offset_x = 0.0f;
            glass.offset_y = 0.0f;
            glass.full_w = static_cast<float>(w);
            glass.full_h = static_cast<float>(h);
        }
        renderer.Draw(g, view, 1.0f, glass);
        return bmp;
    };

    // 只统计胶囊内部（避开描边与投影），整块取平均 —— 内容完全相同，
    // 因此任何差异只能来自背景。
    auto mean_lum = [](Gdiplus::Bitmap* bmp) {
        double sum = 0.0;
        int n = 0;
        for (int y = static_cast<int>(Layout::kMargin) + 5;
             y < static_cast<int>(Layout::kMargin + Layout::kCompactH) - 5; ++y) {
            for (int x = static_cast<int>(Layout::kMargin) + 5;
                 x < static_cast<int>(Layout::kMargin + Layout::kCompactW) - 5; ++x) {
                Gdiplus::Color c;
                if (bmp->GetPixel(x, y, &c) != Gdiplus::Ok) continue;
                sum += 0.2126 * c.GetR() + 0.7152 * c.GetG() + 0.0722 * c.GetB();
                ++n;
            }
        }
        return n > 0 ? sum / n : 0.0;
    };

    const auto glass_red = render(red.get(), true);
    const auto glass_blue = render(blue.get(), true);
    const auto glass_white = render(white.get(), true);
    const auto glass_black = render(black.get(), true);
    const auto opaque_red = render(red.get(), false);
    const auto opaque_blue = render(blue.get(), false);

    const double g_red = mean_lum(glass_red.get());
    const double g_blue = mean_lum(glass_blue.get());
    const double g_white = mean_lum(glass_white.get());
    const double g_black = mean_lum(glass_black.get());
    const double o_red = mean_lum(opaque_red.get());
    const double o_blue = mean_lum(opaque_blue.get());

    std::printf("glass  red=%.1f blue=%.1f white=%.1f black=%.1f\n", g_red, g_blue, g_white, g_black);
    std::printf("opaque red=%.1f blue=%.1f\n", o_red, o_blue);

    // 1) 换背景，玻璃输出必须变（红底亮度 74、蓝底 51，透过 40% 后差约 9）。
    Check(std::fabs(g_red - g_blue) > 4.0, "glass output follows backdrop", __LINE__);
    // 2) 同一个对照下，不透明底必须完全不受背景影响。
    Check(std::fabs(o_red - o_blue) < 1.5, "opaque ignores backdrop", __LINE__);
    // 3) 白底 vs 黑底差一大截 —— 证明背景是真的被画进来了，而不是只有一层固定色。
    Check(g_white - g_black > 25.0, "glass transmits light from backdrop", __LINE__);
    // 4) 白底仍然被压暗：理论值 0.4*255 + 0.6*13 ≈ 110，加高光与文字后放宽到 200。
    Check(g_white < 200.0, "glass tint darkens bright backdrop", __LINE__);
    // 5) 玻璃与不透明是两条真实不同的路径。
    Check(std::fabs(g_white - o_red) > 3.0, "glass and opaque differ", __LINE__);

    }
    Gdiplus::GdiplusShutdown(gdi_token);
}

int main() {
    TestParseProgress();
    TestLayoutContract();
    TestWiden();
    TestGlassBody();
    std::printf("render_tests: run=%d failed=%d\n", g_run, g_failed);
    return g_failed == 0 ? 0 : 1;
}
