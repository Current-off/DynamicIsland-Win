// 白色背景窗：给玻璃效果做「最苛刻背景」的视觉验证。
//
// 为什么需要它：真实桌面今天是深色终端，明天可能是浅色编辑器，
// 而玻璃最难看的场景恰恰是**大面积浅色背景**（透出来的白会把白字冲掉）。
// 每次靠手工摆窗口既不稳定也不可复现，所以做成探针：
// 一个铺满屏幕上半部分的白色窗口，里面画几条彩色色块与文字 ——
// 胶囊压上去之后，模糊是否生效、文字是否还读得清，一眼就能判断。
//
// 用法：white_bg_probe.exe [毫秒数，默认 12000]
// 该窗口不置顶、不抢焦点，胶囊（topmost）自然压在它上面。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>

namespace {

int g_quit_after_ms = 12000;
int g_screen_w = 1920;  // wmain 里按 DPI 感知后的真实宽度写入

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = ::BeginPaint(hwnd, &ps);
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            HBRUSH white = ::CreateSolidBrush(RGB(250, 250, 252));
            ::FillRect(dc, &rc, white);
            ::DeleteObject(white);

            // 四块高饱和色块，**压在胶囊正后方**（以屏幕中线为基准摆放）：
            // 只有色块真的在胶囊背后，"玻璃有没有透出颜色"才是肉眼可判的。
            const COLORREF colors[4] = {RGB(220, 60, 70), RGB(60, 120, 220), RGB(60, 190, 120),
                                       RGB(240, 180, 50)};
            const int center_x = g_screen_w / 2;
            const int band_y0 = 20;
            const int band_y1 = 260;
            for (int i = 0; i < 4; ++i) {
                HBRUSH brush = ::CreateSolidBrush(colors[i]);
                RECT box{center_x - 460 + i * 230, band_y0, center_x - 250 + i * 230, band_y1};
                ::FillRect(dc, &box, brush);
                ::DeleteObject(brush);
            }

            // 一行文字：用来判断「背后的字有没有被糊掉」。
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, RGB(30, 30, 34));
            ::TextOutW(dc, g_screen_w / 2 - 460, 360,
                       L"玻璃背景测试：这行字在胶囊内部应当完全不可读，只留下柔和的明暗。", 36);
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            ::PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default: return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1) g_quit_after_ms = _wtoi(argv[1]);
    if (g_quit_after_ms <= 0) g_quit_after_ms = 12000;

    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"BI_WhiteBackground";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    ::RegisterClassExW(&wc);

    // 必须声明 DPI 感知：否则 GetSystemMetrics 返回的是缩放后的逻辑尺寸，
    // 窗口会被 DWM 二次缩放，色块最终落到胶囊左边而不是背后（实测踩过）。
    using SetCtx = BOOL(WINAPI*)(HANDLE);
    if (auto fn = (SetCtx)::GetProcAddress(::GetModuleHandleW(L"user32.dll"),
                                           "SetProcessDpiAwarenessContext")) {
        fn((HANDLE)(static_cast<intptr_t>(-4)));
    }
    const int screen_w = ::GetSystemMetrics(SM_CXSCREEN);
    g_screen_w = screen_w;
    HWND hwnd = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
                                  L"DIW white background", WS_POPUP, 0, 0, screen_w, 560, nullptr,
                                  nullptr, instance, nullptr);
    if (hwnd == nullptr) return 1;

    ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    ::SetTimer(hwnd, 1, static_cast<UINT>(g_quit_after_ms), nullptr);

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return 0;
}
