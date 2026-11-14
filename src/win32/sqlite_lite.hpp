// balance_island / 极简 SQLite 只读封装（动态绑定系统自带的 winsqlite3.dll）
//
// 为什么需要它：
//   Cursor 和 ZCode 把会话存在 SQLite 里（`state.vscdb` / `db.sqlite`），
//   这是唯一能读到「它此刻在干什么」的地方。但它们都是别人家的私有结构，
//   我们只需要「打开、跑一条查询、取字符串」三件事。
//
// 为什么动态加载而不是引入 sqlite3 源码 / vcpkg：
//   1. Windows 10 起系统自带 `winsqlite3.dll`，零依赖、零体积；
//   2. 这个项目坚持「clone 下来 cl.exe 就能编」，加一个第三方库会破例；
//   3. 少了它也不影响其它 provider —— 加载失败就当这两家不存在。
//
// 只读语义：一律 SQLITE_OPEN_READONLY + URI。**绝不写别人的数据库**：
//   Cursor 正在运行时持有写锁，我们用 `?mode=ro` 打开，读不到最新事务
//   （WAL 未 checkpoint 的部分）就等下一秒再读，绝不尝试 checkpoint。
#ifndef BI_SRC_WIN32_SQLITE_LITE_HPP
#define BI_SRC_WIN32_SQLITE_LITE_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bi {
namespace win32 {

// SQLite 的常量（避免依赖 sqlite3.h：那会把整个第三方头文件拖进来）
constexpr int kSqliteOk = 0;
constexpr int kSqliteRow = 100;
constexpr int kSqliteDone = 101;
constexpr int kSqliteOpenReadOnly = 0x00000001;
constexpr int kSqliteOpenUri = 0x00000040;
constexpr int kSqliteOpenNoMutex = 0x00008000;

class SqliteLite {
public:
    // sqlite3 与 sqlite3_stmt 都是不透明结构，这里只当作句柄用。
    struct Db;
    struct Stmt;

    SqliteLite() = default;
    ~SqliteLite() { Close(); }
    SqliteLite(const SqliteLite&) = delete;
    SqliteLite& operator=(const SqliteLite&) = delete;

    // 全进程共享一次 LoadLibrary：多个 provider 会各开一个库，
    // 每个都 LoadLibrary 一遍会让模块引用计数虚高、也浪费一次磁盘寻道。
    static bool ApiReady() {
        static const bool ready = LoadApi();
        return ready;
    }

    // path 是 Windows 路径。内部转成 `file:...?mode=ro` 的 URI 形式。
    bool Open(const std::wstring& path) { return OpenEx(path, false); }

    // immutable=1：告诉 sqlite「这个文件不会变，别加锁、别看 journal」。
    // 浏览器的 History / Favicons 在浏览器运行期间被独占（普通 mode=ro 打开
    // 直接 SQLITE_BUSY），但我们是只读一次就走，用 immutable 是最省事且不打扰
    // 用户浏览器的做法。代价：如果恰好读到写了一半的页，查询会返回错误 —— 
    // 我们每一处都有「查不到就降级」的分支，所以最坏情况只是这一次没数据。
    bool OpenImmutable(const std::wstring& path) { return OpenEx(path, true); }

    bool OpenEx(const std::wstring& path, bool immutable) {
        Close();
        if (!ApiReady()) return false;
        std::string uri = "file:";
        for (const wchar_t wc : path) {
            if (wc == L'\\') {
                uri += '/';
            } else if (wc < 128) {
                uri += static_cast<char>(wc);
            } else {
                // 非 ASCII 路径（中文用户名）要按 UTF-8 塞进 URI，
                // 否则 sqlite 会按 ANSI 解释、打开一个不存在的文件。
                char buf[8] = {0};
                const int n = ::WideCharToMultiByte(CP_UTF8, 0, &wc, 1, buf, sizeof(buf), nullptr, nullptr);
                if (n > 0) uri.append(buf, static_cast<std::size_t>(n));
            }
        }
        uri += immutable ? "?mode=ro&immutable=1" : "?mode=ro";
        Db* db = nullptr;
        const int rc = open_v2_(uri.c_str(), &db, kSqliteOpenReadOnly | kSqliteOpenUri | kSqliteOpenNoMutex,
                                nullptr);
        if (rc != kSqliteOk || db == nullptr) {
            if (db != nullptr) close_v2_(db);
            return false;
        }
        db_ = db;
        return true;
    }

