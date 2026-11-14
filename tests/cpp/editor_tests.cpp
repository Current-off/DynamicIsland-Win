// 编辑器窗口标题解析的单测。
//
// 这一层的输入是人写的字符串，规则一旦猜错，岛上的表现是「显示了一个半截文件名」
// 或者「把 GitHub 页面当成编辑器」。所以每条规则都要有一句断言。

#include <cstdio>
#include <cstdlib>
#include <string>

#include "editor_state.hpp"

using namespace bi;

static int g_run = 0;
static int g_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_run;                                                                 \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                        \
    } while (0)

#define CHECK_EQ(actual, expect)                                                              \
    do {                                                                                      \
        ++g_run;                                                                              \
        const std::string a = (actual);                                                       \
        const std::string e = (expect);                                                       \
        if (a != e) {                                                                         \
            ++g_failed;                                                                       \
            std::printf("  FAIL %s:%d  %s\n      got \"%s\"\n      want \"%s\"\n", __FILE__,   \
                        __LINE__, #actual, a.c_str(), e.c_str());                             \
        }                                                                                     \
    } while (0)

// VSCode 系：`file — project — app`
static void TestVsCodeStyle() {
    std::string file, project;
    const EditorBrandDef* brand = nullptr;
    CHECK(ParseEditorTitle("renderer.hpp \xE2\x80\x94 balance_island \xE2\x80\x94 Trae CN", &file,
                           &project, &brand));
    CHECK_EQ(file, "renderer.hpp");
    CHECK_EQ(project, "balance_island");
    // 不能先 CHECK 再解引用：解析失败时 brand 是空的，CHECK 只会记一条失败，
    // 紧接着的 brand->id 就直接段错误了 —— 连失败点都看不到。
    CHECK(brand != nullptr && std::string(brand->id) == "trae-cn");  // 必须赢过 "Trae"

    // 普通 hyphen（Windows 上更常见）
    CHECK(ParseEditorTitle("app.ts - web - Cursor", &file, &project, &brand));
    CHECK_EQ(file, "app.ts");
    CHECK_EQ(project, "web");
    CHECK(brand != nullptr && std::string(brand->id) == "cursor");

    // 未保存标记 ● 要剥掉，不能进文件名
    CHECK(ParseEditorTitle("\xE2\x97\x8F main.cpp - engine - VS Code", &file, &project, &brand));
    CHECK_EQ(file, "main.cpp");
    CHECK_EQ(project, "engine");
    CHECK(brand != nullptr && std::string(brand->id) == "vscode");
}

// 文件名自身含分隔符：只切最右边两段
static void TestTrickyFileName() {
    std::string file, project;
    const EditorBrandDef* brand = nullptr;
    CHECK(ParseEditorTitle("a - b - c.md - proj - VS Code", &file, &project, &brand));
    CHECK_EQ(file, "a - b - c.md");
    CHECK_EQ(project, "proj");
}

// 无工作区：只有两段
static void TestNoWorkspace() {
    std::string file, project;
    const EditorBrandDef* brand = nullptr;
    CHECK(ParseEditorTitle("README.md - Notepad++", &file, &project, &brand));
    CHECK_EQ(file, "README.md");
    CHECK_EQ(project, "");
    CHECK(brand != nullptr && std::string(brand->id) == "notepadpp");

    // Notepad++ 的管理员标记
    CHECK(ParseEditorTitle("hosts - Notepad++ [Administrator]", &file, &project, &brand));
    CHECK_EQ(file, "hosts");
    CHECK(brand != nullptr && std::string(brand->id) == "notepadpp");
}

// JetBrains：en dash + 末尾带版本号
static void TestJetBrains() {
    std::string file, project;
    const EditorBrandDef* brand = nullptr;
    CHECK(ParseEditorTitle("main.cpp \xE2\x80\x93 order-service \xE2\x80\x93 IntelliJ IDEA 2024.1",
                           &file, &project, &brand));
    CHECK_EQ(file, "main.cpp");
    CHECK_EQ(project, "order-service");
    CHECK(brand != nullptr && std::string(brand->id) == "idea");

    CHECK(ParseEditorTitle("train.py \xE2\x80\x93 ml-lab \xE2\x80\x93 PyCharm 2024.2", &file, &project,
                           &brand));
    CHECK(brand != nullptr && std::string(brand->id) == "pycharm");
}

// 不是编辑器：浏览器 / 资源管理器 / 随便一段话
static void TestNotEditor() {
    std::string file, project;
    const EditorBrandDef* brand = nullptr;
    CHECK(!ParseEditorTitle("GitHub - Google Chrome", &file, &project, &brand));
    CHECK(!ParseEditorTitle("C:\\Windows", &file, &project, &brand));
    CHECK(!ParseEditorTitle("Trae CN", &file, &project, &brand));  // 没有分隔符
    CHECK(!ParseEditorTitle("", &file, &project, &brand));
}

// 品牌识别：exe 名大小写不敏感，多 stem 用 '|' 分隔
static void TestBrandByExe() {
    CHECK(EditorBrandByExe("Code") != nullptr && std::string(EditorBrandByExe("Code")->id) == "vscode");
    CHECK(EditorBrandByExe("code") != nullptr);
    CHECK(EditorBrandByExe("Trae CN") != nullptr &&
          std::string(EditorBrandByExe("Trae CN")->id) == "trae-cn");
    CHECK(EditorBrandByExe("idea64") != nullptr &&
          std::string(EditorBrandByExe("idea64")->id) == "idea");
    CHECK(EditorBrandByExe("chrome") == nullptr);
    CHECK(EditorBrandByExe("") == nullptr);
}

// 语言推断
static void TestLanguage() {
    CHECK_EQ(LanguageOf("renderer.hpp"), "C++");
    CHECK_EQ(LanguageOf("a.ts"), "TypeScript");
    CHECK_EQ(LanguageOf("README.md"), "Markdown");
    CHECK_EQ(LanguageOf("x.unknownext"), "");
    CHECK_EQ(LanguageOf("Makefile"), "");
}

// 编辑器自身的界面（欢迎/设置）不该出岛
static void TestChrome() {
    CHECK(IsEditorChrome("Welcome"));
    CHECK(IsEditorChrome("Settings"));
    CHECK(IsEditorChrome(""));
    CHECK(!IsEditorChrome("renderer.hpp"));
}

// 端到端：exe + 标题 → 完整状态
static void TestBuildState() {
    EditorState st;
    CHECK(BuildEditorState("Trae CN",
                           "renderer.hpp \xE2\x80\x94 balance_island \xE2\x80\x94 Trae CN", &st));
    CHECK_EQ(st.id, "editor:trae-cn");
    CHECK_EQ(st.label, "Trae CN");
    CHECK_EQ(st.file, "renderer.hpp");
    CHECK_EQ(st.project, "balance_island");
    CHECK_EQ(st.language, "C++");

    // 脏标记
    EditorState dirty;
    CHECK(BuildEditorState("Code", "\xE2\x97\x8F app.ts - web - VS Code", &dirty));
    CHECK(dirty.dirty);

    // 只开了窗口、没打开文件：品牌能认出来，但没有文件名
    EditorState idle;
    CHECK(BuildEditorState("Trae CN", "Trae CN", &idle));
    CHECK_EQ(idle.file, "");
    CHECK(IsEditorChrome(idle.file));

    // 既不是编辑器 exe、标题也不像编辑器
    EditorState no;
    CHECK(!BuildEditorState("chrome", "GitHub - Google Chrome", &no));
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // 崩在哪一步都要能看见
    TestVsCodeStyle();
    TestTrickyFileName();
    TestNoWorkspace();
    TestJetBrains();
    TestNotEditor();
    TestBrandByExe();
    TestLanguage();
    TestChrome();
    TestBuildState();
    std::printf("editor_tests: run=%d failed=%d\n", g_run, g_failed);
    return g_failed == 0 ? 0 : 1;
}
