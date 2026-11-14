// 玻璃背景探针：脱离桌面窗口，单独验证「截屏 → 降采样 → 模糊」这段管线。
//
// 为什么需要它：胶囊里的玻璃是黑的时，原因可能在三个地方 ——
//   1. BitBlt 没抓到（raw 黑）；
//   2. 模糊写坏了（raw 白、模糊黑）；
//   3. 渲染层没把纹理画上去。
// 只看最终截图无法区分。这个探针把 1、2 两步独立出来，并把模糊结果存成 PNG，
// 用 png_probe 读像素即可定论。
//
// 用法：backdrop_probe.exe <x> <y> <w> <h> <out.png>
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../src/win32/backdrop.hpp"

namespace {

bool SaveBitmap(Gdiplus::Bitmap* bitmap, const wchar_t* path) {
    if (bitmap == nullptr) return false;
    CLSID png{};
    UINT num = 0, size = 0;
    if (Gdiplus::GetImageEncodersSize(&num, &size) != Gdiplus::Ok || size == 0) return false;
    std::string raw(size, '\0');
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(&raw[0]);
    if (Gdiplus::GetImageEncoders(num, size, codecs) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
            png = codecs[i].Clsid;
            break;
        }
    }
    return bitmap->Save(path, &png, nullptr) == Gdiplus::Ok;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 6) {
        std::printf("usage: backdrop_probe.exe <x> <y> <w> <h> <out.png>\n");
        return 1;
    }
    const int x = _wtoi(argv[1]);
    const int y = _wtoi(argv[2]);
    const int w = _wtoi(argv[3]);
    const int h = _wtoi(argv[4]);

    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
        std::printf("gdiplus init failed\n");
        return 1;
    }

    int exit_code = 0;
    {
        bi::win32::Backdrop backdrop;
        const bool ok = backdrop.Capture(x, y, w, h, 0, 0, 0, 0, nullptr);
        std::printf("capture=%d  raw=%.1f  blur=%.1f\n", ok ? 1 : 0,
                    backdrop.MeanLuminanceRaw(), backdrop.MeanLuminance());
        if (ok) {
            // 采样打印已移除（问题定位完成后不再需要）

            if (SaveBitmap(backdrop.Blurred(), argv[5])) {
                std::printf("saved %ls\n", argv[5]);
            } else {
                std::printf("save failed\n");
                exit_code = 2;
            }
        } else {
            exit_code = 3;
        }
    }
    Gdiplus::GdiplusShutdown(token);
    return exit_code;
}
