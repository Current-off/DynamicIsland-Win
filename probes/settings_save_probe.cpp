// 探针：验证「设置窗口 → 配置文件」这条链路是真的通的。
//
// 为什么需要它：CollectFromControls 只有在真窗口里跑一次才算被验证过。
// 截图只能证明控件画对了，证明不了「填进去的值能落盘」。
// 做法：抓已经打开的设置窗口（按类名），直接写控件文本/勾选状态，
// 再向窗口发 WM_COMMAND(保存)，最后把 rules.json 读出来比对。
//
// 用法：
//   1) 先启动 build\cpp\balance_island.exe --settings
//   2) 运行本探针，它会打印保存后的关键字段
#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <string>

namespace {

constexpr int kIdAppEdit = 2007;
constexpr int kIdVolume = 2004;
constexpr int kIdQuietOn = 2011;
constexpr int kIdQuietFrom = 2012;
constexpr int kIdQuietTo = 2013;
constexpr int kIdSave = 2014;

std::string ReadFileUtf8(const std::wstring& path) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    ::GetFileSizeEx(file, &size);
    std::string out(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    ::ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    ::CloseHandle(file);
    out.resize(read);
    return out;
}

std::wstring ConfigPath() {
    wchar_t* appdata = nullptr;
    std::wstring dir;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata)) && appdata) {
        dir = std::wstring(appdata) + L"\\balance_island\\rules.json";
        ::CoTaskMemFree(appdata);
    }
    return dir;
}

}  // namespace

int wmain() {
    HWND hwnd = nullptr;
    for (int i = 0; i < 50 && hwnd == nullptr; ++i) {
        hwnd = ::FindWindowW(L"BI_SettingsWindow", nullptr);
        if (hwnd == nullptr) ::Sleep(100);
    }
    if (hwnd == nullptr) {
        std::printf("PROBE FAIL: 没找到设置窗口，请先运行 balance_island.exe --settings\n");
        return 1;
    }
    std::printf("PROBE: 找到设置窗口 hwnd=%p\n", static_cast<void*>(hwnd));

    HWND app_edit = ::GetDlgItem(hwnd, kIdAppEdit);
    HWND volume = ::GetDlgItem(hwnd, kIdVolume);
    HWND quiet_on = ::GetDlgItem(hwnd, kIdQuietOn);
    HWND quiet_from = ::GetDlgItem(hwnd, kIdQuietFrom);
    HWND quiet_to = ::GetDlgItem(hwnd, kIdQuietTo);
    if (app_edit == nullptr || volume == nullptr || quiet_on == nullptr) {
        std::printf("PROBE FAIL: 控件 id 对不上（UI 改过 id 需要同步本探针）\n");
        return 1;
    }

    std::printf("PROBE: 写入前 edit 长度=%ld\n",
                static_cast<long>(::SendMessageW(app_edit, WM_GETTEXTLENGTH, 0, 0)));
    ::SetWindowTextW(app_edit, L"wechat\r\n迅雷\r\ncom.tencent");
    std::printf("PROBE: 写入后 edit 长度=%ld\n",
                static_cast<long>(::SendMessageW(app_edit, WM_GETTEXTLENGTH, 0, 0)));
    std::printf("PROBE: quiet_from 控件类=%p\n", static_cast<void*>(quiet_from));
    ::SendMessageW(volume, BM_SETCHECK, BST_UNCHECKED, 0);          // 关掉音量提示
    ::SendMessageW(quiet_on, BM_SETCHECK, BST_CHECKED, 0);           // 开启免打扰
    ::SetWindowTextW(quiet_from, L"22:30");
    ::SetWindowTextW(quiet_to, L"07:15");

    // 真的点一次「保存」：走 WM_COMMAND，与用户点击同一条路径。
    ::SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(kIdSave, BN_CLICKED), 0);
    ::Sleep(600);

    const std::string text = ReadFileUtf8(ConfigPath());
    if (text.empty()) {
        std::printf("PROBE FAIL: 没读到 rules.json\n");
        return 1;
    }

    // 不引 JSON 库：只做关键字串检查，够判断「值有没有落盘」。
    const bool has_wechat = text.find("wechat") != std::string::npos;
    const bool has_xunlei = text.find("迅雷") != std::string::npos;
    const bool volume_off = text.find("\"volume\": false") != std::string::npos;
    const bool quiet_saved = text.find("\"enabled\": true") != std::string::npos;
    const bool from_ok = text.find("\"22:30\"") != std::string::npos;
    const bool to_ok = text.find("\"07:15\"") != std::string::npos;
    const bool window_closed = ::FindWindowW(L"BI_SettingsWindow", nullptr) == nullptr;

    std::printf("PROBE: app.block 含 wechat=%d 迅雷=%d, volume=false=%d, quiet=%d(%s-%s), 窗口已关闭=%d\n",
                has_wechat ? 1 : 0, has_xunlei ? 1 : 0, volume_off ? 1 : 0, quiet_saved ? 1 : 0,
                from_ok ? "22:30" : "?", to_ok ? "07:15" : "?", window_closed ? 1 : 0);

    const bool pass = has_wechat && has_xunlei && volume_off && quiet_saved && from_ok && to_ok;
    std::printf("PROBE: %s\n", pass ? "PASS (设置窗口 → 配置文件链路通)" : "FAIL");
    return pass ? 0 : 1;
}
