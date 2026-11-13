// 探针：验证「日常消息」这条路能不能走通。
//
// 关键问题：UserNotificationListener 在**未打包**（无 MSIX 身份）的桌面应用里
// 是否可用？这是事实问题，不能靠猜。GetAccessStatus() 是同步调用、不弹窗，
// 适合在开工前做低成本判定。
//
// 三种结果对应三条不同的架构路线：
//   Allowed      → 直接读系统通知，最理想
//   Unspecified  → 需 RequestAccessAsync 申请（会弹窗），有戏
//   Denied       → 未打包应用被拒，必须改走 MSIX 打包或 UI Automation

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <appmodel.h>  // GetCurrentPackageFullName：判断是否具备 MSIX 包身份

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Notifications.Management.h>

#include <cstdio>

using namespace winrt;

int wmain() {
    SetConsoleOutputCP(CP_UTF8);

    try {
        init_apartment(apartment_type::multi_threaded);
    } catch (const hresult_error& e) {
        std::printf("init_apartment FAILED: 0x%08X\n", static_cast<unsigned>(e.code()));
        return 1;
    }

    // 1) 当前身份：能否拿到 package identity，决定后面一切
    try {
        // 未打包进程调用 GetCurrentPackageFullName 会返回 APPMODEL_ERROR_NO_PACKAGE。
        wchar_t name[512]{};
        UINT32 len = 512;
        LONG rc = ::GetCurrentPackageFullName(&len, name);
        if (rc == APPMODEL_ERROR_NO_PACKAGE) {
            std::printf("[identity] NO PACKAGE (unpackaged / win32 裸进程)\n");
        } else if (rc == ERROR_SUCCESS) {
            std::wprintf(L"[identity] packaged: %s\n", name);
        } else {
            std::printf("[identity] unknown rc=%ld\n", rc);
        }
    } catch (...) {
        std::printf("[identity] probe threw\n");
    }

    // 2) 通知监听器权限状态（同步，不弹窗）
    try {
        auto listener = Windows::UI::Notifications::Management::UserNotificationListener::Current();
        if (!listener) {
            std::printf("[notif] Current() returned null\n");
            return 2;
        }
        const auto status = listener.GetAccessStatus();
        const char* text = "Unknown";
        switch (status) {
            case Windows::UI::Notifications::Management::UserNotificationListenerAccessStatus::Allowed:
                text = "Allowed";
                break;
            case Windows::UI::Notifications::Management::UserNotificationListenerAccessStatus::Denied:
                text = "Denied";
                break;
            case Windows::UI::Notifications::Management::UserNotificationListenerAccessStatus::Unspecified:
                text = "Unspecified";
                break;
            default:
                break;
        }
        std::printf("[notif] access status = %s\n", text);

        if (status == Windows::UI::Notifications::Management::UserNotificationListenerAccessStatus::Allowed) {
            // 3) 真的读一次，看能不能拿到现有通知
            try {
                auto notifications = listener.GetNotificationsAsync(
                                             Windows::UI::Notifications::NotificationKinds::Toast)
                                             .get();
                std::printf("[notif] existing toasts = %u\n",
                            static_cast<unsigned>(notifications.Size()));
            } catch (const hresult_error& e) {
                std::printf("[notif] GetNotificationsAsync FAILED: 0x%08X (%ls)\n",
                            static_cast<unsigned>(e.code()), e.message().c_str());
            }
        } else {
            std::printf("[notif] 不可读：需要 %s\n",
                        status == Windows::UI::Notifications::Management::
                                          UserNotificationListenerAccessStatus::Denied
                            ? "MSIX 打包身份（未打包被拒，改路线）"
                            : "调用 RequestAccessAsync 申请（会弹窗）");
        }
    } catch (const hresult_error& e) {
        std::printf("[notif] UserNotificationListener FAILED: 0x%08X (%ls)\n",
                    static_cast<unsigned>(e.code()), e.message().c_str());
    }

    // 4) 对照组：ToastNotificationManager 的历史记录（只能读本应用发的，用于确认 API 面可用）
    try {
        auto history = Windows::UI::Notifications::ToastNotificationManager::History();
        if (history) {
            std::printf("[toast] own history count = %u\n", history.GetHistory().Size());
        }
    } catch (const hresult_error& e) {
        std::printf("[toast] history FAILED: 0x%08X\n", static_cast<unsigned>(e.code()));
    }

    std::printf("[probe] done\n");
    return 0;
}
