// 应用图标链路诊断探针：验证 AUMID → 开始菜单 .lnk → exe 图标 这条 Win32 路线。
//
// 背景：通知的应用图标 WinRT 路线全堵死（GetLogo 恒 null、GetFromAUMId 0x80070490、
// binding 无图片），改走 Win32。产品里 icon=none，需要定位断在哪一步：
//   ① 开始菜单 .lnk 枚举数量
//   ② .lnk 里带 System.AppUserModel.ID 属性的比例
//   ③ 指定 AUMID 是否命中
//   ④ 命中后 exe 路径 / 图标提取 / PNG 落盘是否成功
//
// 用法：appicon_probe.exe [AUMID]   // 默认用 PowerShell 的 AUMID

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <knownfolders.h>
#include <propkey.h>
#include <propsys.h>
#include <shellapi.h>
#include <shlobj.h>

#include <gdiplus.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../../src/win32/live_sources.hpp"

using bi::live::CollectShortcutFiles;
using bi::live::SaveIconPng;
using bi::live::WidenOf;

// 诊断：枚举 shell:AppsFolder，打印 AUMID 含 filter 的项。
// 用来确认「UWP 应用到底在不在这个虚拟文件夹里、AppID 长什么样」。
static void DumpAppsFolder(const wchar_t* filter) {
    winrt::com_ptr<IShellItem> apps;
    HRESULT hr = ::SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr,
                                               IID_PPV_ARGS(apps.put()));
    std::printf("SHCreateItemFromParsingName: 0x%08X\n", static_cast<unsigned>(hr));
    if (FAILED(hr)) return;
    winrt::com_ptr<IShellFolder> folder;
    hr = apps->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(folder.put()));
    std::printf("BindToHandler(BHID_SFObject): 0x%08X\n", static_cast<unsigned>(hr));
    if (FAILED(hr)) return;
    winrt::com_ptr<IEnumIDList> ids;
    hr = folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, ids.put());
    std::printf("EnumObjects: 0x%08X\n", static_cast<unsigned>(hr));
    if (FAILED(hr)) return;
    LPITEMIDLIST pidl = nullptr;
    ULONG fetched = 0;
    int total = 0;
    int with_aumid = 0;
    int shown = 0;
    WCHAR name_buf[512] = {};
    while (ids->Next(1, &pidl, &fetched) == S_OK && fetched == 1) {
        ++total;
        STRRET strret{};
        std::wstring a;
        if (SUCCEEDED(folder->GetDisplayNameOf(pidl, SHGDN_FORPARSING, &strret)) &&
            SUCCEEDED(::StrRetToBufW(&strret, pidl, name_buf,
                                     static_cast<UINT>(std::size(name_buf))))) {
            a = name_buf;
            const auto slash = a.find_last_of(L'\\');
            if (slash != std::wstring::npos) a = a.substr(slash + 1);
        }
        ::CoTaskMemFree(pidl);
        if (a.empty()) continue;
        ++with_aumid;
        if (filter[0] == 0 || a.find(filter) != std::wstring::npos) {
            if (shown < 30) std::printf("  [%d] %ls\n", total, a.c_str());
            ++shown;
        }
    }
    std::printf("apps total=%d with_aumid=%d shown=%d\n", total, with_aumid, shown);
}

