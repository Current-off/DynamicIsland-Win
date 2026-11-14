// balance_island / 玻璃背景捕获与模糊（C++17 + GDI+）
//
// 职责：截取「胶囊背后的那块屏幕」，降采样 + 多趟盒式模糊 ≈ 高斯，
// 交给渲染层做磨砂玻璃底。这是 Mac 风格透玻璃的唯一可靠路线：
//   - DWM 亚克力（SetWindowCompositionAttribute）与逐像素分层窗口
//     （UpdateLayeredWindow）不兼容，官方没有任何 API 能给 ULW 窗口
//     开启系统级背景模糊；
//   - 所以自己截、自己糊 —— 好处是模糊半径、染色、明暗全部可控，
//     且不依赖未文档化 API，Win10/11 行为一致。
//
// 两类捕获，语义不同：
//   1) 干净捕获（epoch）：胶囊在屏幕上完全透明时截取，内容 100% 干净。
//      用于「胶囊出现」的那一刻 —— 反正它还看不见，截屏不需要任何代价。
//   2) 带修补的刷新（refresh）：胶囊可见期间定期重截，此时胶囊自己挡在
//      镜头前。直接用会把胶囊糊进玻璃里（玻璃中间一块诡异的深色团）。
//      解法：先用 epoch 的像素把「当前窗口矩形」那一块填回去，再模糊。
//      代价是中心区域用的是 epoch 时刻的背景 —— 但它马上要被 60% 深色
//      染色盖住，肉眼完全看不出。
//
// 全程无闪烁：不需要「隐藏窗口一帧再截」的把戏。
#ifndef BI_SRC_WIN32_BACKDROP_HPP
#define BI_SRC_WIN32_BACKDROP_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// GDI+ 头文件需要 IStream / PROPID，WIN32_LEAN_AND_MEAN 下必须先引入 objidl.h。
#include <objidl.h>
#include <gdiplus.h>

#include <cstring>
#include <memory>
#include <vector>

namespace bi {
namespace win32 {

class Backdrop {
public:
    Backdrop() = default;
    Backdrop(const Backdrop&) = delete;
    Backdrop& operator=(const Backdrop&) = delete;

    // 截取屏幕 (x,y,w,h)（物理像素），模糊后供渲染层使用。
    //   occlude_x/y/w/h：胶囊窗口在「本截图坐标系」里的矩形（会被截进画面的部分）。
    //   epoch：同几何尺寸的干净捕获；非空时用它的像素修补 occlude 矩形。
    // 返回 false 表示截屏失败（无桌面会话 / 锁屏等），调用方应回落到不透明样式。
    // 后台抓屏线程产出的**纯数据快照**：只有像素数组与尺寸，不含任何 GDI+ 对象，
    // 因此可以安全跨线程传递，由主线程再包装成纹理。
    // 这是根治卡顿的关键 —— 抓屏 + 降采样 + 3 趟模糊全部在后台线程完成，
    // 主线程每帧只做一次指针/向量交换，不再被 30~100ms 的 BitBlt 阻塞。
    struct Snapshot {
        std::vector<std::uint8_t> small_px;  // 均衡档：重模糊 + 去饱和
        std::vector<std::uint8_t> lite_px;   // 液态玻璃档：轻模糊 + 保色
        int small_w = 0, small_h = 0;
        int full_w = 0, full_h = 0;
        int x = 0, y = 0;
        double raw_lum = 0.0, blur_lum = 0.0;
        double cost_ms = 0.0;
        bool ok = false;
    };

    // 把当前已算好的纹理导出成可跨线程传递的数据（只在后台线程调用）。
    void ExportSnapshot(Snapshot* out) const {
        out->small_px = small_.px;
        out->lite_px = lite_.px;
        out->small_w = small_.w;
        out->small_h = small_.h;
        out->full_w = full_.w;
        out->full_h = full_.h;
        out->x = x_;
        out->y = y_;
        out->raw_lum = MeanLuminanceRaw();
        out->blur_lum = MeanLuminance();
    }