    void Close() {
        if (db_ != nullptr && close_v2_ != nullptr) close_v2_(db_);
        db_ = nullptr;
    }

    bool Ready() const { return db_ != nullptr; }

    // 一条查询 + 逐行回调。返回 false 时 LastError() 有原因。
    // params 用绑定而不是拼接：会话 id 里可能出现引号，拼接会拼出语法错误
    // 甚至注入（虽然来源是我们自己，但绑定的写法不会让人误以为可以不转义）。
    bool Query(const char* sql, const std::vector<std::string>& params,
               const std::function<bool(const std::vector<std::string>&, const std::vector<bool>&)>& on_row) {
        if (db_ == nullptr) return false;
        Stmt* st = nullptr;
        if (prepare_v2_(db_, sql, -1, &st, nullptr) != kSqliteOk || st == nullptr) {
            err_ = errmsg_ != nullptr ? errmsg_(db_) : "prepare failed";
            return false;
        }
        for (std::size_t i = 0; i < params.size(); ++i) {
            bind_text_(st, static_cast<int>(i + 1), params[i].c_str(), static_cast<int>(params[i].size()),
                       Transient());
        }
        bool ok = true;
        int rc = step_(st);
        while (rc == kSqliteRow) {
            const int cols = column_count_(st);
            std::vector<std::string> text(static_cast<std::size_t>(cols));
            std::vector<bool> is_null(static_cast<std::size_t>(cols), false);
            for (int c = 0; c < cols; ++c) {
                if (column_type_(st, c) == kSqliteNull) {
                    is_null[static_cast<std::size_t>(c)] = true;
                    continue;
                }
                const unsigned char* p = column_text_(st, c);
                const int n = column_bytes_(st, c);
                if (p != nullptr && n > 0) {
                    text[static_cast<std::size_t>(c)].assign(reinterpret_cast<const char*>(p),
                                                             static_cast<std::size_t>(n));
                }
            }
            if (on_row && !on_row(text, is_null)) break;
            rc = step_(st);
        }
        if (rc != kSqliteDone && rc != kSqliteRow) {
            ok = false;
            err_ = errmsg_ != nullptr ? errmsg_(db_) : "step failed";
        }
        finalize_(st);
        return ok;
    }

    const std::string& LastError() const { return err_; }

private:
    static constexpr int kSqliteNull = 5;
    // SQLITE_TRANSIENT：让 sqlite 自己拷贝一份字符串。用 STATIC 的话，
    // 我们传进去的临时 std::string 在 step 之前就析构了。
    // 它是 `((sqlite3_destructor_type)-1)`，C++ 里不能用 constexpr 表达这种转换。
    static void (*Transient())(void*) { return reinterpret_cast<void (*)(void*)>(-1); }

