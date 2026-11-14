// 繁转简探针：直接调产品代码 bi::win32::ToSimplified，验证系统 locale 能转换。
// 依赖 zh-CN 语言包（本机是中文系统）；若系统没装，ToSimplified 会原样返回，
// 此时这里会 FAIL —— 那说明「显示繁体」在这台机器上无法靠 LCMapStringEx 解决，
// 需要换内置映射表。所以这个探针也是「要不要上大词表」的判据。
#include "../src/win32/hanzi.hpp"

#include <cstdio>
#include <string>

int main() {
    struct Case {
        const char* in;
        const char* want;
    };
    const Case cases[] = {
        {"故事的小黃花", "故事的小黄花"},
        {"從出生那年就飄著", "从出生那年就飘着"},
        {"童年的盪鞦韆", "童年的荡秋千"},
        {"愛笑的眼睛", "爱笑的眼睛"},
        {"Falling Slowly", "Falling Slowly"},   // 非中文必须原样返回
        {"", ""},                               // 空串不能崩
    };
    int failed = 0;
    for (const auto& c : cases) {
        const std::string got = bi::win32::ToSimplified(c.in);
        const bool ok = got == c.want;
        std::printf("  %s  \"%s\" -> \"%s\"", ok ? "ok  " : "FAIL", c.in, got.c_str());
        if (!ok) std::printf("  (want \"%s\")", c.want);
        std::printf("\n");
        if (!ok) ++failed;
    }
    std::printf("hanzi_probe: failed=%d\n", failed);
    return failed == 0 ? 0 : 1;
}