    // 保存一次「干净背景」作为修补源（epoch）。只拷原始像素，不碰纹理与位图。
    void AdoptClean(const Backdrop& src) {
        full_ = src.full_;
        x_ = src.x_;
        y_ = src.y_;
        valid_ = true;
    }

    // 轻量内容探测：只把目标区域缩成 32×8 的小图算指纹，不做全尺寸拷贝。
    // 静止桌面时跳过每 2 秒一次的 2.3M 像素 BitBlt + 降采样 + 3 趟模糊
    //（issue #1 报告里「桌面几乎没变却反复全屏抓屏」那条）。
    // 采样失败时返回 true（保守当成"变了"），绝不因探测失败而漏更新。
    bool ContentChanged(int x, int y, int w, int h) {
        constexpr int kProbeW = 32;
        constexpr int kProbeH = 8;
        if (w <= 0 || h <= 0) return true;
        HDC screen = ::GetDC(nullptr);
        if (screen == nullptr) return true;
        HDC mem = ::CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = kProbeW;
        info.bmiHeader.biHeight = -kProbeH;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = (mem != nullptr)
                          ? ::CreateDIBSection(mem, &info, DIB_RGB_COLORS, &bits, nullptr, 0)
                          : nullptr;
        bool sampled = false;
        std::uint64_t hash = 1469598103934665603ULL;  // FNV-1a offset basis
        if (dib != nullptr && bits != nullptr) {
            const HGDIOBJ prev = ::SelectObject(mem, dib);
            ::SetStretchBltMode(mem, COLORONCOLOR);
            if (::StretchBlt(mem, 0, 0, kProbeW, kProbeH, screen, x, y, w, h, SRCCOPY)) {
                sampled = true;
                const auto* p = static_cast<const std::uint8_t*>(bits);
                for (std::size_t i = 0; i < static_cast<std::size_t>(kProbeW) * kProbeH * 4u; ++i) {
                    hash ^= p[i];
                    hash *= 1099511628211ULL;
                }
            }
            ::SelectObject(mem, prev);
            ::DeleteObject(dib);
        }
        if (mem != nullptr) ::DeleteDC(mem);
        ::ReleaseDC(nullptr, screen);
        if (!sampled) return true;
        const bool changed = (hash != probe_hash_);
        probe_hash_ = hash;
        return changed;
    }

    bool Capture(int x, int y, int w, int h, int occlude_x, int occlude_y, int occlude_w, int occlude_h,
                 const Backdrop* epoch) {
        if (w <= 0 || h <= 0) return false;
        if (!GrabScreen(x, y, w, h)) return false;
        // 记下抓屏原点：渲染层用 X()/Y() 算纹理偏移（overlay_main 的
        // glass.offset_x = X() - anchor_x）。这里不写的话它们恒为 0，
        // 取样区整体错位 glass_x —— 副屏上错约一整屏宽（issue #2 第 4 条）。
        x_ = x;
        y_ = y;

        // 修补：把胶囊挡住的区域填回 epoch 时刻的干净像素。
        if (epoch != nullptr && epoch->full_ == full_ && epoch->Valid()) {
            const int x0 = occlude_x < 0 ? 0 : occlude_x;
            const int y0 = occlude_y < 0 ? 0 : occlude_y;
            const int x1 = occlude_x + occlude_w > w ? w : occlude_x + occlude_w;
            const int y1 = occlude_y + occlude_h > h ? h : occlude_y + occlude_h;
            for (int row = y0; row < y1; ++row) {
                std::memcpy(Pixel(x0, row), epoch->Pixel(x0, row),
                            static_cast<std::size_t>(x1 - x0) * 4u);
            }
        }

        Blur();
        valid_ = true;
        return true;
    }

    bool Valid() const { return valid_; }
    int X() const { return x_; }
    int Y() const { return y_; }
    int FullWidth() const { return full_.w; }
    int FullHeight() const { return full_.h; }

