// 发一条真实系统 Toast，用来验证「通知实时到达」这条链路。
//
// 为什么需要这个探针：pwsh 7 不接受 `[Type, Assembly, ContentType=WindowsRuntime]`
// 这种旧的 WinRT 类型加载语法，PowerShell 发不出 toast；而「通知实时到达」
// 是产品里唯一还没被实测过的事件源。用 C++ 直接调 WinRT 最省事，
// 也和产品用的是同一条 API 路径（UserNotificationListener 读、ToastNotifier 写）。
//
// 用法：
//   toast_probe.exe                 // 发一条默认文案
//   toast_probe.exe "标题" "正文"   // 自定义

#include <windows.h>

#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/base.h>

#include <cstdio>
#include <string>

int wmain(int argc, wchar_t** argv) {
    const wchar_t* title = L"灵动岛实时通知测试";
    const wchar_t* body = L"这条是刚刚发出的真实系统通知";
    bool clear_first = false;
    int base = 1;
    if (argc > 1 && std::wstring(argv[1]) == L"--clear") {
        clear_first = true;
        base = 2;
    }
    if (argc > base + 1) {
        title = argv[base];
        body = argv[base + 1];
    }

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        using namespace winrt::Windows::UI::Notifications;

        // 清空通知中心：用于验证「通知中心为空时，下一条通知会不会被当成基线吞掉」。
        if (clear_first) {
            ToastNotificationManager::History().Clear();
            std::printf("history cleared\n");
        }

        auto xml = ToastNotificationManager::GetTemplateContent(ToastTemplateType::ToastText02);
        auto texts = xml.GetElementsByTagName(L"text");
        texts.Item(0).AppendChild(xml.CreateTextNode(title));
        texts.Item(1).AppendChild(xml.CreateTextNode(body));

        // 借 PowerShell 已注册的 AUMID：目标不是「像 PowerShell 的通知」，
        // 而是「一条能被 UserNotificationListener 读到的真实 toast」。
        const auto app_id = L"{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\WindowsPowerShell\\v1.0\\powershell.exe";
        ToastNotificationManager::CreateToastNotifier(app_id).Show(ToastNotification(xml));

        std::printf("toast sent: %ls / %ls\n", title, body);
        return 0;
    } catch (const winrt::hresult_error& e) {
        std::printf("toast failed: 0x%08X %ls\n", static_cast<unsigned>(e.code()), e.message().c_str());
        return 1;
    } catch (...) {
        std::printf("toast failed: unknown\n");
        return 2;
    }
}