int main(int argc, char** argv) {
    const std::string aumid =
        argc > 1 ? std::string(argv[1])
                 : std::string(
                       "{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\WindowsPowerShell\\v1.0\\"
                       "powershell.exe");
    std::printf("target aumid: %s\n", aumid.c_str());

    Gdiplus::GdiplusStartupInput gdi_init;
    ULONG_PTR gdi_token = 0;
    if (Gdiplus::GdiplusStartup(&gdi_token, &gdi_init, nullptr) != Gdiplus::Ok) {
        std::printf("gdiplus startup failed\n");
        return 1;
    }

    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool co_ok = SUCCEEDED(hr);
    std::printf("coinit: %s (0x%08X)\n", co_ok ? "ok" : "failed", static_cast<unsigned>(hr));

    if (argc > 2 && std::string(argv[2]) == "--dump-apps") {
        DumpAppsFolder(L"");
        return 0;
    }

    std::vector<std::wstring> lnks;
    for (bool common : {true, false}) {
        PWSTR root = nullptr;
        if (SUCCEEDED(::SHGetKnownFolderPath(
                common ? FOLDERID_CommonPrograms : FOLDERID_Programs, 0, nullptr, &root)) &&
            root != nullptr) {
            const std::size_t before = lnks.size();
            CollectShortcutFiles(root, &lnks);
            std::printf("start menu (%s): %zu lnks\n", common ? "common" : "user",
                        lnks.size() - before);
            ::CoTaskMemFree(root);
        } else {
            std::printf("start menu (%s): SHGetKnownFolderPath failed\n",
                        common ? "common" : "user");
        }
    }
    std::printf("total lnks: %zu\n", lnks.size());

    int with_aumid = 0;
    int matched = 0;
    for (const auto& lnk : lnks) {
        IShellLinkW* link_raw = nullptr;
        if (FAILED(::CoCreateInstance(__uuidof(ShellLink), nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&link_raw))) ||
            link_raw == nullptr) {
            continue;
        }
        IPersistFile* file = nullptr;
        if (FAILED(link_raw->QueryInterface(IID_PPV_ARGS(&file))) ||
            FAILED(file->Load(lnk.c_str(), STGM_READ))) {
            if (file) file->Release();
            link_raw->Release();
            continue;
        }
        IPropertyStore* store = nullptr;
        if (SUCCEEDED(link_raw->QueryInterface(IID_PPV_ARGS(&store)))) {
            PROPVARIANT var{};
            if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &var))) {
                if (var.vt == VT_LPWSTR && var.pwszVal != nullptr && var.pwszVal[0] != L'\0') {
                    ++with_aumid;
                    if (_wcsicmp(var.pwszVal, WidenOf(aumid).c_str()) == 0) {
                        ++matched;
                        std::printf("MATCH: %ls\n", lnk.c_str());
                        wchar_t target[MAX_PATH] = {0};
                        if (SUCCEEDED(link_raw->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH))) {
                            std::printf("  target: %ls\n", target);
                        } else {
                            std::printf("  GetPath failed\n");
                        }
                        SHFILEINFOW sfi{};
                        if (::SHGetFileInfoW(target, 0, &sfi, sizeof(sfi),
                                             SHGFI_SYSICONINDEX) != 0) {
                            std::printf("  sysicon index: %d\n", sfi.iIcon);
                            IImageList* images = nullptr;
                            if (SUCCEEDED(::SHGetImageList(SHIL_EXTRALARGE,
                                                           IID_PPV_ARGS(&images)))) {
                                HICON icon = nullptr;
                                if (SUCCEEDED(images->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &icon))) {
                                    const std::wstring out =
                                        L"appicon_probe_out.png";
                                    const bool ok = SaveIconPng(icon, out);
                                    ::DestroyIcon(icon);
                                    std::printf("  SaveIconPng: %s -> %ls\n",
                                                ok ? "ok" : "FAILED", out.c_str());
                                } else {
                                    std::printf("  GetIcon failed\n");
                                }
                                images->Release();
                            } else {
                                std::printf("  SHGetImageList failed\n");
                            }
                        } else {
                            std::printf("  SHGetFileInfo failed\n");
                        }
                    }
                }
                ::PropVariantClear(&var);
            }
            store->Release();
        }
        file->Release();
        link_raw->Release();
    }

    std::printf("lnks with AUMID property: %d / %zu, matched: %d\n", with_aumid, lnks.size(),
                matched);

    // 最后用产品函数本身跑一遍（隐式 AUMID 解码 + .lnk 扫描 + 缓存），
    // 确认产品路径与探针路径行为一致。
    const std::string icon = bi::live::AppIconForAumid(aumid);
    std::printf("AppIconForAumid: %s\n", icon.empty() ? "(empty)" : icon.c_str());

    // 打包应用形态对照（通知中心给 PFN，.lnk 存 PFN!App）
    const std::string wb = "WorkBuddy.WorkBuddy";
    const std::string icon2 = bi::live::AppIconForAumid(wb);
    std::printf("AppIconForAumid(%s): %s\n", wb.c_str(), icon2.empty() ? "(empty)" : icon2.c_str());

    if (co_ok) ::CoUninitialize();
    Gdiplus::GdiplusShutdown(gdi_token);
    return 0;
}