    // 诊断用：模糊后纹理的平均亮度。玻璃内部若是纯黑（≈0），说明截到的是黑帧
    // （锁屏 / 无桌面会话 / 抓屏被拒），不是产品逻辑的问题。
    double MeanLuminance() const {
        if (small_.px.empty()) return 0.0;
        double sum = 0.0;
        std::size_t n = 0;
        for (std::size_t i = 0; i + 2 < small_.px.size(); i += 4) {
            sum += 0.2126 * small_.px[i + 2] + 0.7152 * small_.px[i + 1] + 0.0722 * small_.px[i + 0];
            ++n;
        }
        return n > 0 ? sum / static_cast<double>(n) : 0.0;
    }

    // 诊断用：抓屏原始像素的平均亮度（模糊前）。和 MeanLuminance() 一起看，
    // 能区分「没抓到」（raw 也黑）和「模糊写坏了」（raw 白、blur 黑）。
    double MeanLuminanceRaw() const {
        if (full_.px.empty()) return 0.0;
        double sum = 0.0;
        std::size_t n = 0;
        for (std::size_t i = 0; i + 2 < full_.px.size(); i += 4) {
            sum += 0.2126 * full_.px[i + 2] + 0.7152 * full_.px[i + 1] + 0.0722 * full_.px[i + 0];
            ++n;
        }
        return n > 0 ? sum / static_cast<double>(n) : 0.0;
    }

    // 供渲染层直接 DrawImage：已模糊的小图（约为全尺寸的 1/kDownsample），
    // 由 GDI+ 双线性放大 —— 放大本身就是模糊的一部分，小图直接就是最终纹理。
    Gdiplus::Bitmap* Blurred() {
        if (!valid_) return nullptr;
        if (small_bitmap_ == nullptr) small_bitmap_ = WrapSmall();
        return small_bitmap_.get();
    }

    // 液态玻璃专用的纹理：**轻模糊 + 保留色彩**。
    //
    // 玻璃之所以看起来像玻璃，一半来自"背后还能认出是什么"。Blurred() 为了
    // 让深色胶囊不抢内容，做了 3 趟重模糊 + 35% 去饱和 —— 米色壁纸进去、
    // 出来是一片均匀的灰，叠上染色就是"灰色塑料"。液态玻璃要的正好相反：
    // 纹理可以糊，色相必须留着（甚至更浓：玻璃会聚光）。
    Gdiplus::Bitmap* Liquid() {
        if (!valid_) return nullptr;
        if (lite_bitmap_ == nullptr) lite_bitmap_ = WrapLite();
        return lite_bitmap_.get();
    }

    // 渲染层需要知道小图映射回全尺寸时的实际覆盖范围（像素对齐用）。
    static constexpr int kDownsample = 4;

private:
    struct Buffer {
        int w = 0;
        int h = 0;
        std::vector<std::uint8_t> px;  // BGRA，自上而下
        bool operator==(const Buffer& other) const { return w == other.w && h == other.h; }
    };

    std::uint8_t* Pixel(int x, int y) { return full_.px.data() + (static_cast<std::size_t>(y) * full_.w + x) * 4u; }
    const std::uint8_t* Pixel(int x, int y) const {
        return full_.px.data() + (static_cast<std::size_t>(y) * full_.w + x) * 4u;
    }

    bool GrabScreen(int x, int y, int w, int h) {
        full_ = Buffer{};
        BITMAPINFO info;
        std::memset(&info, 0, sizeof(info));
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;  // 自上而下
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        HDC screen = ::GetDC(nullptr);
        if (screen == nullptr) return false;
        HDC mem = ::CreateCompatibleDC(screen);
        void* bits = nullptr;
        HBITMAP dib = (mem != nullptr) ? ::CreateDIBSection(mem, &info, DIB_RGB_COLORS, &bits, nullptr, 0)
                                       : nullptr;
        bool ok = false;
        if (dib != nullptr && bits != nullptr) {
            const HGDIOBJ prev = ::SelectObject(mem, dib);
            // 注意：**不能**加 CAPTUREBLT。带上它之后，凡是被本进程自己的
            // 分层窗口覆盖的像素都会抓成纯黑（实测 904x348 全黑，均值 0.5），
            // 而我们要抓的恰恰就是胶囊背后那块区域 —— 等于把整个玻璃糊成了黑膜。
            // 纯 SRCCOPY 抓到的就是屏幕上看得见的东西：胶囊透明时是桌面，
            // 胶囊可见时是「桌面 + 胶囊」，后者由调用方用 epoch 像素修补。
            ok = ::BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY) != FALSE;
            ::SelectObject(mem, prev);
            if (ok) {
                full_.w = w;
                full_.h = h;
                full_.px.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
                std::memcpy(full_.px.data(), bits, full_.px.size());
                // BitBlt 不写 alpha；玻璃是逐像素混色，alpha 统一 255。
                for (std::size_t i = 3; i < full_.px.size(); i += 4) full_.px[i] = 255u;
            }
            ::DeleteObject(dib);
        }
        if (mem != nullptr) ::DeleteDC(mem);
        ::ReleaseDC(nullptr, screen);
        return ok;
    }

