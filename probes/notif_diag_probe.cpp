// 通知链路实时诊断探针：定位两个实机问题。
//   1) UserNotificationListener::NotificationChanged 订阅为什么失败
//      （产品日志出现 "event subscribe failed (poll-only)"）。
//   2) 通知的 AppInfo().DisplayInfo().GetLogo() 为什么拿不到图标
//      （产品日志 icon=none）。
//
// 用法：
//   notif_diag_probe.exe              // 跑 30 秒，期间用 toast_probe 发一条通知
//   notif_diag_probe.exe 60           // 自定义秒数
//
// 每一步都打印 HRESULT，哪一步断了一目了然。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/base.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

namespace {

std::string Narrow(const wchar_t* w) {
    if (w == nullptr) return {};
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<std::size_t>(need - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), need, nullptr, nullptr);
    return out;
}

void DumpNotification(const winrt::Windows::UI::Notifications::UserNotification& n) {
    std::uint32_t id = 0;
    try {
        id = n.Id();
    } catch (const winrt::hresult_error& e) {
        std::printf("  [?] Id() failed 0x%08X\n", static_cast<unsigned>(e.code()));
        return;
    }

    std::string aumid, name;
    try {
        aumid = Narrow(n.AppInfo().AppUserModelId().c_str());
    } catch (const winrt::hresult_error& e) {
        std::printf("  [%u] AppUserModelId failed 0x%08X\n", id, static_cast<unsigned>(e.code()));
    } catch (...) {
        std::printf("  [%u] AppUserModelId failed (non-hresult)\n", id);
    }
    try {
        name = Narrow(n.AppInfo().DisplayInfo().DisplayName().c_str());
    } catch (const winrt::hresult_error& e) {
        std::printf("  [%u] DisplayName failed 0x%08X\n", id, static_cast<unsigned>(e.code()));
    } catch (...) {
        std::printf("  [%u] DisplayName failed (non-hresult)\n", id);
    }
    std::printf("  [%u] app=%s aumid=%s\n", id, name.c_str(),
                aumid.empty() ? "(empty)" : aumid.c_str());

    // GetLogo：产品 icon=none 的直接怀疑对象。
    // 实验 1：不同尺寸——GetLogo 按 Size 取，某些实现只对特定尺寸返回非空。
    static const float kSizes[] = {16.0f, 24.0f, 32.0f, 44.0f, 48.0f, 96.0f, 256.0f};
    for (float s : kSizes) {
        try {
            auto logo = n.AppInfo().DisplayInfo().GetLogo(
                winrt::Windows::Foundation::Size{s, s});
            if (!logo) {
                std::printf("  [%u] GetLogo(%.0f) -> null reference\n", id, s);
            } else {
                try {
                    auto stream = logo.OpenReadAsync().get();
                    std::printf("  [%u] GetLogo(%.0f) -> stream %lld bytes\n", id, s,
                                static_cast<long long>(stream ? stream.Size() : -1));
                } catch (const winrt::hresult_error& e) {
                    std::printf("  [%u] GetLogo(%.0f) OpenRead failed 0x%08X\n", id, s,
                                static_cast<unsigned>(e.code()));
                }
            }
        } catch (const winrt::hresult_error& e) {
            std::printf("  [%u] GetLogo(%.0f) failed 0x%08X\n", id, s,
                        static_cast<unsigned>(e.code()));
        } catch (...) {
            std::printf("  [%u] GetLogo(%.0f) failed (non-hresult)\n", id, s);
        }
    }

    // 实验 2：AppInfo::GetFromAppUserModelId(AUMID) 静态方法 —— 换一条路拿 DisplayInfo。
    if (!aumid.empty()) {
        try {
            auto app_info = winrt::Windows::ApplicationModel::AppInfo::GetFromAppUserModelId(
                winrt::to_hstring(aumid));
            std::string name2;
            try {
                name2 = Narrow(app_info.DisplayInfo().DisplayName().c_str());
            } catch (...) {
            }
            std::printf("  [%u] GetFromAUMId: display=%s\n", id,
                        name2.empty() ? "(empty)" : name2.c_str());
            auto logo = app_info.DisplayInfo().GetLogo(
                winrt::Windows::Foundation::Size{96.0f, 96.0f});
            if (!logo) {
                std::printf("  [%u] GetFromAUMId logo -> null reference\n", id);
            } else {
                auto stream = logo.OpenReadAsync().get();
                std::printf("  [%u] GetFromAUMId logo -> stream %lld bytes\n", id,
                            static_cast<long long>(stream ? stream.Size() : -1));
            }
        } catch (const winrt::hresult_error& e) {
            std::printf("  [%u] GetFromAUMId failed 0x%08X %ls\n", id,
                        static_cast<unsigned>(e.code()), e.message().c_str());
        } catch (...) {
            std::printf("  [%u] GetFromAUMId failed (non-hresult)\n", id);
        }
    }

    // 实验 3：从 toast XML 里直接找 image（QQ/微信这类应用的 toast 通常自带 logo 图）。
    try {
        auto toast = n.Notification().as<winrt::Windows::UI::Notifications::IToastNotification>();
        const std::string xml = Narrow(toast.Content().GetXml().c_str());
        std::size_t pos = 0;
        int imgs = 0;
        while ((pos = xml.find("<image", pos)) != std::string::npos && imgs < 3) {
            const std::size_t end = xml.find('>', pos);
            if (end == std::string::npos) break;
            std::string tag = xml.substr(pos, end - pos);
            const std::size_t src = tag.find("src=");
            if (src != std::string::npos) {
                std::printf("  [%u] toast image[%d] %s\n", id, imgs,
                            tag.substr(src, tag.find(' ', src) - src).c_str());
            }
            ++imgs;
            pos = end;
        }
        if (imgs == 0) std::printf("  [%u] toast xml: no <image> elements\n", id);
    } catch (const winrt::hresult_error& e) {
        std::printf("  [%u] toast xml failed 0x%08X\n", id, static_cast<unsigned>(e.code()));
    } catch (...) {
        std::printf("  [%u] toast xml failed (non-hresult)\n", id);
    }

    try {
        auto binding =
            n.Notification().Visual().GetBinding(
                winrt::Windows::UI::Notifications::KnownNotificationBindings::ToastGeneric());
        if (binding) {
            auto texts = binding.GetTextElements();
            for (std::uint32_t i = 0; i < texts.Size() && i < 2; ++i) {
                std::printf("  [%u] text[%u]=%s\n", id, i,
                            Narrow(texts.GetAt(i).Text().c_str()).c_str());
            }
        }
    } catch (...) {
    }
}

}  // namespace

