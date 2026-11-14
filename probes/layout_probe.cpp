// 展开态尺寸探针：把每一种展开态各渲染一张 PNG，用来判断「底部是否太空 /
// 是否被裁切」。存在的理由很具体 —— 只看代码判断不出留白，而留白问题
// 已经出现过两次（日历、通知/剪贴板/电量），每次都是渲染出来并排看才定位到。
//
//   build\cpp\layout_probe.exe             → 输出到 build\layout\
//   build\cpp\layout_probe.exe  D:\tmp\lay → 输出到指定目录
//
// 判读方法：打开 PNG 看文字块是否垂直居中、上下留白是否接近；
// 同时下面的尺寸表会告诉你每一类的胶囊高度，便于横向比较。
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/win32/renderer.hpp"

using namespace bi::win32;

namespace {

CapsuleView Base(const char* kind, const char* title, const char* sub) {
    CapsuleView v;
    v.opacity_mode = 2;
    v.kind = kind;
    v.title = title;
    v.subtitle = sub;
    v.alpha = 1.0;
    v.wave_phase = 1.2;
    return v;
}

bool SavePng(Gdiplus::Bitmap& bmp, const std::wstring& path) {
    CLSID png{};
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<std::uint8_t> raw(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(raw.data());
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
            png = codecs[i].Clsid;
            return bmp.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok;
        }
    }
    return false;
}

struct Case {
    std::string name;
    CapsuleView view;
};

}  // namespace

