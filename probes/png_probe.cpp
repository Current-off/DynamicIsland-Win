// 读取 PNG 指定列的像素值，用来判断「胶囊内部的横向条带」到底是真实的
// 8bit 渐变量化，还是截图/压缩产生的伪影。
//
// 为什么不用现成工具：本机没有 PIL，PowerShell 的 Add-Type 被安全策略拦截，
// 而项目本来就在用 GDI+，读一张 PNG 是最短路径。
//
// 用法：png_probe.exe <文件> <x> [y0] [y1]      读一列（查色带）
//       png_probe.exe <文件> row:<y> [x0] [x1]  读一行（查玻璃是否真透出）

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: png_probe.exe <file.png> <x> [y0] [y1]\n");
        return 1;
    }

    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
        std::printf("gdiplus init failed\n");
        return 1;
    }

    {
        Gdiplus::Bitmap bmp(argv[1], FALSE);
        if (bmp.GetLastStatus() != Gdiplus::Ok) {
            std::printf("cannot open png\n");
            Gdiplus::GdiplusShutdown(token);
            return 1;
        }

        // row 模式：png_probe.exe <file> row:<y> [x0] [x1]
        // 玻璃效果的核心判据是「同一行像素有没有横向变化」：
        // 不透明底是常数，真玻璃会跟随背后内容起伏。用标准差说话，不靠眼睛。
        const std::wstring arg2(argv[2]);
        if (arg2.rfind(L"row:", 0) == 0) {
            const int y = _wtoi(arg2.c_str() + 4);
            const int x0 = argc > 3 ? _wtoi(argv[3]) : 0;
            const int x1 = argc > 4 ? _wtoi(argv[4]) : static_cast<int>(bmp.GetWidth());
            double sum = 0.0, sum_sq = 0.0;
            int n = 0;
            int lum_min = 255, lum_max = 0;
            for (int x = x0; x < x1; ++x) {
                Gdiplus::Color c;
                if (bmp.GetPixel(x, y, &c) != Gdiplus::Ok) break;
                const double lum = 0.2126 * c.GetR() + 0.7152 * c.GetG() + 0.0722 * c.GetB();
                sum += lum;
                sum_sq += lum * lum;
                const int rounded = static_cast<int>(lum + 0.5);
                if (rounded < lum_min) lum_min = rounded;
                if (rounded > lum_max) lum_max = rounded;
                ++n;
            }
            if (n == 0) {
                std::printf("no pixels read\n");
                return 1;  // 不在这里 shutdown：bmp 还在作用域里，析构会踩到已关闭的 GDI+
            }
            const double mean = sum / n;
            const double variance = sum_sq / n - mean * mean;
            const double sd = variance > 0.0 ? sqrt(variance) : 0.0;
            std::printf("row y=%d x=%d..%d n=%d  lum mean=%.1f sd=%.2f min=%d max=%d\n", y, x0, x1,
                        n, mean, sd, lum_min, lum_max);
            return 0;  // 同上：让 bmp 先析构，进程退出时再释放 GDI+

        }

        const int x = _wtoi(argv[2]);
        const int y0 = argc > 3 ? _wtoi(argv[3]) : 0;
        const int y1 = argc > 4 ? _wtoi(argv[4]) : static_cast<int>(bmp.GetHeight());

        std::printf("size=%ux%u  column x=%d  rows %d..%d\n", bmp.GetWidth(), bmp.GetHeight(), x,
                    y0, y1);

        int prev = -1;
        int run_start = y0;
        for (int y = y0; y < y1; ++y) {
            Gdiplus::Color c;
            if (bmp.GetPixel(x, y, &c) != Gdiplus::Ok) break;
            const int v = c.GetR();
            if (prev < 0) {
                prev = v;
                run_start = y;
                continue;
            }
            if (v != prev) {
                // 只打印持续 2 行以上的「平台」，单行跳变通常是抗锯齿边缘。
                if (y - run_start >= 2) {
                    std::printf("  y=%3d..%3d  R=%-3d  (%d 行)\n", run_start, y - 1, prev,
                                y - run_start);
                }
                prev = v;
                run_start = y;
            }
        }
        if (y1 - run_start >= 2) {
            std::printf("  y=%3d..%3d  R=%-3d  (%d 行)\n", run_start, y1 - 1, prev, y1 - run_start);
        }
    }

    Gdiplus::GdiplusShutdown(token);
    return 0;
}
