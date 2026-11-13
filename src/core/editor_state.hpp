#pragma once
// 编辑器「此刻在改哪个文件」的纯数据与纯函数。
//
// 为什么单独一层：这一块**唯一**的数据源是窗口标题，而窗口标题是人写的字符串 ——
// 「renderer.hpp — DynamicIsland-Win — Trae CN」这句话里，哪一段是文件、哪一段是
// 项目、哪一段是应用，全靠猜规则。规则猜错的表现是「岛上显示了一个半截路径」，
// 而这种错必须能被单测钉住，否则每加一个编辑器就要重新肉眼验证一遍。
//
// 支持的编辑器都是「标题里带文件名」的那一类：VS Code 及其所有分支（Cursor/Trae/
// Windsurf/VSCodium/Kiro/Qoder）、JetBrains 全家桶、Notepad++、Sublime、Zed。
// 终端里的 vim/nvim 拿不到 —— 那是终端，不是编辑器窗口，会在 README 里写清楚。

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace diw {

struct EditorState {
    std::string id;         // editor:<brand>
    std::string brand;      // vscode / cursor / trae-cn / idea …
    std::string label;      // "Trae CN"
    std::string file;       // 文件名（含扩展名，不含目录）
    std::string project;    // 工作区 / 项目名
    std::string language;   // C++ / TypeScript / Markdown …（由扩展名推断）
    std::string exe_stem;   // Code / Trae CN（判重与聚焦用）

    bool dirty = false;       // 标题前面那个实心圆点 = 有未保存修改
    bool foreground = false;  // 此刻是不是它在前台
    bool remote = false;      // 远程开发（SSH / WSL / Dev Container）—— 标题里有提示
    std::int64_t last_fg_ms = 0;
};

namespace editor_detail {

inline bool EndsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

inline std::string Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

inline std::string Trim(std::string s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

}  // namespace editor_detail

// 品牌表。`stems` 用 '|' 分隔多个 exe 主名（JetBrains 各产品共用一套命名规则）。
struct EditorBrandDef {
    const char* id;
    const char* label;
    const char* stems;
    // 窗口标题末尾那段应用名（用于剥后缀），多个候选用 '|' 分隔。
    // 需要多候选是因为同一个编辑器在不同版本/主题下写法不同：
    // VS Code 官方是 "Visual Studio Code"，但有些发行版/本地化只写 "VS Code"。
    // JetBrains 还会跟版本号，所以匹配规则是「前缀 + 后面是空格/括号」。
    const char* title_tail;
};

inline const EditorBrandDef* EditorBrandTable(int* count) {
    static const EditorBrandDef kBrands[] = {
        {"vscode", "VS Code", "Code", "Visual Studio Code|VS Code"},
        {"vscode-insiders", "VS Code Insiders", "Code - Insiders",
         "Visual Studio Code - Insiders|VS Code Insiders"},
        {"cursor", "Cursor", "Cursor", "Cursor"},
        {"trae", "Trae", "Trae", "Trae"},
        {"trae-cn", "Trae CN", "Trae CN", "Trae CN"},
        {"windsurf", "Windsurf", "Windsurf", "Windsurf"},
        {"vscodium", "VSCodium", "VSCodium", "VSCodium"},
        {"kiro", "Kiro", "Kiro", "Kiro"},
        {"qoder", "Qoder", "Qoder", "Qoder"},
        {"zed", "Zed", "zed", "Zed"},
        {"sublime", "Sublime Text", "sublime_text", "Sublime Text"},
        {"notepadpp", "Notepad++", "notepad++", "Notepad++"},
        {"idea", "IntelliJ IDEA", "idea64", "IntelliJ IDEA"},
        {"pycharm", "PyCharm", "pycharm64", "PyCharm"},
        {"webstorm", "WebStorm", "webstorm64", "WebStorm"},
        {"goland", "GoLand", "goland64", "GoLand"},
        {"clion", "CLion", "clion64", "CLion"},
        {"rider", "Rider", "rider64", "Rider"},
        {"phpstorm", "PhpStorm", "phpstorm64", "PhpStorm"},
        {"rubymine", "RubyMine", "rubymine64", "RubyMine"},
        {"android-studio", "Android Studio", "studio64", "Android Studio"},
        {"datagrip", "DataGrip", "datagrip64", "DataGrip"},
    };
    if (count != nullptr) *count = static_cast<int>(sizeof(kBrands) / sizeof(kBrands[0]));
    return kBrands;
}

// exe 主名（不含 .exe）→ 品牌。大小写不敏感。
inline const EditorBrandDef* EditorBrandByExe(const std::string& stem) {
    if (stem.empty()) return nullptr;
    const std::string want = editor_detail::Lower(stem);
    int count = 0;
    const EditorBrandDef* table = EditorBrandTable(&count);
    for (int i = 0; i < count; ++i) {
        std::string stems = table[i].stems;
        std::size_t start = 0;
        while (start <= stems.size()) {
            const std::size_t bar = stems.find('|', start);
            const std::string one =
                stems.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
            if (!one.empty() && editor_detail::Lower(one) == want) return &table[i];
            if (bar == std::string::npos) break;
            start = bar + 1;
        }
    }
    return nullptr;
}

// 标题末尾那段是不是某个已知编辑器 → 返回它。
// 只认「等于」或「以它开头且后面跟着空格/版本号」，避免把文件名里恰好含
// "Zed" 的句子误判成编辑器。
inline const EditorBrandDef* EditorBrandByTitleTail(const std::string& tail) {
    if (tail.empty()) return nullptr;
    const std::string t = editor_detail::Trim(tail);
    int count = 0;
    const EditorBrandDef* table = EditorBrandTable(&count);
    const EditorBrandDef* best = nullptr;
    std::size_t best_len = 0;
    for (int i = 0; i < count; ++i) {
        const std::string candidates = table[i].title_tail;
        std::size_t start = 0;
        while (start <= candidates.size()) {
            const std::size_t bar = candidates.find('|', start);
            const std::string name =
                candidates.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
            if (!name.empty() && t.size() >= name.size() && t.compare(0, name.size(), name) == 0) {
                // 后面必须是结尾、空格（版本号）、或 '[' / '('（Notepad++ 的管理员标记）
                bool ok = true;
                if (t.size() > name.size()) {
                    const char c = t[name.size()];
                    ok = (c == ' ' || c == '[' || c == '(' || c == '\t');
                }
                if (ok && name.size() > best_len) {  // 最长的赢：Trae CN 要盖过 Trae
                    best = &table[i];
                    best_len = name.size();
                }
            }
            if (bar == std::string::npos) break;
            start = bar + 1;
        }
    }
    return best;
}

// 标题里的分隔符有三套：VS Code 用 " - "，JetBrains 用 " – "（en dash），
// 有些主题/本地化会变成 " — "（em dash）。统一按「最后一个分隔符」切。
inline std::size_t LastSeparator(const std::string& s) {
    static const char* kSeps[] = {" - ", " \xE2\x80\x93 ", " \xE2\x80\x94 "};  // - – —
    std::size_t best = std::string::npos;
    for (const char* sep : kSeps) {
        const std::string one(sep);
        const std::size_t at = s.rfind(one);
        if (at != std::string::npos && (best == std::string::npos || at > best)) best = at;
    }
    return best;
}

inline std::size_t SeparatorLen(const std::string& s, std::size_t at) {
    if (at == std::string::npos) return 0;
    if (s.compare(at, 3, " - ") == 0) return 3;
    return 5;  // " – " / " — " 都是 3 字节 UTF-8 + 两个空格 = 5
}

// 扩展名 → 语言名。只收「看一眼就知道是什么语言」的常见项，
// 剩下的宁可留空，也不要显示 "Unknown"。
inline std::string LanguageOf(const std::string& file) {
    const std::size_t dot = file.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= file.size()) return std::string();
    const std::string ext = editor_detail::Lower(file.substr(dot + 1));
    if (ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "hpp" || ext == "hxx") return "C++";
    if (ext == "h") return "C/C++ 头文件";
    if (ext == "c") return "C";
    if (ext == "cs") return "C#";
    if (ext == "ts" || ext == "tsx") return "TypeScript";
    if (ext == "js" || ext == "jsx" || ext == "mjs" || ext == "cjs") return "JavaScript";
    if (ext == "py") return "Python";
    if (ext == "go") return "Go";
    if (ext == "rs") return "Rust";
    if (ext == "java") return "Java";
    if (ext == "kt" || ext == "kts") return "Kotlin";
    if (ext == "swift") return "Swift";
    if (ext == "rb") return "Ruby";
    if (ext == "php") return "PHP";
    if (ext == "lua") return "Lua";
    if (ext == "sh" || ext == "bash" || ext == "zsh") return "Shell";
    if (ext == "ps1") return "PowerShell";
    if (ext == "sql") return "SQL";
    if (ext == "html" || ext == "htm") return "HTML";
    if (ext == "css" || ext == "scss" || ext == "less") return "CSS";
    if (ext == "json" || ext == "jsonc") return "JSON";
    if (ext == "yaml" || ext == "yml") return "YAML";
    if (ext == "toml") return "TOML";
    if (ext == "xml") return "XML";
    if (ext == "md" || ext == "markdown") return "Markdown";
    if (ext == "txt") return "纯文本";
    if (ext == "cmake" || ext == "mk") return "构建脚本";
    return std::string();
}

// 从窗口标题里把「文件 / 项目 / 应用」拆出来。
//
// 输入形如：
//   renderer.hpp — DynamicIsland-Win — Trae CN        （VS Code 系，有工作区）
//   ● app.ts — web — Cursor                           （有未保存修改）
//   README.md - Notepad++                             （无工作区）
//   main.cpp – order-service – IntelliJ IDEA 2024.1   （JetBrains，带版本号）
//
// 规则：从右往左剥。最后一段是应用名（必须能匹配品牌表，否则整句都不认），
// 倒数第二段是项目，剩下全是文件名（文件名本身可能含 " - "）。
inline bool ParseEditorTitle(const std::string& title, std::string* file, std::string* project,
                             const EditorBrandDef** brand) {
    if (file != nullptr) file->clear();
    if (project != nullptr) project->clear();
    if (brand != nullptr) *brand = nullptr;

    std::string rest = editor_detail::Trim(title);
    if (rest.empty()) return false;

    // 未保存标记：VS Code 系是实心圆点，JetBrains 是星号。
    bool dirty = false;
    if (rest.size() >= 3 && rest.compare(0, 3, "\xE2\x97\x8F") == 0) {  // ●
        dirty = true;
        rest = editor_detail::Trim(rest.substr(3));
    } else if (!rest.empty() && rest[0] == '*') {
        dirty = true;
        rest = editor_detail::Trim(rest.substr(1));
    }
    (void)dirty;

    // 1) 剥应用后缀
    std::size_t sep = LastSeparator(rest);
    if (sep == std::string::npos) return false;
    const std::string tail = rest.substr(sep + SeparatorLen(rest, sep));
    const EditorBrandDef* found = EditorBrandByTitleTail(tail);
    if (found == nullptr) return false;
    if (brand != nullptr) *brand = found;
    rest = editor_detail::Trim(rest.substr(0, sep));

    // 2) 再切一段：右边是项目，左边是文件
    const std::size_t sep2 = LastSeparator(rest);
    if (sep2 == std::string::npos) {
        if (file != nullptr) *file = rest;  // 无工作区：整句就是文件名
        return true;
    }
    if (project != nullptr) *project = editor_detail::Trim(rest.substr(sep2 + SeparatorLen(rest, sep2)));
    const std::string left = editor_detail::Trim(rest.substr(0, sep2));
    if (file != nullptr) *file = left;
    return true;
}

// 标题是不是「编辑器自己的界面」（欢迎页 / 设置 / 扩展市场 …），
// 这类标题没有文件名，出岛没有任何信息量。
inline bool IsEditorChrome(const std::string& file) {
    if (file.empty()) return true;
    static const char* kChrome[] = {"Welcome", "Settings", "Extensions", "Keyboard Shortcuts",
                                    "Release Notes", "Trae CN", "Cursor", "VS Code",
                                    "Visual Studio Code", "Windsurf", "VSCodium", "Kiro",
                                    "Qoder", "Zed", "Sublime Text", "Notepad++"};
    for (const char* k : kChrome) {
        if (file == k) return true;
    }
    return false;
}

// 组装一份可用的状态。解析失败返回 false（调用方据此跳过，不产出活动）。
inline bool BuildEditorState(const std::string& exe_stem, const std::string& title,
                             EditorState* out) {
    if (out == nullptr || title.empty()) return false;
    *out = EditorState{};
    const EditorBrandDef* by_exe = EditorBrandByExe(exe_stem);
    std::string file;
    std::string project;
    const EditorBrandDef* by_title = nullptr;
    const bool parsed = ParseEditorTitle(title, &file, &project, &by_title);

    const EditorBrandDef* brand = by_exe != nullptr ? by_exe : by_title;
    if (brand == nullptr) return false;  // 既不是编辑器 exe，标题也不像编辑器
    out->brand = brand->id;
    out->label = brand->label;
    out->exe_stem = exe_stem;
    out->file = file;
    out->project = project;
    out->language = LanguageOf(file);
    // 两种未保存标记都要认：VS Code 系是 ●（U+25CF），JetBrains 是 *。
    // 早先这里只判了 ●，于是用 * 的编辑器「标记被剥掉了、dirty 却没置上」——
    // 界面上表现为「明明改了没保存，岛上什么提示都没有」。
    out->dirty = (title.size() >= 3 && title.compare(0, 3, "\xE2\x97\x8F") == 0) ||
                 (!title.empty() && title[0] == '*');
    out->remote = title.find("[SSH") != std::string::npos ||
                  title.find("[WSL") != std::string::npos ||
                  title.find("Remote") != std::string::npos;
    out->id = std::string("editor:") + brand->id;
    if (!parsed && by_exe != nullptr) {
        // 只认出 exe、标题解析失败（比如刚启动还没打开文件）：
        // 保留品牌但清掉文件，交给调用方用 IsEditorChrome 决定要不要出岛。
        out->file.clear();
    }
    return true;
}

}  // namespace diw
