// 渲染层可测部分：不创建窗口、不初始化 GDI+，只验证纯函数与跨层常量契约。
// 视觉正确性无法用断言替代（见 CONTRIBUTING 的 L3 视觉验收），但下面这些必须先绿。
#include <cstdio>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

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
    CHECK(bi::win32::ParseProgress(R"({"progress":0.62})") == 0.62);
    CHECK(bi::win32::ParseProgress(R"({"a":1,"progress":0.5,"b":2})") == 0.5);
    CHECK(bi::win32::ParseProgress(R"({"progress": 1})") == 1.0);
    CHECK(bi::win32::ParseProgress(R"({"progress": 0})") == 0.0);
    // 缺失、越界、非数字、空串都必须回落到 fallback，不能让渲染层拿到脏值。
    CHECK(bi::win32::ParseProgress(R"({"other":1})") == -1.0);
    CHECK(bi::win32::ParseProgress(R"({"progress":1.5})") == -1.0);
    CHECK(bi::win32::ParseProgress(R"({"progress":-0.2})") == -1.0);
    CHECK(bi::win32::ParseProgress(R"({"progress":"high"})") == -1.0);
    CHECK(bi::win32::ParseProgress("") == -1.0);
    CHECK(bi::win32::ParseProgress(R"({"progress":abc})", 0.25) == 0.25);
}

// 渲染层布局常量必须落在核心域声明的合法区间内，否则展开态会被设计约束悄悄破坏。
void TestLayoutContract() {
    CHECK(bi::IsValidExpandedHeight(bi::win32::Layout::kExpandedH));
    CHECK(bi::win32::Layout::kExpandedW > bi::win32::Layout::kCompactW);
    CHECK(bi::win32::Layout::kCompactH > 0.0f);
    CHECK(bi::win32::Layout::kDetachedSize > 0.0f);
    CHECK(bi::win32::Layout::kMargin * 2.0f < bi::win32::Layout::kCompactW);
    // 分离小胶囊的高度不能超过主胶囊，否则视觉上会「比主位还重」。
    CHECK(bi::win32::Layout::kDetachedSize <= bi::win32::Layout::kCompactH);
}

