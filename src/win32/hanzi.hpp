// 繁 → 简。纯 Win32（LCMapStringEx），不引第三方词表。
//
// 为什么需要：歌词来自 LRCLIB 的用户上传，周杰伦这类早期作品几乎全是繁体
// （「故事的小黃花」）；而歌词库的匹配又是按繁体标题最准，所以不能拿简体
// 去查 —— 只能「查询用原文，显示用简体」。
//
// LCMapStringEx 的 LCMAP_SIMPLIFIED_CHINESE 依赖系统装有中文 locale：
// 调用失败时原样返回（宁可显示繁体，也不能把歌词变成乱码）。
#ifndef BI_SRC_WIN32_HANZI_HPP
#define BI_SRC_WIN32_HANZI_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <string>

namespace bi {
namespace win32 {

inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string s(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], need, nullptr,
                          nullptr);
    return s;
}

inline std::wstring Utf8ToWideHanzi(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int need =
        ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring w(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], need);
    return w;
}

// 只有含 CJK 统一表意文字时才做转换：英文歌、日文假名不必走一趟 API。
inline bool HasCjk(const std::wstring& w) {
    for (const wchar_t c : w) {
        if ((c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) ||
            (c >= 0xF900 && c <= 0xFAFF)) {
            return true;
        }
    }
    return false;
}

// 繁体转简体。locale 用 zh-CN：本机是中文系统，装的是 zh-CN 语言包；
// 若系统没有中文语言，API 会失败，此时原样返回（绝不返回空串）。
inline std::string ToSimplified(const std::string& utf8) {
    if (utf8.empty()) return utf8;
    const std::wstring w = Utf8ToWideHanzi(utf8);
    if (w.empty() || !HasCjk(w)) return utf8;

    // 先问需要多长，再一次性转换：歌词整段可能上千字。
    int need = ::LCMapStringEx(L"zh-CN", LCMAP_SIMPLIFIED_CHINESE, w.c_str(),
                               static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr, 0);
    if (need <= 0) {
        need = ::LCMapStringEx(LOCALE_NAME_SYSTEM_DEFAULT, LCMAP_SIMPLIFIED_CHINESE, w.c_str(),
                               static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr, 0);
    }
    if (need <= 0) return utf8;
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    const int written = ::LCMapStringEx(L"zh-CN", LCMAP_SIMPLIFIED_CHINESE, w.c_str(),
                                        static_cast<int>(w.size()), &out[0], need, nullptr,
                                        nullptr, 0);
    if (written <= 0) {
        const int fallback = ::LCMapStringEx(LOCALE_NAME_SYSTEM_DEFAULT,
                                             LCMAP_SIMPLIFIED_CHINESE, w.c_str(),
                                             static_cast<int>(w.size()), &out[0], need, nullptr,
                                             nullptr, 0);
        if (fallback <= 0) return utf8;
        out.resize(static_cast<std::size_t>(fallback));
    } else {
        out.resize(static_cast<std::size_t>(written));
    }
    const std::string s = WideToUtf8(out);
    return s.empty() ? utf8 : s;
}

}  // namespace win32
}  // namespace bi

#endif  // BI_SRC_WIN32_HANZI_HPP
