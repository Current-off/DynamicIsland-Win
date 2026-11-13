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

#include <cstdio>
#include <string>
#include <vector>

#include "../src/win32/renderer.hpp"

using namespace diw::win32;

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
        CapsuleView v = Base("agent", "DynamicIsland-Win", "正在编辑 renderer.hpp");
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