void TestWiden() {
    CHECK(bi::win32::Widen("").empty());
    CHECK(bi::win32::Widen("abc") == L"abc");
    // 中文必须能无损往返，否则标题会渲染成方块。
    const std::wstring zh = bi::win32::Widen("产品站会");
    CHECK(zh.size() == 4);
    CHECK(bi::win32::Widen("晴天") == L"晴天");
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
    using namespace bi::win32;
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

// 把一张位图的像素读成字节序列，用于逐字节比较。
std::vector<std::uint8_t> BytesOf(Gdiplus::Bitmap& bmp) {
    const int w = static_cast<int>(bmp.GetWidth());
    const int h = static_cast<int>(bmp.GetHeight());
    Gdiplus::BitmapData data{};
    const Gdiplus::Rect rect(0, 0, w, h);
    if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok) {
        return {};
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    const auto* src = static_cast<const std::uint8_t*>(data.Scan0);
    const std::size_t stride =
        static_cast<std::size_t>(data.Stride < 0 ? -data.Stride : data.Stride);
    for (int y = 0; y < h; ++y) {
        std::memcpy(out.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4u,
                    src + static_cast<std::size_t>(y) * stride,
                    static_cast<std::size_t>(w) * 4u);
    }
    bmp.UnlockBits(&data);
    return out;
}

// 铬层与内容「分两次画」必须与「一次画完」逐字节相同。
//
// 为什么这条比"看起来一样"更值得测：主循环的铬层缓存正是靠这个等式成立的 ——
// 命中时它把上一次存下的铬层像素 memcpy 回来，再只画内容。一旦两段的绘制顺序、
// 裁剪关系或质量档位被改动，画面就会出现"底对了、字错位"这类只在某几种布局下
// 才露头的偏差，而这种偏差人眼在深色胶囊上极难发现。这里用逐字节比较兜住它。
void TestPhaseSplit() {
    using namespace bi::win32;
    Gdiplus::GdiplusStartupInput gdi_input;
    ULONG_PTR gdi_token = 0;
    if (Gdiplus::GdiplusStartup(&gdi_token, &gdi_input, nullptr) != Gdiplus::Ok) {
        Check(false, "GdiplusStartup failed", __LINE__);
        return;
    }
    {
        // 画布要容得下最宽的场景（展开态 + 分离小圆 + HUD 余量）。
        const int cw = 1000;
        const int chh = 420;

        // 固定的合成背景：玻璃材质必须喂一张确定的纹理，否则与"同一台机器上
        // 两次渲染比对"这个前提就破了（真实桌面抓图每次都不一样）。
        auto backdrop = std::make_unique<Gdiplus::Bitmap>(64, 64, PixelFormat32bppARGB);
        {
            Gdiplus::Graphics bg(backdrop.get());
            for (int y = 0; y < 64; ++y) {
                for (int x = 0; x < 64; ++x) {
                    const bool on = ((x / 8) + (y / 8)) % 2 == 0;
                    Gdiplus::SolidBrush b(Gdiplus::Color(255, on ? 210 : 40, on ? 180 : 60,
                                                         on ? 60 : 150));
                    bg.FillRectangle(&b, x, y, 1, 1);
                }
            }
        }

        auto quality = [](Gdiplus::Graphics& g) {
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
            g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        };

        // split=true 时用两个 Graphics 分两段画，模拟主循环的缓存路径。
        auto render = [&](const CapsuleView& v, bool with_glass, bool split, bool content_only) {
            auto bmp = std::make_unique<Gdiplus::Bitmap>(cw, chh, PixelFormat32bppARGB);
            CapsuleRenderer renderer;
            GlassParams glass;
            if (with_glass) {
                glass.backdrop = backdrop.get();
                glass.full_w = 64.0f;
                glass.full_h = 64.0f;
            }
            {
                Gdiplus::Graphics g(bmp.get());
                quality(g);
                if (!split) {
                    renderer.Draw(g, v, 1.0f, glass);
                } else if (content_only) {
                    renderer.Draw(g, v, 1.0f, glass, CapsuleRenderer::Phase::Content);
                } else {
                    renderer.Draw(g, v, 1.0f, glass, CapsuleRenderer::Phase::Chrome);
                }
            }
            if (split && !content_only) {
                Gdiplus::Graphics g(bmp.get());
                quality(g);
                renderer.Draw(g, v, 1.0f, glass, CapsuleRenderer::Phase::Content);
            }
            return bmp;
        };

        auto base_media = [](int mode) {
            CapsuleView v;
            v.opacity_mode = mode;
            v.content_w = Layout::kCompactW;
            v.content_h = Layout::kCompactH;
            v.title = "晴天";
            v.subtitle = "周杰伦 · 叶惠美";
            v.meta = "网易云音乐";
            v.kind = "media";
            v.playing = true;
            v.progress = 0.42;
            v.wave_phase = 1.7;
            v.more_count = 2;
            return v;
        };

        CapsuleView compact = base_media(2);
        DetachedItem detached;
        detached.kind = "notification";
        detached.activity_id = "live-notif";
        compact.detached_w = Layout::kDetachedSize;
        compact.detached_items.push_back(detached);

        CapsuleView agent = compact;
        agent.detached_items.clear();
        agent.detached_w = 0.0;
        agent.agent = true;
        agent.kind = "agent";
        agent.agent_provider = "claude";
        agent.agent_state = "running";
        agent.agent_action = "正在编辑 renderer.hpp";
        agent.agent_meta = "3/7";

        CapsuleView expanded = base_media(2);
        expanded.expanded = true;
        expanded.content_w = Layout::kExpandedW;
        expanded.content_h = Layout::kExpandedLyricH;
        expanded.lyric_active = true;
        expanded.lyric_status = "ok";
        expanded.lyric_prev = "故事的小黄花";
        expanded.lyric_cur = "从出生那年就飘着";
        expanded.lyric_next = "童年的荡秋千";

        CapsuleView overview = base_media(2);
        overview.overview = true;
        for (int i = 0; i < 3; ++i) {
            OverviewItem item;
            item.kind = (i == 0) ? "media" : (i == 1) ? "browser" : "agent";
            item.title = "任务标题";
            item.subtitle = "副标题 · 说明";
            item.state = "running";
            overview.items.push_back(item);
        }

        CapsuleView sleep = base_media(2);
        sleep.sleep_bar = true;
        sleep.playing = false;

        CapsuleView glassY = base_media(1);
        CapsuleView liquid = base_media(0);

        struct Case {
            const char* name;
            CapsuleView view;
            bool glass;
        };
        const Case cases[] = {
            {"compact+detached", compact, false}, {"agent", agent, false},
            {"expanded", expanded, false},        {"overview", overview, false},
            {"sleep_bar", sleep, false},          {"glass-balanced", glassY, true},
            {"glass-liquid", liquid, true},
        };

        for (const auto& c : cases) {
            const auto whole = render(c.view, c.glass, false, false);
            const auto split = render(c.view, c.glass, true, false);
            const auto a = BytesOf(*whole);
            const auto b = BytesOf(*split);
            const bool equal = !a.empty() && a == b;
            ++g_run;
            if (!equal) {
                ++g_failed;
                std::size_t first = 0;
                while (first < a.size() && first < b.size() && a[first] == b[first]) ++first;
                std::printf("FAIL phase split [%s]: first divergence at byte %zu\n", c.name, first);
            }
        }

        // 反向确认：只画内容必须与画完整不同，否则上面的等式可能是"两段都空"的假绿。
        const auto whole = render(compact, false, false, false);
        const auto content_only = render(compact, false, true, true);
        CHECK(BytesOf(*whole) != BytesOf(*content_only));
    }
    Gdiplus::GdiplusShutdown(gdi_token);
}

int main() {
    TestParseProgress();
    TestLayoutContract();
    TestWiden();
    TestGlassBody();
    TestPhaseSplit();
    std::printf("render_tests: run=%d failed=%d\n", g_run, g_failed);
    return g_failed == 0 ? 0 : 1;
}
