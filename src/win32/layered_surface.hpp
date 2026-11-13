// DynamicIsland-Win / 分层窗口绘制表面（C++17 + GDI+）
//
// 职责边界：只负责「一块 32bpp 位图 + 提交到桌面」这件事，不含任何业务语义。
// 关键工程点：
//   1. UpdateLayeredWindow 要求 **预乘 alpha**，而 GDI+ 写入的是非预乘像素，
//      因此每帧提交前必须做一次 premultiply，否则边缘会出现亮描边。
//   2. 分层窗口必须在 ShowWindow 之前完成第一次 UpdateLayeredWindow，
//      否则会闪出一块纯黑矩形。
//   3. 本文件是 Windows 相关代码的唯一落点；核心域仍保持平台无关。
#ifndef DIW_SRC_WIN32_LAYERED_SURFACE_HPP
#define DIW_SRC_WIN32_LAYERED_SURFACE_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// windows.h 的 min/max 宏会破坏 std::min / std::max（编译期 C2589），必须关掉。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// GDI+ 头文件需要 IStream / PROPID，WIN32_LEAN_AND_MEAN 下必须先引入 objidl.h。
#include <objidl.h>
#include <gdiplus.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace diw {
namespace win32 {

// GDI+ 生命周期。必须是进程内唯一且存活时间覆盖所有绘图对象。
class GdiPlusSession {
public:
    GdiPlusSession() {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token_, &input, nullptr) != Gdiplus::Ok) {
            throw std::runtime_error("GdiplusStartup failed");
        }
    }
    ~GdiPlusSession() {
        if (token_ != 0) Gdiplus::GdiplusShutdown(token_);
    }
    GdiPlusSession(const GdiPlusSession&) = delete;
    GdiPlusSession& operator=(const GdiPlusSession&) = delete;

private:
    ULONG_PTR token_ = 0;
};

class LayeredSurface {
public:
    LayeredSurface() = default;
    ~LayeredSurface() { Release(); }
    LayeredSurface(const LayeredSurface&) = delete;
    LayeredSurface& operator=(const LayeredSurface&) = delete;

    void EnsureSize(int width, int height) {
        if (dib_ != nullptr && width == width_ && height == height_) return;
        if (width <= 0 || height <= 0) throw std::invalid_argument("surface size must be positive");
        Release();

        BITMAPINFO info;
        std::memset(&info, 0, sizeof(info));
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;  // 负高度 = 自上而下，避免手工翻行
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        HDC screen = ::GetDC(nullptr);
        mem_dc_ = ::CreateCompatibleDC(screen);
        ::ReleaseDC(nullptr, screen);
        if (mem_dc_ == nullptr) throw std::runtime_error("CreateCompatibleDC failed");

        void* bits = nullptr;
        dib_ = ::CreateDIBSection(mem_dc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dib_ == nullptr) throw std::runtime_error("CreateDIBSection failed");

        bits_ = static_cast<std::uint8_t*>(bits);
        previous_ = ::SelectObject(mem_dc_, dib_);
        width_ = width;
        height_ = height;
    }

    // 每帧起点：清成全透明。
    void Clear() {
        if (bits_ == nullptr) return;
        std::memset(bits_, 0, static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4u);
        dithered_ = false;
    }

    // 有序抖动（4x4 Bayer），幅度 ±1 LSB。
    //
    // 为什么必须有这一步：深色胶囊上的柔光是白色 alpha 0.10→0 的渐变，
    // 8bit 一共只有二十几级，每级在屏幕上占 4~6 行，于是渲染出来的是一圈圈
    // 同心色带 —— 截图放大能数出来，实物看着就是「廉价塑料感」。
    // GDI+ 没有抖动开关，只能在像素缓冲上补这一道。
    //
    // 用 Bayer 而非随机噪声：随机噪声会在静止画面上闪（每帧图案不同），
    // 有序抖动的图案与坐标绑定，画面静止时图案也静止，不会被看成"雪花"。
    // ±1 是刻意的保守选择：再大就能看出网格纹理了。
    void Dither() {
        if (bits_ == nullptr || dithered_) return;
        static constexpr int kBayer4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
        std::uint8_t* row = bits_;
        for (int y = 0; y < height_; ++y, row += static_cast<std::size_t>(width_) * 4u) {
            std::uint8_t* p = row;
            const int* pattern = kBayer4 + ((y & 3) << 2);
            for (int x = 0; x < width_; ++x, p += 4) {
                if (p[3] == 0u) continue;  // 全透明像素不抖动，省一半时间
                const int t = pattern[x & 3];
                const int d = (t < 5) ? -1 : (t > 10 ? 1 : 0);
                if (d == 0) continue;
                const int b = p[0] + d;
                const int g = p[1] + d;
                const int r = p[2] + d;
                p[0] = static_cast<std::uint8_t>(b < 0 ? 0 : (b > 255 ? 255 : b));
                p[1] = static_cast<std::uint8_t>(g < 0 ? 0 : (g > 255 ? 255 : g));
                p[2] = static_cast<std::uint8_t>(r < 0 ? 0 : (r > 255 ? 255 : r));
            }
        }
        dithered_ = true;
    }

