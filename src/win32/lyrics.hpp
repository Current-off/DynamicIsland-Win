// DynamicIsland-Win / 在线歌词
//
// 背景：Windows 的 SMTC（系统媒体传输控制）只暴露歌名 / 歌手 / 专辑 / 封面 /
// 播放进度，**不提供歌词**。想要歌词必须自己找数据源。
//
// 这里用 LRCLIB（https://lrclib.net，开源歌词库）的公开 API：
//   GET https://lrclib.net/api/search?track_name=<title>&artist_name=<artist>
// 免密钥、返回带 [mm:ss.xx] 时间轴的 syncedLyrics，适合逐句同步。
//
// 工程约束：
//   1. HTTP 请求绝不能放在渲染主线程 —— 用独立 worker 线程 + 条件变量，
//      主线程只做「换歌时投一个请求」和「每帧读一次结果」。
//   2. 切歌要丢弃在途结果（active_key_ 比对），否则会闪出上一首的歌词。
//   3. 无网络 / 超时 / 无结果都要有明确状态，界面显示占位而不是空白。
//   4. 纯函数（ParseLrc / SelectLyricLines / UrlEncode / ExtractJsonString）
//      与 I/O 分离，前者可直接单测。
#ifndef DIW_SRC_WIN32_LYRICS_HPP
#define DIW_SRC_WIN32_LYRICS_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include "hanzi.hpp"
#include <winhttp.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace diw {
namespace win32 {

struct LyricLine {
    std::int64_t ms = 0;
    std::string text;
};

struct LyricTriple {
    std::string prev;
    std::string cur;
    std::string next;
    bool available = false;  // 有歌词数据（哪怕当前在前奏）
};

inline void AppendUtf8(std::string* out, unsigned cp) {
    if (cp <= 0x7F) {
        *out += static_cast<char>(cp);
    } else if (cp <= 0x7FF) {
        *out += static_cast<char>(0xC0 | (cp >> 6));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        *out += static_cast<char>(0xE0 | (cp >> 12));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        *out += static_cast<char>(0xF0 | (cp >> 18));
        *out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

inline void TrimAscii(std::string* s) {
    const auto not_space = [](unsigned char c) { return c > ' '; };
    const auto b = std::find_if(s->begin(), s->end(), not_space);
    const auto e = std::find_if(s->rbegin(), s->rend(), not_space).base();
    if (b >= e) {
        s->clear();
    } else {
        *s = std::string(b, e);
    }
}

// 解析 LRC。支持一行多个时间戳（同一句反复出现）、[mm:ss] 与 [mm:ss.xx]，
// 以及 [ar:] / [ti:] / [offset:] 之类元信息行（解析不出时间戳即跳过）。
inline std::vector<LyricLine> ParseLrc(const std::string& lrc) {
    std::vector<LyricLine> out;
    const std::size_t n = lrc.size();
    std::size_t i = 0;
    std::int64_t offset_ms = 0;
    while (i < n) {
        while (i < n && (lrc[i] == '\r' || lrc[i] == '\n')) ++i;
        if (i >= n) break;

        std::vector<std::int64_t> stamps;
        while (i < n && lrc[i] == '[') {
            const auto close = lrc.find(']', i);
            if (close == std::string::npos) break;
            const std::string tag = lrc.substr(i + 1, close - i - 1);
            i = close + 1;

            // [offset:+/-毫秒]
            if (tag.rfind("offset:", 0) == 0) {
                try {
                    offset_ms = std::stoll(tag.substr(7));
                } catch (...) {
                }
                continue;
            }
            const auto colon = tag.find(':');
            if (colon == std::string::npos) continue;  // [ar:xxx] 等元信息
            try {
                const double minutes = std::stod(tag.substr(0, colon));
                const double seconds = std::stod(tag.substr(colon + 1));
                if (minutes < 0 || seconds < 0) continue;
                stamps.push_back(static_cast<std::int64_t>((minutes * 60.0 + seconds) * 1000.0));
            } catch (...) {
            }
        }

        const auto eol = lrc.find_first_of("\r\n", i);
        const std::size_t text_end = (eol == std::string::npos) ? n : eol;
        std::string text = lrc.substr(i, text_end - i);
        i = text_end;
        TrimAscii(&text);
        if (!text.empty()) {
            for (std::int64_t ms : stamps) {
                out.push_back({ms - offset_ms, text});
            }
        }
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const LyricLine& a, const LyricLine& b) { return a.ms < b.ms; });
    return out;
}

// 按播放进度取「上一句 / 当前句 / 下一句」。前奏阶段 cur 为空、next 为首句。
inline LyricTriple SelectLyricLines(const std::vector<LyricLine>& lines, std::int64_t pos_ms) {
    LyricTriple t;
    if (lines.empty()) return t;
    t.available = true;
    std::size_t idx = 0;
    bool found = false;
    for (std::size_t k = 0; k < lines.size(); ++k) {
        if (lines[k].ms <= pos_ms) {
            idx = k;
            found = true;
        } else {
            break;
        }
    }
    if (!found) {
        t.next = lines.front().text;
        return t;
    }
    t.cur = lines[idx].text;
    if (idx > 0) t.prev = lines[idx - 1].text;
    if (idx + 1 < lines.size()) t.next = lines[idx + 1].text;
    return t;
}

inline std::string UrlEncodeUtf8(const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

// 从 JSON 里取某个字符串字段（含 \n / \" / \\ / \uXXXX 与代理对解码）。
// LRCLIB 的响应是数组，这里从整段文本里找「第一个非空」的该字段。
inline std::string ExtractFirstNonEmptyJsonString(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    std::size_t p = 0;
    while ((p = json.find(needle, p)) != std::string::npos) {
        auto colon = json.find(':', p + needle.size());
        if (colon == std::string::npos) return std::string();
        ++colon;
        while (colon < json.size() && (json[colon] == ' ' || json[colon] == '\t' ||
                                       json[colon] == '\r' || json[colon] == '\n')) {
            ++colon;
        }
        p = colon + 1;
        if (colon >= json.size() || json[colon] != '"') continue;
        std::string out;
        for (std::size_t q = colon + 1; q < json.size(); ++q) {
            const char c = json[q];
            if (c == '"') break;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (q + 1 >= json.size()) break;
            const char e = json[++q];
            switch (e) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'u': {
                    if (q + 4 >= json.size()) break;
                    unsigned cp = 0;
                    bool ok = true;
                    for (int k = 0; k < 4; ++k) {
                        const char h = json[++q];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        else { ok = false; break; }
                    }
                    if (!ok) break;
                    // 高代理 + 紧跟的低代理 → 合成一个码点
                    if (cp >= 0xD800 && cp <= 0xDBFF && q + 6 < json.size() &&
                        json[q + 1] == '\\' && json[q + 2] == 'u') {
                        unsigned low = 0;
                        bool low_ok = true;
                        std::size_t r = q + 2;
                        for (int k = 0; k < 4; ++k) {
                            const char h = json[++r];
                            low <<= 4;
                            if (h >= '0' && h <= '9') low |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') low |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') low |= static_cast<unsigned>(h - 'A' + 10);
                            else { low_ok = false; break; }
                        }
                        if (low_ok && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            q = r;
                        }
                    }
                    AppendUtf8(&out, cp);
                    break;
                }
                default: out += e; break;
            }
        }
        if (!out.empty()) return out;
    }
    return std::string();
}

// WinHTTP 同步 GET（只在 worker 线程里调用）。超时 3s 连接 / 5s 总读取。
// 实测踩坑：本机 HTTP_PROXY=http://127.0.0.1:3876，直连 LRCLIB 8s 超时，
// 走该代理 200 成功。但 WinHTTP 的 AUTOMATIC_PROXY / DEFAULT_PROXY 都拿不到
// 环境变量里的代理，WinHttpSendRequest 直接 12002（超时）—— 而 .NET / 浏览器
// 走的是环境变量。所以这里显式读，优先按命名代理试一次，失败再回落直连。
inline std::wstring EnvProxyForHttps() {
    const wchar_t* kNames[] = {L"HTTPS_PROXY", L"https_proxy", L"ALL_PROXY", L"all_proxy",
                               L"HTTP_PROXY", L"http_proxy"};
    for (const wchar_t* name : kNames) {
        const DWORD need = ::GetEnvironmentVariableW(name, nullptr, 0);
        if (need == 0) continue;
        std::wstring v(static_cast<std::size_t>(need), L'\0');
        const DWORD got = ::GetEnvironmentVariableW(name, &v[0], need);
        if (got == 0) continue;
        v.resize(got);
        while (!v.empty() && v.back() == L'/') v.pop_back();
        if (!v.empty()) return v;
    }
    return std::wstring();
}

// 「Internet 选项」里配置的代理（WinINet 视角）。环境变量没有时的第二路。
// 注意：用户把 ProxyEnable 关掉时这里拿不到值，所以它只是兜底之一。
inline std::wstring SystemProxyForHttps() {
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG cfg{};
    std::wstring out;
    if (::WinHttpGetIEProxyConfigForCurrentUser(&cfg)) {
        if (cfg.lpszProxy != nullptr) {
            const std::wstring raw(cfg.lpszProxy);
            // 可能是 "host:port"，也可能是 "http=h:p;https=h:p" 的分协议写法。
            auto pick = [&raw](const wchar_t* key) -> std::wstring {
                const std::wstring k(key);
                const auto p = raw.find(k);
                if (p == std::wstring::npos) return std::wstring();
                const auto end = raw.find(L';', p);
                return raw.substr(p + k.size(),
                                  end == std::wstring::npos ? std::wstring::npos : end - p - k.size());
            };
            out = pick(L"https=");
            if (out.empty()) out = pick(L"http=");
            if (out.empty()) out = raw;
        }
        if (cfg.lpszProxy) ::GlobalFree(cfg.lpszProxy);
        if (cfg.lpszProxyBypass) ::GlobalFree(cfg.lpszProxyBypass);
        if (cfg.lpszAutoConfigUrl) ::GlobalFree(cfg.lpszAutoConfigUrl);
    }
    return out;
}

inline bool HttpGetOnce(const std::wstring& host, const std::wstring& path,
                        const std::wstring& proxy, std::string* out, std::string* error) {
    if (out == nullptr) return false;
    out->clear();
    if (error) error->clear();
    auto fail = [&](const char* stage, DWORD code) {
        if (error) {
            *error = std::string(stage) + " err=" + std::to_string(static_cast<unsigned long>(code));
        }
    };
    HINTERNET session = proxy.empty()
        ? ::WinHttpOpen(L"DynamicIsland-Win/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)
        : ::WinHttpOpen(L"DynamicIsland-Win/1.0", WINHTTP_ACCESS_TYPE_NAMED_PROXY,
                        proxy.c_str(), WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        fail("WinHttpOpen", ::GetLastError());
        return false;
    }
    HINTERNET conn = ::WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (conn == nullptr) {
        fail("WinHttpConnect", ::GetLastError());
        ::WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET req = ::WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (req == nullptr) {
        fail("WinHttpOpenRequest", ::GetLastError());
        ::WinHttpCloseHandle(conn);
        ::WinHttpCloseHandle(session);
        return false;
    }
    ::WinHttpSetTimeouts(req, 5000, 5000, 5000, 10000);
    ::WinHttpAddRequestHeaders(req, L"Accept: application/json\r\n", static_cast<DWORD>(-1),
                               WINHTTP_ADDREQ_FLAG_ADD);
    bool ok = ::WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                                   0, 0) != FALSE;
    if (!ok) {
        fail("WinHttpSendRequest", ::GetLastError());
    } else {
        ok = ::WinHttpReceiveResponse(req, nullptr) != FALSE;
        if (!ok) fail("WinHttpReceiveResponse", ::GetLastError());
    }
    if (ok) {
        DWORD status = 0;
        DWORD status_size = sizeof(status);
        if (::WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                                  WINHTTP_NO_HEADER_INDEX)) {
            if (status != 200) {
                fail("HTTP status", status);
                ok = false;
            }
        }
    }
    if (ok) {
        for (;;) {
            DWORD avail = 0;
            if (!::WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            std::vector<char> buf(avail);
            DWORD read = 0;
            if (!::WinHttpReadData(req, buf.data(), avail, &read) || read == 0) break;
            out->append(buf.data(), read);
            if (out->size() > 2 * 1024 * 1024) break;  // 别让异常响应撑爆内存
        }
    }
    ::WinHttpCloseHandle(req);
    ::WinHttpCloseHandle(conn);
    ::WinHttpCloseHandle(session);
    if (ok && out->empty()) fail("empty body", 0);
    return ok && !out->empty();
}

// 对外入口。代理优先级：调用方显式指定 > 环境变量 > Internet 选项 > 直连。
// 这台机器实测：IPv6 的 443 不通、IPv4 通，直连会优先 IPv6 然后 12002 超时，
// 所以「读到代理」几乎是能出网的唯一路径，四路都要试。
inline bool HttpGetUtf8(const std::wstring& host, const std::wstring& path, std::string* out,
                        std::string* error = nullptr,
                        const std::wstring& preferred_proxy = std::wstring()) {
    if (out == nullptr) return false;
    if (error) error->clear();
    std::wstring proxy = preferred_proxy;
    if (proxy.empty()) proxy = EnvProxyForHttps();
    if (proxy.empty()) proxy = SystemProxyForHttps();
    if (!proxy.empty()) {
        std::string proxy_err;
        if (HttpGetOnce(host, path, proxy, out, &proxy_err)) {
            if (error) error->clear();
            return true;
        }
        if (error) *error = "proxy " + proxy_err + "; ";
    }
    std::string direct_err;
    const bool ok = HttpGetOnce(host, path, std::wstring(), out, &direct_err);
    if (!ok && error) *error += "direct " + direct_err;
    if (ok && error) error->clear();
    return ok;
}

inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring w(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], need);
    return w;
}

