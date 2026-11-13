// DynamicIsland-Win / 规则持久化与热加载（Win32）
//
// 配置文件位置：%APPDATA%\DynamicIsland-Win\rules.json
//   - 放在 APPDATA 而不是程序目录：程序目录可能只读（Program Files），
//     而且用户升级覆盖 exe 时不该把配置一起冲掉。
//   - 文件不存在时自动写一份带说明的默认配置，用户改坏了也能重新生成。
//
// 热加载：每 500ms 查一次 mtime，变了就重读。不需要重启程序，也不需要 IPC。
// 解析失败时**保留上一份可用配置**，只记录错误 —— 用户写错一个逗号不该让胶囊停摆。
#ifndef DIW_SRC_WIN32_RULES_IO_HPP
#define DIW_SRC_WIN32_RULES_IO_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "rules.hpp"

namespace diw {
namespace win32 {

class RulesStore {
public:
    // 优先 %APPDATA%，拿不到则退到 exe 同目录。
    void Init(const std::wstring& exe_dir) {
        PWSTR appdata = nullptr;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata)) &&
            appdata != nullptr) {
            dir_ = std::wstring(appdata) + L"\\DynamicIsland-Win";
            ::CoTaskMemFree(appdata);
        } else {
            dir_ = exe_dir;
        }
        ::CreateDirectoryW(dir_.c_str(), nullptr);  // 已存在会返回 ERROR_ALREADY_EXISTS，忽略
        path_ = dir_ + L"\\rules.json";

        EnsureFile();
        Load();
    }

    const RuleConfig& Config() const { return config_; }
    const std::wstring& Path() const { return path_; }
    std::wstring Directory() const { return dir_; }
    const std::string& LastError() const { return last_error_; }
    bool LastLoadOk() const { return last_load_ok_; }

    // 返回 true 表示配置文件真的变了且被重新解析（成功或失败都会返回 true）。
    bool ReloadIfChanged() {
        const std::int64_t stamp = StampOf(path_);
        if (stamp == 0 || stamp == stamp_) return false;
        stamp_ = stamp;
        Load();
        return true;
    }

    bool Save(const RuleConfig& cfg) {
        const std::string text = cfg.ToJson();
        // 不能直接 trunc 再写：写到一半崩溃/断电/磁盘满会留下**半截 JSON**，
        // 下次启动读到坏文件，用户攒下的全部设置就没了。走临时文件 + 原子替换。
        if (!WriteAtomic(path_, text)) {
            last_error_ = "无法写入配置文件";
            return false;
        }
        config_ = cfg;
        stamp_ = StampOf(path_);
        last_load_ok_ = true;
        last_error_.clear();
        return true;
    }

private:
    // 先写 `<path>.tmp`，成功后再替换目标文件。替换失败（如另一个进程占着）
    // 时删掉临时文件，保持原文件不动 —— 最坏情况是这次没保存成功，而不是丢配置。
    static bool WriteAtomic(const std::wstring& path, const std::string& text) {
        const std::wstring tmp = path + L".tmp";
        {
            std::ofstream out(tmp, std::ios::out | std::ios::trunc | std::ios::binary);
            if (!out) return false;
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            if (!out) {  // 磁盘满等：不能把半截临时文件留下来当配置
                out.close();
                ::DeleteFileW(tmp.c_str());
                return false;
            }
        }
        if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            ::DeleteFileW(tmp.c_str());
            return false;
        }
        return true;
    }

    static std::int64_t StampOf(const std::wstring& path) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
        ULARGE_INTEGER u{};
        u.LowPart = data.ftLastWriteTime.dwLowDateTime;
        u.HighPart = data.ftLastWriteTime.dwHighDateTime;
        return static_cast<std::int64_t>(u.QuadPart);
    }

    void EnsureFile() {
        if (StampOf(path_) != 0) return;
        WriteAtomic(path_, RuleConfig::Defaults().ToJson());
    }

    void Load() {
        std::ifstream in(path_, std::ios::in | std::ios::binary);
        if (!in) {
            last_error_ = "无法读取配置文件，使用内置默认值";
            last_load_ok_ = false;
            return;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        std::string text = buffer.str();

        // UTF-8 BOM：记事本另存为 UTF-8 会带，strtod/解析器都不认这三个字节。
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF) {
            text.erase(0, 3);
        }

        json::Value root;
        std::string error;
        if (!json::Parse(text, root, &error)) {
            // 保留上一份可用配置：用户写错一个逗号，不该让胶囊改用默认值。
            last_error_ = "配置解析失败：" + error;
            last_load_ok_ = false;
            return;
        }
        config_ = RuleConfig::FromJson(root);
        last_load_ok_ = true;
        last_error_.clear();
        stamp_ = StampOf(path_);
    }

    RuleConfig config_;
    std::wstring dir_;
    std::wstring path_;
    std::string last_error_;
    bool last_load_ok_ = true;
    std::int64_t stamp_ = 0;
};

}  // namespace win32
}  // namespace diw

#endif  // DIW_SRC_WIN32_RULES_IO_HPP