int main(int argc, char** argv) {
    std::string out_dir = (argc > 1) ? argv[1] : "build/layout";

    Gdiplus::GdiplusStartupInput in;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &in, nullptr) != Gdiplus::Ok) {
        std::printf("GdiplusStartup failed\n");
        return 1;
    }
    ::CreateDirectoryW(std::wstring(out_dir.begin(), out_dir.end()).c_str(), nullptr);

    std::vector<Case> cases;

    // ---- 紧凑态 ----
    {
        CapsuleView v = Base("media", "晴天", "周杰伦 · 叶惠美");
        v.meta = "网易云音乐";
        v.playing = true;
        v.progress = 0.42;
        v.content_w = Layout::kCompactW;
        v.content_h = Layout::kCompactH;
        cases.push_back({"compact_media", v});
    }
    {
        CapsuleView v = Base("agent", "balance_island", "正在编辑 renderer.hpp");
        v.agent = true;
        v.agent_provider = "claude";
        v.agent_name = "Claude Code";
        v.agent_state = "running";
        v.agent_meta = "3/7";
        v.content_w = Layout::kCompactW;
        v.content_h = Layout::kCompactH;
        cases.push_back({"compact_agent", v});
    }

    // ---- 展开态：带进度条 / 带歌词 ----
    {
        CapsuleView v = Base("media", "晴天", "周杰伦 · 叶惠美");
        v.expanded = true;
        v.playing = true;
        v.progress = 0.42;
        v.meta = "1:23 / 3:45";
        v.content_w = Layout::kExpandedW;
        v.content_h = Layout::kExpandedH;
        cases.push_back({"expanded_media", v});
    }
    {
        CapsuleView v = Base("media", "晴天", "周杰伦 · 叶惠美");
        v.expanded = true;
        v.playing = true;
        v.progress = 0.42;
        v.lyric_active = true;
        v.lyric_status = "ok";
        v.lyric_prev = "故事的小黄花";
        v.lyric_cur = "从出生那年就飘着";
        v.lyric_next = "童年的荡秋千";
        v.content_w = Layout::kExpandedW;
        v.content_h = Layout::kExpandedLyricH;
        cases.push_back({"expanded_media_lyric", v});
    }

    // ---- 展开态：通用短档（这次改动的对象） ----
    {
        CapsuleView v = Base("notification", "Chrome", "下载完成 · report.pdf");
        v.expanded = true;
        v.content_w = Layout::kExpandedW;
        v.content_h = Layout::kExpandedShortH;
        cases.push_back({"expanded_notification", v});
    }
    {
        CapsuleView v = Base("clipboard", "已复制 137 字", "来自 Chrome");
        v.expanded = true;
        v.content_w = Layout::kExpandedW;
        v.content_h = Layout::kExpandedShortH;
        cases.push_back({"expanded_clipboard", v});
    }
    {
        CapsuleView v = Base("power", "电量 23%", "建议插电");
        v.expanded = true;
        v.content_w = Layout::kExpandedW;
        v.content_h = Layout::kExpandedShortH;
        cases.push_back({"expanded_power", v});
    }
    {
        CapsuleView v = Base("calendar", "产品站会", "15:00 – 15:30 · 会议室 A");
        v.expanded = true;
        v.content_w = Layout::kExpandedW;
        v.content_h = Layout::kExpandedShortH;
        cases.push_back({"expanded_calendar", v});
    }

    // ---- 短展开态（无进度条）：图标 42px 占 18..60，页脚那一行会不会压上去 ----
    {
        CapsuleView v = Base("clipboard", "已复制", "微信 · 这段话被复制了");
        v.meta = "微信";
        v.expanded = true;
        v.content_w = Layout::kExpandedW;
        v.content_h = Layout::kExpandedShortH;
        cases.push_back({"expanded_clipboard", v});
    }
    {
        CapsuleView v = Base("notification", "微信", "张三：晚上一起吃饭吗？");
        v.meta = "微信";
        v.expanded = true;
        v.content_w = Layout::kExpandedW;
        v.content_h = Layout::kExpandedShortH;
        cases.push_back({"expanded_notification", v});
    }
    {
        CapsuleView v = Base("power", "电量 23%", "建议插电");
        v.meta = "23%";
        v.expanded = true;
        v.content_w = Layout::kExpandedW;
        v.content_h = Layout::kExpandedShortH;
        cases.push_back({"expanded_power_meta", v});
    }
    // ---- 紧凑态对照 ----
    {
        CapsuleView v = Base("clipboard", "已复制", "");
        v.meta = "Microsoft Edge";
        v.content_w = Layout::kCompactW;
        v.content_h = Layout::kCompactH;
        cases.push_back({"compact_clipboard", v});
    }

    // ---- 其余可视状态 ----
    {
        CapsuleView v = Base("media", "", "");
        v.sleep_bar = true;
        v.playing = false;
        v.content_w = Layout::kCompactW;
        v.content_h = Layout::kSleepBarH;
        cases.push_back({"sleep_bar", v});
    }
    {
        CapsuleView v = Base("", "", "");
        v.alive_dot = true;
        v.alpha = 0.26;
        v.content_w = Layout::kSleepBarH;
        v.content_h = Layout::kSleepBarH;
        cases.push_back({"alive_dot", v});
    }

    // ---- 展开动画基准：宽度逐帧变化时，重绘一帧要多久 ----
    //
    // 这条基准存在的理由很具体：展开动画期间几何每帧都变，铬层缓存必然失效，
    // 所以「动画卡不卡」= 「一次完整铬层重绘要多久」。桌面上的实测会被全屏判定、
    // 活动是否存活等外部状态干扰，这里直接量渲染器本身。
    if (argc > 1 && std::string(argv[1]) == "--bench") {
        // 用媒体展开态做基准：几何最简单（图标 + 两行文字 + 进度条 + 走带），
        // 但铬层路径与其它展开态完全一致（阴影 + 玻璃 + 内外描边）。
        CapsuleView v = Base("media", "晴天", "周杰伦 · 叶惠美");
        v.expanded = true;
        v.playing = true;
        v.progress = 0.42;
        v.meta = "1:23 / 3:45";
        v.content_h = Layout::kExpandedH;
        Gdiplus::Bitmap surface(960, 640, PixelFormat32bppARGB);
        CapsuleRenderer renderer;
        double worst = 0.0;
        double total = 0.0;
        int frames = 0;
        for (int i = 0; i <= 40; ++i) {
            const float t = static_cast<float>(i) / 40.0f;
            v.content_w = Layout::kCompactW + (Layout::kExpandedW - Layout::kCompactW) * t;
            const auto t0 = std::chrono::steady_clock::now();
            {
                Gdiplus::Graphics g(&surface);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
                g.SetCompositingQuality(Gdiplus::CompositingQualityHighSpeed);
                g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
                g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                renderer.Draw(g, v, 2.0f, GlassParams{}, CapsuleRenderer::Phase::Chrome);
                renderer.Draw(g, v, 2.0f, GlassParams{}, CapsuleRenderer::Phase::Content);
            }
            const double ms =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 1000.0;
            worst = std::max(worst, ms);
            total += ms;
            ++frames;
        }
        // 这里**不能**调 GdiplusShutdown：surface / renderer 持有 GDI+ 位图，
        // 会在 return 时析构，而那一刻 GDI+ 已经关了 → 释放失效句柄 → 访问冲突
        // （实测 0xC0000005，且 stdout 缓冲全丢，看起来像「程序什么都没输出」）。
        // 进程退出的清理交给系统，比在这里手动排序安全。
        std::printf("bench: frames=%d avg=%.2fms worst=%.2fms (no backdrop)\n", frames,
                    total / frames, worst);
        std::printf("       last frame: shadow=%.0fms body=%.0fms glass=%.0fms\n",
                    renderer.last_shadow_ms, renderer.last_body_ms,
                    renderer.last_glassbody_ms);
        std::fflush(stdout);
        return 0;
    }

    std::printf("%-24s %s\n", "case", "canvas");
    int ok = 0;
    for (const auto& c : cases) {
        const int w = static_cast<int>(c.view.content_w + Layout::kMargin * 2);
        const int h = static_cast<int>(c.view.content_h + Layout::kMargin * 2);
        Gdiplus::Bitmap surface(w, h, PixelFormat32bppARGB);
        CapsuleRenderer renderer;
        {
            Gdiplus::Graphics g(&surface);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
            g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            renderer.Draw(g, c.view, 1.0f);
        }
        const std::wstring path = std::wstring(out_dir.begin(), out_dir.end()) + L"\\" +
                                  std::wstring(c.name.begin(), c.name.end()) + L".png";
        const bool saved = SavePng(surface, path);
        std::printf("%-24s %4dx%-4d  %s\n", c.name.c_str(), w, h, saved ? "ok" : "FAIL");
        if (saved) ++ok;
    }

    Gdiplus::GdiplusShutdown(token);
    std::printf("\n%d/%zu 张已写入 %s\n", ok, cases.size(), out_dir.c_str());
    return ok == static_cast<int>(cases.size()) ? 0 : 1;
}