// 后台拉取 + 缓存。SetSong 非阻塞；Lines()/Status() 给主线程每帧读。
class LyricFetcher {
public:
    LyricFetcher() : worker_([this] { Work(); }) {}
    ~LyricFetcher() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            quit_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    LyricFetcher(const LyricFetcher&) = delete;
    LyricFetcher& operator=(const LyricFetcher&) = delete;

    // 返回 true 表示「换歌了」（用于日志/清状态）。
    bool SetSong(const std::string& artist, const std::string& title) {
        if (title.empty()) return false;
        const std::string key = artist + "|" + title;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (key == active_key_) return false;
            active_key_ = key;
            pending_key_ = key;
            pending_artist_ = artist;
            pending_title_ = title;
            lines_.clear();
            status_ = "loading";
        }
        cv_.notify_all();
        return true;
    }

    std::vector<LyricLine> Lines() const {
        std::lock_guard<std::mutex> lock(mu_);
        return lines_;
    }
    std::string Status() const {
        std::lock_guard<std::mutex> lock(mu_);
        return status_;
    }
    // 失败原因（代理 / 直连各自报什么），供日志定位，不参与渲染。
    std::string LastError() const {
        std::lock_guard<std::mutex> lock(mu_);
        return error_;
    }
    // 手动指定代理（rules.json 的 lyrics_proxy），空串表示自动探测。
    // 歌词繁转简。查询仍用原始标题（繁体标题在 LRCLIB 里匹配率更高），
    // 只把显示用的文本转成简体 —— 用户看到的是「故事的小黄花」而不是「小黃花」。
    void SetSimplify(bool on) {
        std::lock_guard<std::mutex> lock(mu_);
        simplify_ = on;
    }

    void SetProxy(const std::wstring& proxy) {
        std::lock_guard<std::mutex> lock(mu_);
        proxy_ = proxy;
    }

