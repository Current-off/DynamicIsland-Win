// 设置面板探针：把四个分页离屏渲染成 PNG，并逐块检查「画了没 / 点得到吗」。
//
// 为什么需要它：
//   v0.3.0 把二十个字段加进了 BuildZones()，却忘了在 Paint() 里画出来 ——
//   结果用户打开设置只看到三个旧控件，剩下的全是空白，而 `--selftest` 的
//   命中表全绿（区域都在，只是没人画）。这类 bug 用眼睛看代码看不出来，
//   必须真的把像素渲染出来比对。
//
// 两种用法：
//   settings_probe.exe             → 出图到 build/settings/（人眼看版式）
//   settings_probe.exe --check     → 只做检查，不可视化的控件直接点名，非零退出
//
// 检查项：
//   ① 命中表：区域中心必须命中它自己（与 --selftest 同一条规则）
//   ② 越界：区域必须在窗口内（布局比窗口高 = 底部按钮和最后几行看不见）
//   ③ 可视化：区域内的像素必须与「只画背景」的同一张图不同 ——
//      差异为 0 就是「有点不到、也看不见」的隐形控件
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/win32/settings_ui.hpp"

using namespace bi::win32;
using bi::win32::settings_ui::Hit;
using bi::win32::settings_ui::State;
using bi::win32::settings_ui::Zone;

namespace {

struct Canvas {
    int w = 0;
    int h = 0;
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HBITMAP old = nullptr;
    void* bits = nullptr;

    bool Create(int width, int height) {
        w = width;
        h = height;
        HDC screen = ::GetDC(nullptr);
        dc = ::CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height;  // 自上而下，行号与屏幕一致
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ::ReleaseDC(nullptr, screen);
        if (bmp == nullptr) return false;
        old = static_cast<HBITMAP>(::SelectObject(dc, bmp));
        return true;
    }

    void Destroy() {
        if (dc != nullptr) {
            ::SelectObject(dc, old);
            ::DeleteObject(bmp);
            ::DeleteDC(dc);
            dc = nullptr;
        }
    }

    const std::uint32_t* Pixels() const { return static_cast<const std::uint32_t*>(bits); }
};

bool SavePng(const Canvas& c, const std::wstring& path) {
    CLSID png{};
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<std::uint8_t> raw(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(raw.data());
    Gdiplus::GetImageEncoders(num, size, codecs);
    Gdiplus::Bitmap img(c.w, c.h, c.w * 4, PixelFormat32bppARGB,
                        static_cast<BYTE*>(c.bits));
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
            png = codecs[i].Clsid;
            return img.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok;
        }
    }
    return false;
}

void Render(const State& st, Canvas& c) {
    settings_ui::Paint(const_cast<State&>(st), c.dc, c.w, c.h);
}

std::wstring W(const char* utf8) { return bi::win32::Widen(utf8); }

