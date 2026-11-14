// balance_island / 异步玻璃管线
//
// 根治「笔记本卡顿」的核心改动。原先抓屏 + 降采样 + 3 趟盒模糊全部同步跑在
// UI 线程上，单次 30~100ms（150% 缩放时更久），表现为动画不跟手、周期性顿挫。
// issue #1 已用 epoch 节流把阻塞占比从 15.2% 降到 1.7%，但那是降低**频率**；
// 这里把这一步整体搬到后台线程，主线程不再有任何抓屏阻塞。
//
// 设计取舍（都是刻意的）：
//  1. 后台线程只产出**纯像素数组**，绝不创建 GDI+ 对象 —— GDI+ 位图由主线程
//     在 GlassTexture 里包装，避免跨线程使用 GDI+ 带来的不确定性。
//  2. 单槽请求 + 单槽结果，新请求直接覆盖旧请求、新结果直接覆盖旧结果。
//     玻璃是"越新越好"的数据，排队反而会把旧帧一张张送到屏幕上。
//  3. 结果与请求都用 std::move 交接，不拷贝大数组；主线程每帧只做一次
//     mutex + 指针交换（微秒级）。
//  4. epoch（干净背景）与 work_ 都归后台线程私有，主线程完全不碰，
//     因此不需要锁住整块像素缓冲。
//  5. 失败降级：抓屏失败时 ok=false，主线程沿用上一张纹理或不透明样式。
#ifndef BI_SRC_WIN32_GLASS_ASYNC_HPP
#define BI_SRC_WIN32_GLASS_ASYNC_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "backdrop.hpp"

namespace bi {
namespace win32 {

// ---------------------------------------------------------------- 主线程侧纹理
//
// 持有后台交付的像素，按需包装成 GDI+ 位图。Adopt() 会让缓存失效，
// 下一次 Blurred()/Liquid() 重新 LockBits 灌入新像素。
class GlassTexture {
public:
    void Adopt(const Backdrop::Snapshot& snap) {
        snap_ = snap;
        small_bitmap_.reset();
        lite_bitmap_.reset();
    }

    void Clear() {
        snap_ = Backdrop::Snapshot{};
        small_bitmap_.reset();
        lite_bitmap_.reset();
    }

    bool Ready() const {
        return snap_.ok && snap_.small_w > 0 && snap_.small_h > 0 &&
               !snap_.small_px.empty();
    }

    int X() const { return snap_.x; }
    int Y() const { return snap_.y; }
    int FullWidth() const { return snap_.full_w; }
    int FullHeight() const { return snap_.full_h; }

    Gdiplus::Bitmap* Blurred() { return Wrap(snap_.small_px, small_bitmap_); }
    Gdiplus::Bitmap* Liquid() { return Wrap(snap_.lite_px, lite_bitmap_); }

private:
    Gdiplus::Bitmap* Wrap(const std::vector<std::uint8_t>& px,
                          std::unique_ptr<Gdiplus::Bitmap>& cache) {
        if (cache != nullptr) return cache.get();
        const int w = snap_.small_w;
        const int h = snap_.small_h;
        if (w <= 0 || h <= 0 || px.size() < static_cast<std::size_t>(w) * h * 4u) return nullptr;
        auto bitmap = std::make_unique<Gdiplus::Bitmap>(w, h, PixelFormat32bppARGB);
        Gdiplus::BitmapData data{};
        const Gdiplus::Rect rect(0, 0, w, h);
        if (bitmap->LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &data) !=
            Gdiplus::Ok) {
            return nullptr;
        }
        const std::size_t row_bytes = static_cast<std::size_t>(w) * 4u;
        for (int row = 0; row < h; ++row) {
            std::memcpy(static_cast<std::uint8_t*>(data.Scan0) +
                            static_cast<std::size_t>(row) * data.Stride,
                        px.data() + static_cast<std::size_t>(row) * row_bytes, row_bytes);
        }
        bitmap->UnlockBits(&data);
        cache = std::move(bitmap);
        return cache.get();
    }

    Backdrop::Snapshot snap_;
    std::unique_ptr<Gdiplus::Bitmap> small_bitmap_;
    std::unique_ptr<Gdiplus::Bitmap> lite_bitmap_;
};

// ---------------------------------------------------------------- 后台抓屏
class GlassAsync {
public:
    struct Request {
        int x = 0, y = 0, w = 0, h = 0;
        int occlude_x = 0, occlude_y = 0, occlude_w = 0, occlude_h = 0;
        bool epoch = false;  // true = 干净背景（不做修补）；false = 用 epoch 修补
    };

    GlassAsync() = default;
    ~GlassAsync() { Stop(); }
    GlassAsync(const GlassAsync&) = delete;
    GlassAsync& operator=(const GlassAsync&) = delete;

    void Start() {
        if (running_.exchange(true)) return;
        worker_ = std::thread([this] { Loop(); });
    }

    void Stop() {
        if (!running_.exchange(false)) return;
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    // 是否还有请求在飞。主线程用它避免重复提交同一帧的请求。
    bool Busy() {
        std::lock_guard<std::mutex> lock(mutex_);
        return has_request_ || working_;
    }

    void Submit(const Request& request) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            request_ = request;
            has_request_ = true;
        }
        cv_.notify_one();
    }

    // 非阻塞取出最新结果。返回 false 表示后台还没算完，主线程继续用旧纹理。
    bool TryTake(Backdrop::Snapshot* out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!has_result_) return false;
        *out = std::move(result_);
        has_result_ = false;
        return true;
    }

private:
    void Loop() {
        for (;;) {
            Request request;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return !running_.load() || has_request_; });
                if (!running_.load()) break;
                request = request_;
                has_request_ = false;
                working_ = true;
            }

            const auto started = std::chrono::steady_clock::now();
            Backdrop::Snapshot snap;
            // 这一步（GetDC + BitBlt + 降采样 + 3 趟模糊）就是原先卡住 UI 的部分。
            snap.ok = work_.Capture(request.x, request.y, request.w, request.h,
                                    request.occlude_x, request.occlude_y,
                                    request.occlude_w, request.occlude_h,
                                    request.epoch ? nullptr : &epoch_);
            if (snap.ok) {
                if (request.epoch) epoch_.AdoptClean(work_);
                work_.ExportSnapshot(&snap);
            }
            snap.cost_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                          started)
                    .count();

            {
                std::lock_guard<std::mutex> lock(mutex_);
                result_ = std::move(snap);  // 新结果覆盖旧结果，不排队
                has_result_ = true;
                working_ = false;
            }
        }
    }

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    bool has_request_ = false;
    bool has_result_ = false;
    bool working_ = false;
    Request request_;
    Backdrop::Snapshot result_;
    Backdrop work_;   // 后台私有：当前捕获
    Backdrop epoch_;  // 后台私有：干净背景（修补源）
};

}  // namespace win32
}  // namespace bi

#endif  // BI_SRC_WIN32_GLASS_ASYNC_HPP