int main(int argc, char** argv) {
    int seconds = 30;
    bool bench = false;
    if (argc > 1) {
        if (std::string(argv[1]) == "--bench") {
            bench = true;
        } else {
            seconds = std::atoi(argv[1]);
        }
    }
    if (seconds <= 0) seconds = 30;

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
        std::printf("init_apartment failed (continuing)\n");
    }

    using namespace winrt::Windows::UI::Notifications;
    using Clock = std::chrono::steady_clock;

    Management::UserNotificationListener listener{nullptr};
    try {
        listener = Management::UserNotificationListener::Current();
        const auto status = listener.GetAccessStatus();
        std::printf("listener access: %d (1=Allowed 2=Denied 0=Unspecified)\n",
                    static_cast<int>(status));
    } catch (const winrt::hresult_error& e) {
        std::printf("listener init failed 0x%08X\n", static_cast<unsigned>(e.code()));
        return 1;
    }

    // ---- 基准：单次 GetNotificationsAsync 的真实成本 ----
    // 实测背景：2s 轮询时空闲 CPU 1.88% 单核，500ms 轮询时 10.94%，
    // 说明成本几乎全在这个调用上，必须量出单次耗时才能选间隔。
    if (bench) {
        double sum = 0.0, worst = 0.0, best = 1e9;
        std::uint32_t count = 0;
        constexpr int kRuns = 20;
        for (int i = 0; i < kRuns; ++i) {
            const auto t0 = Clock::now();
            auto items = listener.GetNotificationsAsync(NotificationKinds::Toast).get();
            std::uint32_t ids = 0;
            count = items.Size();
            for (std::uint32_t k = 0; k < count; ++k) ids += items.GetAt(k).Id() ? 1 : 0;
            const double ms =
                std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            sum += ms;
            worst = ms > worst ? ms : worst;
            best = ms < best ? ms : best;
            (void)ids;
        }
        std::printf("GetNotificationsAsync x%d, toasts=%u: avg=%.2fms min=%.2f max=%.2f\n", kRuns,
                    count, sum / kRuns, best, worst);
        return 0;
    }

    // ---- 1) 事件订阅诊断（产品里失败的正是这一步）----
    std::atomic<int> events{0};
    Management::UserNotificationListener::NotificationChanged_revoker revoker;
    bool subscribed = false;
    try {
        revoker = listener.NotificationChanged(
            winrt::auto_revoke,
            [&events](const Management::UserNotificationListener&,
                      const UserNotificationChangedEventArgs&) { events.fetch_add(1); });
        subscribed = true;
        std::printf("NotificationChanged: subscribed OK\n");
    } catch (const winrt::hresult_error& e) {
        std::printf("NotificationChanged: subscribe failed 0x%08X %ls\n",
                    static_cast<unsigned>(e.code()), e.message().c_str());
    } catch (...) {
        std::printf("NotificationChanged: subscribe failed (non-hresult)\n");
    }

    // ---- 2) 枚举 + Logo 诊断（只在新通知出现时逐条打印）----
    std::set<std::uint32_t> seen;
    int last_events = -1;
    for (int t = 0; t < seconds * 2; ++t) {
        try {
            auto items = listener.GetNotificationsAsync(NotificationKinds::Toast).get();
            const auto count = items.Size();
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto id = items.GetAt(i).Id();
                if (seen.insert(id).second) {
                    DumpNotification(items.GetAt(i));
                }
            }
            if (events.load() != last_events) {
                std::printf("t=%ds toast_count=%u events=%d\n", t / 2, count, events.load());
                last_events = events.load();
            }
        } catch (const winrt::hresult_error& e) {
            std::printf("poll failed 0x%08X\n", static_cast<unsigned>(e.code()));
        }
        ::Sleep(500);
    }

    std::printf("done: subscribed=%d events=%d\n", subscribed ? 1 : 0, events.load());
    return 0;
}