const char* HitName(Hit h) {
    switch (h) {
        case Hit::Opacity0: return "Opacity0";
        case Hit::Opacity1: return "Opacity1";
        case Hit::Opacity2: return "Opacity2";
        case Hit::Anchor0: return "Anchor0";
        case Hit::Anchor1: return "Anchor1";
        case Hit::Anchor2: return "Anchor2";
        case Hit::Glass: return "Glass";
        case Hit::Fullscreen: return "Fullscreen";
        case Hit::Secret: return "Secret";
        case Hit::QuietOn: return "QuietOn";
        case Hit::QuietFromDec: return "QuietFromDec";
        case Hit::QuietFromInc: return "QuietFromInc";
        case Hit::QuietToDec: return "QuietToDec";
        case Hit::QuietToInc: return "QuietToInc";
        case Hit::OpenConfig: return "OpenConfig";
        case Hit::Save: return "Save";
        case Hit::Cancel: return "Cancel";
        case Hit::Close: return "Close";
        case Hit::TitleBar: return "TitleBar";
        case Hit::TabAppearance: return "Tab:外观";
        case Hit::TabBehavior: return "Tab:行为";
        case Hit::TabPerformance: return "Tab:性能";
        case Hit::TabPreset: return "Tab:预设";
        case Hit::Theme0: return "Theme0";
        case Hit::Theme1: return "Theme1";
        case Hit::Theme2: return "Theme2";
        case Hit::Accent0: return "Accent0";
        case Hit::Accent1: return "Accent1";
        case Hit::Accent2: return "Accent2";
        case Hit::Accent3: return "Accent3";
        case Hit::Accent4: return "Accent4";
        case Hit::Accent5: return "Accent5";
        case Hit::UiScaleDec: return "UiScale-";
        case Hit::UiScaleInc: return "UiScale+";
        case Hit::FontOffsetDec: return "FontOffset-";
        case Hit::FontOffsetInc: return "FontOffset+";
        case Hit::CornerDec: return "Corner-";
        case Hit::CornerInc: return "Corner+";
        case Hit::TopOffsetDec: return "TopOffset-";
        case Hit::TopOffsetInc: return "TopOffset+";
        case Hit::AutoHideFocus: return "AutoHideFocus";
        case Hit::AutoHidePaused: return "AutoHidePaused";
        case Hit::AutoHideFullscreen: return "AutoHideFullscreen";
        case Hit::QuietForm0: return "QuietForm0";
        case Hit::QuietForm1: return "QuietForm1";
        case Hit::QuietForm2: return "QuietForm2";
        case Hit::YieldEnabled: return "YieldEnabled";
        case Hit::YieldKey0: return "YieldKey0";
        case Hit::YieldKey1: return "YieldKey1";
        case Hit::YieldKey2: return "YieldKey2";
        case Hit::ReduceMotion: return "ReduceMotion";
        case Hit::LowPowerDowngrade: return "LowPowerDowngrade";
        case Hit::GlassQuality0: return "GlassQuality0";
        case Hit::GlassQuality1: return "GlassQuality1";
        case Hit::GlassQuality2: return "GlassQuality2";
        case Hit::GlassQuality3: return "GlassQuality3";
        case Hit::CaptureRefreshDec: return "Refresh-";
        case Hit::CaptureRefreshInc: return "Refresh+";
        case Hit::TargetMonitor0: return "Monitor0";
        case Hit::TargetMonitor1: return "Monitor1";
        case Hit::TargetMonitor2: return "Monitor2";
        case Hit::MonitorIndexDec: return "MonIdx-";
        case Hit::MonitorIndexInc: return "MonIdx+";
        case Hit::Preset0: return "Preset0";
        case Hit::Preset1: return "Preset1";
        case Hit::Preset2: return "Preset2";
        case Hit::Preset3: return "Preset3";
        case Hit::PresetReset: return "PresetReset";
        default: break;
    }
    if (settings_ui::EventIndex(h) >= 0) return "Event";
    return "?";
}

std::string NarrowAscii(const char* s) { return std::string(s); }

}  // namespace