    // 降采样 4x（块平均）+ 3 趟半径 9 的可分离盒式模糊 + 25% 去饱和。
    //
    // 模糊量是实测调出来的，不是拍的：半径 3 时背后标签页的标题字仍然可辨
    //（σ≈8 物理像素 ≈ 4 逻辑像素），观感像"蒙了层灰玻璃"而不是磨砂玻璃。
    // 半径 14 → σ≈27 物理像素 ≈ 13.5 逻辑像素：色块边界、图标轮廓都被揉平，
    // 只留下大块的色相与明暗 —— 这才是"磨砂"，不是"半透明窗帘"。
    //
    // 去饱和同样重要：花壁纸的饱和色会透过玻璃"跳"出来抢内容注意力，
    // 向亮度混合 25% 后颜色还在（能看出是壁纸），但不再刺眼。
    void Blur() {
        const int sw = full_.w / kDownsample;
        const int sh = full_.h / kDownsample;
        small_ = Buffer{};
        if (sw <= 0 || sh <= 0) return;
        small_.w = sw;
        small_.h = sh;
        small_.px.resize(static_cast<std::size_t>(sw) * static_cast<std::size_t>(sh) * 4u);

        for (int sy = 0; sy < sh; ++sy) {
            for (int sx = 0; sx < sw; ++sx) {
                int b = 0, g = 0, r = 0;
                const int base_x = sx * kDownsample;
                const int base_y = sy * kDownsample;
                for (int dy = 0; dy < kDownsample; ++dy) {
                    const std::uint8_t* row = Pixel(base_x, base_y + dy);
                    for (int dx = 0; dx < kDownsample; ++dx, row += 4) {
                        b += row[0];
                        g += row[1];
                        r += row[2];
                    }
                }
                const int n = kDownsample * kDownsample;
                std::uint8_t* dst = small_.px.data() + (static_cast<std::size_t>(sy) * sw + sx) * 4u;
                dst[0] = static_cast<std::uint8_t>(b / n);
                dst[1] = static_cast<std::uint8_t>(g / n);
                dst[2] = static_cast<std::uint8_t>(r / n);
                dst[3] = 255u;
            }
        }

        // 先留一份「刚降采样、还没重模糊」的副本给液态玻璃。
        // 降采样本身已经是 4×4 均值，等于一趟轻模糊 —— 这正好是想要的粒度：
        // 细节糊掉但色块轮廓还在。
        lite_ = small_;

        for (int pass = 0; pass < 3; ++pass) {
            BoxBlurAxis(small_, true, 14);
            BoxBlurAxis(small_, false, 14);
        }
        // 去饱和 35%：花壁纸/彩色窗口透进来时，饱和色会"抢"内容。
        // macOS 的磨砂本质上就是「把背后的颜色揉成一团哑光」，而不是留一层彩色滤镜。
        Desaturate(small_, 0.35f);

        // 液态玻璃版：一趟中等半径模糊，**不去饱和**，反而把饱和度提 20%
        // —— 玻璃聚光，透过去的颜色本来就比背景更浓。
        //
        // 半径不能太小：5 的时候背后窗口的标题栏边线会清晰地透过来，玻璃上
        // 凭空多出一条横线，看着像"没渲染完"。9 既抹掉了硬边界，又保住了
        // 大块的色相（和均衡档的 3 趟 ×14 完全不是一个量级）。
        BoxBlurAxis(lite_, true, 9);
        BoxBlurAxis(lite_, false, 9);
        Saturate(lite_, 0.20f);

        small_bitmap_.reset();  // 下一帧重新包装
        lite_bitmap_.reset();
    }