private:
    void Work() {
        for (;;) {
            std::string key, artist, title;
            std::wstring proxy;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this] { return quit_ || !pending_key_.empty(); });
                if (quit_) return;
                key = pending_key_;
                artist = pending_artist_;
                title = pending_title_;
                pending_key_.clear();
                proxy = proxy_;
            }

            std::vector<LyricLine> result;
            std::string status = "none";
            bool cached = false;
            {
                std::lock_guard<std::mutex> lock(mu_);
                const auto it = cache_.find(key);
                if (it != cache_.end()) {
                    result = it->second;
                    cached = true;
                    status = result.empty() ? "none" : "ok";
                }
            }

            if (!cached) {
                const std::string q = "/api/search?track_name=" + UrlEncodeUtf8(title) +
                                      "&artist_name=" + UrlEncodeUtf8(artist);
                std::string body, http_error;
                if (HttpGetUtf8(L"lrclib.net", Utf8ToWide(q), &body, &http_error, proxy)) {
                    std::string lrc = ExtractFirstNonEmptyJsonString(body, "syncedLyrics");
                    if (simplify_) lrc = ToSimplified(lrc);
                    result = ParseLrc(lrc);
                    status = result.empty() ? "none" : "ok";
                } else {
                    status = "error";
                    std::lock_guard<std::mutex> lock(mu_);
                    error_ = http_error;
                }
                // 成功与「查无此歌」都缓存；网络错误不缓存，换歌时可再试。
                if (status != "error") {
                    std::lock_guard<std::mutex> lock(mu_);
                    cache_[key] = result;
                }
            }

            {
                std::lock_guard<std::mutex> lock(mu_);
                if (key == active_key_) {  // 歌没再切，结果才算数
                    lines_ = result;
                    status_ = status;
                }
            }
        }
    }

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::thread worker_;
    bool quit_ = false;
    std::string pending_key_, pending_artist_, pending_title_, active_key_;
    bool simplify_ = true;  // 显示用简体（查询仍用原文）
    std::vector<LyricLine> lines_;
    std::string status_ = "idle";
    std::string error_;
    std::wstring proxy_;
    std::map<std::string, std::vector<LyricLine>> cache_;
};

}  // namespace win32
}  // namespace diw

#endif  // DIW_SRC_WIN32_LYRICS_HPP