    HDC Dc() const { return mem_dc_; }
    int Width() const { return width_; }
    int Height() const { return height_; }

    // BGRA 顺序做预乘：RGB *= A / 255。
    void Premultiply() {
        if (bits_ == nullptr) return;
        Dither();  // 先抖动再预乘：抖动量是 ±1 LSB，必须在量化发生的位置生效
        const std::size_t count = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
        std::uint8_t* p = bits_;
        for (std::size_t i = 0; i < count; ++i, p += 4) {
            const std::uint32_t a = p[3];
            if (a == 255u) continue;
            if (a == 0u) {
                p[0] = p[1] = p[2] = 0;
                continue;
            }
            p[0] = static_cast<std::uint8_t>((p[0] * a + 127u) / 255u);
            p[1] = static_cast<std::uint8_t>((p[1] * a + 127u) / 255u);
            p[2] = static_cast<std::uint8_t>((p[2] * a + 127u) / 255u);
        }
    }

    // 提交到桌面。global_alpha 为整体不透明度（0-255）。
    void Present(HWND hwnd, int x, int y, BYTE global_alpha) {
        Premultiply();
        POINT src{0, 0};
        SIZE size{width_, height_};
        POINT dst{x, y};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = global_alpha;
        blend.AlphaFormat = AC_SRC_ALPHA;
        if (!::UpdateLayeredWindow(hwnd, nullptr, &dst, &size, mem_dc_, &src, 0, &blend, ULW_ALPHA)) {
            throw std::runtime_error("UpdateLayeredWindow failed");
        }
    }

    // 屏幕抓取后位图的 alpha 字节是 0（BitBlt 不写 alpha），必须强制不透明，
    // 否则存出的 PNG 会整张透明。
    void ForceOpaqueAlpha() {
        if (bits_ == nullptr) return;
        const std::size_t count = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
        std::uint8_t* p = bits_;
        for (std::size_t i = 0; i < count; ++i, p += 4) p[3] = 255u;
    }

    // 把当前帧存成 PNG。用于无桌面会话下的确定性视觉验证（不等同实机观感）。
    void SavePng(const std::wstring& path) const {
        if (dib_ == nullptr) throw std::runtime_error("surface is empty");
        // 存图前也补一次抖动：屏幕上是抖过的，导出的 PNG 若原样保存，
        // 复盘时看到的色带会比实物更严重，容易误判成"没修好"。
        const_cast<LayeredSurface*>(this)->Dither();
        CLSID png{};
        if (!EncoderClsid(L"image/png", &png)) throw std::runtime_error("png encoder not found");
        Gdiplus::Bitmap bitmap(dib_, nullptr);
        if (bitmap.GetLastStatus() != Gdiplus::Ok) throw std::runtime_error("bitmap wrap failed");
        if (bitmap.Save(path.c_str(), &png, nullptr) != Gdiplus::Ok) {
            throw std::runtime_error("bitmap save failed");
        }
    }

private:
    static bool EncoderClsid(const wchar_t* mime, CLSID* out) {
        UINT num = 0;
        UINT size = 0;
        if (Gdiplus::GetImageEncodersSize(&num, &size) != Gdiplus::Ok || size == 0) return false;
        std::string raw(size, '\0');
        auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(&raw[0]);
        if (Gdiplus::GetImageEncoders(num, size, codecs) != Gdiplus::Ok) return false;
        for (UINT i = 0; i < num; ++i) {
            if (wcscmp(codecs[i].MimeType, mime) == 0) {
                *out = codecs[i].Clsid;
                return true;
            }
        }
        return false;
    }

    void Release() {
        if (mem_dc_ != nullptr) {
            if (previous_ != nullptr) ::SelectObject(mem_dc_, previous_);
            ::DeleteDC(mem_dc_);
        }
        if (dib_ != nullptr) ::DeleteObject(dib_);
        mem_dc_ = nullptr;
        dib_ = nullptr;
        previous_ = nullptr;
        bits_ = nullptr;
        width_ = 0;
        height_ = 0;
    }

    HDC mem_dc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    std::uint8_t* bits_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    bool dithered_ = false;  // 抖动与坐标绑定，同一帧重复调用无意义，用标志挡住
};

}  // namespace win32
}  // namespace diw

#endif  // DIW_SRC_WIN32_LAYERED_SURFACE_HPP