    // 与 Desaturate 反向：把颜色从灰度往原色推。
    static void Saturate(Buffer& buf, float amount) {
        const float k = 1.0f + amount;
        for (std::size_t i = 0; i + 3 < buf.px.size(); i += 4) {
            const float b = buf.px[i + 0];
            const float g = buf.px[i + 1];
            const float r = buf.px[i + 2];
            const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            const float nb = lum + (b - lum) * k;
            const float ng = lum + (g - lum) * k;
            const float nr = lum + (r - lum) * k;
            buf.px[i + 0] = static_cast<std::uint8_t>(nb < 0.0f ? 0.0f : (nb > 255.0f ? 255.0f : nb));
            buf.px[i + 1] = static_cast<std::uint8_t>(ng < 0.0f ? 0.0f : (ng > 255.0f ? 255.0f : ng));
            buf.px[i + 2] = static_cast<std::uint8_t>(nr < 0.0f ? 0.0f : (nr > 255.0f ? 255.0f : nr));
        }
    }

    static void Desaturate(Buffer& buf, float amount) {
        for (std::size_t i = 0; i + 3 < buf.px.size(); i += 4) {
            const float b = buf.px[i + 0];
            const float g = buf.px[i + 1];
            const float r = buf.px[i + 2];
            const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            buf.px[i + 0] = static_cast<std::uint8_t>(b + (lum - b) * amount);
            buf.px[i + 1] = static_cast<std::uint8_t>(g + (lum - g) * amount);
            buf.px[i + 2] = static_cast<std::uint8_t>(r + (lum - r) * amount);
        }
    }

    // 可分离盒模糊（单方向一趟）。滑动窗口累加和，O(n) 与半径无关。
    //
    // 边缘处理是这种实现的生死线。旧版在这里栽过两次跟头：
    //   1) 输出指针不随 x 前进 → 每行只写第一个像素，整块玻璃全黑
    //      （probes/backdrop_probe 单测暴露）；
    //   2) 窗口在左缘是「扩张」的（lo 钉在 0 不再左移），此时绝不能
    //      按 x-radius 去减一个根本不在窗口里的元素。旧代码在 x < radius
    //      期间每步都减 v[0]，半径 14 时累计多减 15 次，累加和带上
    //      -15*v[0] 的恒定偏差 —— 桌面左上角是浅色时（v[0]≈253）偏差
    //      高达 -3800，除完落在负数，转回 uint8_t 回绕成随机彩色块。
    //      实测表现：raw≈253 的白桌面，blur 均值 118，玻璃纹理却是
    //      饱和红绿蓝马赛克（且随截屏内容漂移，时好时坏）。
    //   正确做法与 renderer.hpp 的 BoxBlurAxis 同源：clo/chi 两根指针，
    //   元素真正进出窗口时才加减，除数恒等于窗口内实际样本数。
    static void BoxBlurAxis(Buffer& buf, bool horizontal, int radius) {
        const int w = buf.w;
        const int h = buf.h;
        const int len = horizontal ? w : h;
        const int lines = horizontal ? h : w;
        if (len <= 1 || radius <= 0) return;
        std::vector<std::uint8_t> tmp(buf.px.size());
        const std::size_t stride = static_cast<std::size_t>(w) * 4u;
        for (int line = 0; line < lines; ++line) {
            const std::uint8_t* src;
            std::uint8_t* dst;
            std::size_t step;
            if (horizontal) {
                const std::size_t row = static_cast<std::size_t>(line) * stride;
                src = buf.px.data() + row;
                dst = tmp.data() + row;
                step = 4u;
            } else {
                src = buf.px.data() + static_cast<std::size_t>(line) * 4u;
                dst = tmp.data() + static_cast<std::size_t>(line) * 4u;
                step = stride;
            }
            int acc[3] = {0, 0, 0};
            int clo = 0;  // 窗口左端（含），滑出时才减
            int chi = (radius < len - 1 ? radius : len - 1);  // 窗口右端（含）
            for (int i = clo; i <= chi; ++i) {
                const std::uint8_t* p = src + static_cast<std::size_t>(i) * step;
                acc[0] += p[0];
                acc[1] += p[1];
                acc[2] += p[2];
            }
            for (int x = 0; x < len; ++x) {
                const int lo = (x - radius > 0) ? x - radius : 0;
                const int hi = (x + radius < len - 1) ? x + radius : len - 1;
                while (chi < hi) {  // 右缘滑入
                    ++chi;
                    const std::uint8_t* p = src + static_cast<std::size_t>(chi) * step;
                    acc[0] += p[0];
                    acc[1] += p[1];
                    acc[2] += p[2];
                }
                while (clo < lo) {  // 左缘滑出
                    const std::uint8_t* p = src + static_cast<std::size_t>(clo) * step;
                    acc[0] -= p[0];
                    acc[1] -= p[1];
                    acc[2] -= p[2];
                    ++clo;
                }
                const int count = hi - lo + 1;  // 窗口内实际样本数
                std::uint8_t* out = dst + static_cast<std::size_t>(x) * step;
                out[0] = static_cast<std::uint8_t>(acc[0] / count);
                out[1] = static_cast<std::uint8_t>(acc[1] / count);
                out[2] = static_cast<std::uint8_t>(acc[2] / count);
                out[3] = 255u;
            }
        }
        buf.px.swap(tmp);
    }