    static bool LoadApi() {
        HMODULE mod = ::LoadLibraryW(L"winsqlite3.dll");
        if (mod == nullptr) return false;
        auto sym = [mod](const char* name) -> void* {
            return reinterpret_cast<void*>(::GetProcAddress(mod, name));
        };
        open_v2_ = reinterpret_cast<decltype(open_v2_)>(sym("sqlite3_open_v2"));
        close_v2_ = reinterpret_cast<decltype(close_v2_)>(sym("sqlite3_close_v2"));
        prepare_v2_ = reinterpret_cast<decltype(prepare_v2_)>(sym("sqlite3_prepare_v2"));
        step_ = reinterpret_cast<decltype(step_)>(sym("sqlite3_step"));
        finalize_ = reinterpret_cast<decltype(finalize_)>(sym("sqlite3_finalize"));
        column_count_ = reinterpret_cast<decltype(column_count_)>(sym("sqlite3_column_count"));
        column_type_ = reinterpret_cast<decltype(column_type_)>(sym("sqlite3_column_type"));
        column_text_ = reinterpret_cast<decltype(column_text_)>(sym("sqlite3_column_text"));
        column_bytes_ = reinterpret_cast<decltype(column_bytes_)>(sym("sqlite3_column_bytes"));
        bind_text_ = reinterpret_cast<decltype(bind_text_)>(sym("sqlite3_bind_text"));
        errmsg_ = reinterpret_cast<decltype(errmsg_)>(sym("sqlite3_errmsg"));
        return open_v2_ != nullptr && prepare_v2_ != nullptr && step_ != nullptr && finalize_ != nullptr &&
               column_text_ != nullptr && column_bytes_ != nullptr && bind_text_ != nullptr;
    }

    using OpenV2Fn = int (*)(const char*, Db**, int, const char*);
    using CloseV2Fn = int (*)(Db*);
    using PrepareFn = int (*)(Db*, const char*, int, Stmt**, const char**);
    using StepFn = int (*)(Stmt*);
    using FinalizeFn = int (*)(Stmt*);
    using ColumnCountFn = int (*)(Stmt*);
    using ColumnTypeFn = int (*)(Stmt*, int);
    using ColumnTextFn = const unsigned char* (*)(Stmt*, int);
    using ColumnBytesFn = int (*)(Stmt*, int);
    using BindTextFn = int (*)(Stmt*, int, const char*, int, void (*)(void*));
    using ErrMsgFn = const char* (*)(Db*);

    static inline OpenV2Fn open_v2_ = nullptr;
    static inline CloseV2Fn close_v2_ = nullptr;
    static inline PrepareFn prepare_v2_ = nullptr;
    static inline StepFn step_ = nullptr;
    static inline FinalizeFn finalize_ = nullptr;
    static inline ColumnCountFn column_count_ = nullptr;
    static inline ColumnTypeFn column_type_ = nullptr;
    static inline ColumnTextFn column_text_ = nullptr;
    static inline ColumnBytesFn column_bytes_ = nullptr;
    static inline BindTextFn bind_text_ = nullptr;
    static inline ErrMsgFn errmsg_ = nullptr;

    Db* db_ = nullptr;
    std::string err_;
};

// 把 "file:///c%3A/Users/ow1/Desktop/x" 或 "file:///c:/Users/..." 解成 Windows 路径。
// Cursor 的 workspace.json 存的是 VS Code 的 URI 编码形式，混着 %3A 与正斜杠。
inline std::wstring UriToWindowsPath(const std::string& uri) {
    std::string s = uri;
    const std::string prefix = "file://";
    if (s.compare(0, prefix.size(), prefix) == 0) s = s.substr(prefix.size());
    std::string decoded;
    decoded.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(s[i + 1]);
            const int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        decoded.push_back(s[i]);
    }
    // "/c:/Users/..." -> "c:\Users\..."
    if (decoded.size() > 2 && decoded[0] == '/' && decoded[2] == ':') decoded.erase(decoded.begin());
    std::wstring out;
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, decoded.data(), static_cast<int>(decoded.size()),
                                           nullptr, 0);
    if (need <= 0) return out;
    out.resize(static_cast<std::size_t>(need));
    ::MultiByteToWideChar(CP_UTF8, 0, decoded.data(), static_cast<int>(decoded.size()), &out[0], need);
    for (auto& c : out) {
        if (c == L'/') c = L'\\';
    }
    return out;
}

}  // namespace win32
}  // namespace bi

#endif  // BI_SRC_WIN32_SQLITE_LITE_HPP