int main(int argc, char** argv) {
    bool check = false;
    bool want_png_cramped = false;
    std::string out_dir = "build/settings";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--check") {
            check = true;
        } else if (a == "--cramped") {
            want_png_cramped = true;  // 额外出一组「受限高度 + 滚动」的图
        } else {
            out_dir = a;
        }
    }

    Gdiplus::GdiplusStartupInput in;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &in, nullptr) != Gdiplus::Ok) {
        std::printf("GdiplusStartup failed\n");
        return 1;
    }
    if (!check) ::CreateDirectoryW(W(out_dir.c_str()).c_str(), nullptr);

    int failures = 0;
    int zones_total = 0;
    int invisible_total = 0;

    // 两轮：
    //   ① 自然高度 —— 版式是否正确、每个控件是否真的画出来了
    //   ② 受限高度 —— 模拟 1080p + 高缩放（工作区放不下）：必须出现滚动，
    //      且底部按钮仍在窗口内，否则「点不到保存」比「少画一个开关」更严重
    std::printf("%-4s %-5s %-4s %-12s %-7s %s\n", "page", "dpi", "mode", "size", "zones",
                "verdict");
    for (int dpi : {96, 120, 144, 168}) {
        for (int page = 0; page < settings_ui::kPageCount; ++page) {
            for (int cramped = 0; cramped < 2; ++cramped) {
                if (cramped == 1 && dpi < 120) continue;  // 96dpi 下本来就放得下

                State st;
                st.dpi = dpi;
                st.page = page;
                st.cfg = bi::RuleConfig();
                st.path = L"C://Users//you//AppData//Roaming//balance_island//rules.json";
                st.width = settings_ui::S(settings_ui::layout::kWidth, dpi);
                st.height = 0;
                st.max_height = cramped ? settings_ui::S(660, 96) : 0;  // 约 1040 物理像素
                const int natural = settings_ui::BuildZones(st, st.zones);
                const int win_h = st.height > 0 ? st.height : natural;
                st.height = win_h;
                if (cramped) settings_ui::BuildZones(st, st.zones);

                int unclickable = 0;
                int outside = 0;
                for (const auto& z : st.zones) {
                    if (z.hit == Hit::TitleBar) continue;
                    ++zones_total;
                    const int cx = (z.rc.left + z.rc.right) / 2;
                    const int cy = (z.rc.top + z.rc.bottom) / 2;
                    if (settings_ui::ZoneAt(st, cx, cy) != z.hit) {
                        ++unclickable;
                        std::printf("  ! 点不到: %-20s (page %d dpi %d)\n", HitName(z.hit), page,
                                    dpi);
                    }
                    if (z.rc.right > st.width || z.rc.bottom > win_h || z.rc.left < 0 ||
                        z.rc.top < 0) {
                        ++outside;
                        std::printf("  ! 越界: %-20s (%d,%d)-(%d,%d) 窗口 %dx%d\n",
                                    HitName(z.hit), static_cast<int>(z.rc.left),
                                    static_cast<int>(z.rc.top), static_cast<int>(z.rc.right),
                                    static_cast<int>(z.rc.bottom), st.width, win_h);
                    }
                }
                // 底部按钮必须真的在窗口里 —— 这条单独盯，因为它决定能不能保存。
                for (Hit h : {Hit::Save, Hit::Cancel}) {
                    const RECT* r = settings_ui::FindRect(st, h);
                    if (r == nullptr || r->bottom > win_h) {
                        ++outside;
                        std::printf("  ! 按钮出界: %s\n", HitName(h));
                    }
                }

                int invisible = 0;
                {
                    Canvas full;
                    Canvas bare;
                    if (full.Create(st.width, win_h) && bare.Create(st.width, win_h)) {
                        Render(st, full);
                        State empty = st;
                        empty.zones.clear();
                        Render(empty, bare);
                        const std::uint32_t* a = full.Pixels();
                        const std::uint32_t* b = bare.Pixels();
                        for (const auto& z : st.zones) {
                            // TitleBar 没有自己的图形；Close 的 × 是无条件画的（不来自
                            // zones），这两者在「只画背景」的对照图里也存在，比不出差异。
                            if (z.hit == Hit::TitleBar || z.hit == Hit::Close) continue;
                            int diff = 0;
                            for (int y = z.rc.top; y < z.rc.bottom && diff == 0; ++y) {
                                for (int x = z.rc.left; x < z.rc.right; ++x) {
                                    if (a[y * full.w + x] != b[y * bare.w + x]) {
                                        ++diff;
                                        break;
                                    }
                                }
                            }
                            if (diff == 0) {
                                ++invisible;
                                ++invisible_total;
                                std::printf("  ! 没画出来: %-20s (page %d dpi %d)\n",
                                            HitName(z.hit), page, dpi);
                            }
                        }
                        if (!check && (!cramped || want_png_cramped)) {
                            char name[80];
                            std::snprintf(name, sizeof(name), "page%d_%ddpi%s.png", page, dpi,
                                          cramped ? "_cramped" : "");
                            SavePng(full, W((out_dir + "/" + name).c_str()));
                        }
                    } else {
                        std::printf("canvas create failed\n");
                        ++failures;
                    }
                    full.Destroy();
                    bare.Destroy();
                }

                char size[48];
                std::snprintf(size, sizeof(size), "%dx%d%s", st.width, win_h,
                              st.max_scroll > 0 ? "+scroll" : "");
                const bool ok = unclickable == 0 && outside == 0 && invisible == 0;
                std::printf("%-4d %-5d %-4s %-12s %-7zu %s\n", page, dpi,
                            cramped ? "窄" : "全", size, st.zones.size(), ok ? "ok" : "FAIL");
                if (!ok) ++failures;
            }
        }
    }

    std::printf("\n共 %d 个区域，隐形 %d，失败组合 %d\n", zones_total, invisible_total, failures);
    Gdiplus::GdiplusShutdown(token);
    return failures == 0 ? 0 : 1;
}