    std::unique_ptr<Gdiplus::Bitmap> WrapSmall() {
        auto bitmap = std::make_unique<Gdiplus::Bitmap>(small_.w, small_.h, PixelFormat32bppARGB);
        if (bitmap->GetLastStatus() != Gdiplus::Ok) return nullptr;
        Gdiplus::BitmapData data;
        const Gdiplus::Rect rect(0, 0, small_.w, small_.h);
        if (bitmap->LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &data) !=
            Gdiplus::Ok) {
            return nullptr;
        }
        const std::size_t row_bytes = static_cast<std::size_t>(small_.w) * 4u;
        for (int y = 0; y < small_.h; ++y) {
            std::memcpy(static_cast<std::uint8_t*>(data.Scan0) + static_cast<std::size_t>(y) * data.Stride,
                        small_.px.data() + static_cast<std::size_t>(y) * row_bytes, row_bytes);
        }
        bitmap->UnlockBits(&data);
        return bitmap;
    }

    std::unique_ptr<Gdiplus::Bitmap> WrapLite() {
        auto bitmap = std::make_unique<Gdiplus::Bitmap>(lite_.w, lite_.h, PixelFormat32bppARGB);
        if (bitmap->GetLastStatus() != Gdiplus::Ok) return nullptr;
        Gdiplus::BitmapData data;
        const Gdiplus::Rect rect(0, 0, lite_.w, lite_.h);
        if (bitmap->LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &data) !=
            Gdiplus::Ok) {
            return nullptr;
        }
        const std::size_t row_bytes = static_cast<std::size_t>(lite_.w) * 4u;
        for (int y = 0; y < lite_.h; ++y) {
            std::memcpy(static_cast<std::uint8_t*>(data.Scan0) + static_cast<std::size_t>(y) * data.Stride,
                        lite_.px.data() + static_cast<std::size_t>(y) * row_bytes, row_bytes);
        }
        bitmap->UnlockBits(&data);
        return bitmap;
    }

    Buffer full_;   // 截屏原始像素（修补在这一层做，必须在模糊之前）
    Buffer small_;  // 模糊后的纹理（均衡档采样它）
    Buffer lite_;   // 轻模糊、保色的纹理（液态玻璃采样它）
    std::unique_ptr<Gdiplus::Bitmap> small_bitmap_;
    std::unique_ptr<Gdiplus::Bitmap> lite_bitmap_;
    int x_ = 0, y_ = 0;
    bool valid_ = false;
    std::uint64_t probe_hash_ = 0;  // 上次探测到的桌面内容指纹
};

}  // namespace win32
}  // namespace bi

#endif  // BI_SRC_WIN32_BACKDROP_HPP
