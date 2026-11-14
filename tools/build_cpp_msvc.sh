#!/usr/bin/env bash
# 用本机 Visual Studio 的 MSVC 直接编译并运行 C++ 测试与桌面胶囊程序（无需 CMake）。
# 适用环境：Git Bash + VS 安装在 D:\code\vs\vs（如不同，改下方变量）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Windows 风格路径（cl.exe / link.exe 只认这种形式）
MSVC_WIN='D:\code\vs\vs\VC\Tools\MSVC\14.51.36231'
KIT_WIN='D:\Windows Kits\10'
SDK_VER='10.0.26100.0'
CL_BIN='/d/code/vs/vs/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64'

if [ ! -f "$CL_BIN/cl.exe" ]; then
  echo "ERROR: cl.exe not found at $CL_BIN" >&2
  echo "Edit MSVC_WIN / KIT_WIN / CL_BIN in this script." >&2
  exit 1
fi

export MSYS2_ARG_CONV_EXCL='*'
# cppwinrt 目录是 C++/WinRT 投影头文件所在处，读真实媒体/通知时必须加。
export INCLUDE="$MSVC_WIN\\include;$KIT_WIN\\Include\\$SDK_VER\\ucrt;$KIT_WIN\\Include\\$SDK_VER\\shared;$KIT_WIN\\Include\\$SDK_VER\\um;$KIT_WIN\\Include\\$SDK_VER\\winrt;$KIT_WIN\\Include\\$SDK_VER\\cppwinrt"
export LIB="$MSVC_WIN\\lib\\x64;$KIT_WIN\\Lib\\$SDK_VER\\ucrt\\x64;$KIT_WIN\\Lib\\$SDK_VER\\um\\x64"
export PATH="$CL_BIN:$PATH"

mkdir -p build/cpp
# -DNOMINMAX：windows.h 的 min/max 宏会破坏 core 里的 std::min/std::max。
# -std:c++20：C++/WinRT 在 C++17 下走 experimental/coroutine，该头已被 MSVC 14.51 弃用并静态断言失败。
#   所以凡是引用 live_sources.hpp 的目标必须用 C++20；核心域本身仍是 C++17 兼容语法。
FLAGS=(-nologo -std:c++20 -utf-8 -EHsc -W4 -DNOMINMAX -DUNICODE -D_UNICODE -Icontracts -Isrc/core)
# windowsapp.lib：WinRT 激活入口；ole32.lib：Core Audio（读系统音量）。
GUI_LIBS=(user32.lib gdi32.lib gdiplus.lib shlwapi.lib shell32.lib ole32.lib windowsapp.lib winhttp.lib)

echo "=== compile contract_smoke ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/contract_smoke.obj" \
  -Fe"build/cpp/contract_smoke.exe" tests/cpp/contract_smoke.cpp

echo "=== compile core_tests ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/core_tests.obj" \
  -Fe"build/cpp/core_tests.exe" tests/cpp/core_tests.cpp

echo "=== compile rules_tests (规则引擎 + 映射，纯 C++ 无 Win32) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/rules_tests.obj" \
  -Fe"build/cpp/rules_tests.exe" tests/cpp/rules_tests.cpp

echo "=== compile render_tests (需要 Windows 头文件，但不创建窗口；玻璃用例要真实 GDI+ 合成) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/render_tests.obj" \
  -Fe"build/cpp/render_tests.exe" tests/cpp/render_tests.cpp \
  /link gdiplus.lib user32.lib gdi32.lib

echo "=== compile interaction_tests (歌词解析 / 时间轴 / 命中几何) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/" -Fe"build/cpp/interaction_tests.exe" \
  tests/cpp/interaction_tests.cpp \
  /link /SUBSYSTEM:CONSOLE gdiplus.lib user32.lib gdi32.lib winhttp.lib

echo "=== compile agent_tests (会话文件解析：Claude/WorkBuddy/Codex/Gemini) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/" -Fe"build/cpp/agent_tests.exe" \
  tests/cpp/agent_tests.cpp /link /SUBSYSTEM:CONSOLE

echo "=== compile agent probe (真实会话目录 -> 解析结果) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/" -Fe"build/cpp/agent_probe.exe" \
  probes/agent_probe.cpp /link /SUBSYSTEM:CONSOLE shell32.lib ole32.lib

echo "=== compile editor_tests (窗口标题 -> 文件/项目/品牌) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/" -Fe"build/cpp/editor_tests.exe" \
  tests/cpp/editor_tests.cpp /link /SUBSYSTEM:CONSOLE

echo "=== compile browser_tests (URL/站名/标题/媒体归属 判定) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/" -Fe"build/cpp/browser_tests.exe" \
  tests/cpp/browser_tests.cpp /link /SUBSYSTEM:CONSOLE

echo "=== compile browser probe (本机真实浏览器 -> 采集结果) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/" -Fe"build/cpp/browser_probe.exe" \
  probes/browser_probe.cpp /link /SUBSYSTEM:CONSOLE user32.lib shell32.lib ole32.lib

echo "=== compile balance_island (桌面胶囊，GUI 子系统) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/" -Fe"build/cpp/balance_island.exe" \
  src/win32/overlay_main.cpp \
  /link /SUBSYSTEM:WINDOWS "${GUI_LIBS[@]}"

echo "=== compile live probe (读真实媒体会话的独立探针) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/" -Fe"build/cpp/smtc_probe.exe" \
  probes/smtc_probe.cpp /link /SUBSYSTEM:CONSOLE windowsapp.lib gdiplus.lib shlwapi.lib ole32.lib

echo "=== compile settings probe (验证设置窗口→配置文件链路) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/" -Fe"build/cpp/settings_probe.exe" \
  probes/settings_save_probe.cpp /link /SUBSYSTEM:CONSOLE shell32.lib user32.lib ole32.lib

echo "=== compile toast probe (发真实系统通知，验证实时到达) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/" -Fe"build/cpp/toast_probe.exe" \
  probes/toast_probe.cpp /link /SUBSYSTEM:CONSOLE windowsapp.lib

echo "=== compile cover probe (逐段定位封面链路) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/" -Fe"build/cpp/cover_probe.exe" \
  probes/cover_probe.cpp /link /SUBSYSTEM:CONSOLE windowsapp.lib

echo "=== compile media inject probe (带封面的真实 SMTC 会话) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/" -Fe"build/cpp/media_inject_probe.exe" \
  probes/media_inject_probe.cpp /link /SUBSYSTEM:CONSOLE windowsapp.lib

echo "=== compile png probe (读像素，判断渐变色带是否真实) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/" -Fe"build/cpp/png_probe.exe" \
  probes/png_probe.cpp /link /SUBSYSTEM:CONSOLE gdiplus.lib

echo "=== compile backdrop probe (单独验证截屏+模糊管线) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/" -Fe"build/cpp/backdrop_probe.exe" \
  probes/backdrop_probe.cpp /link /SUBSYSTEM:CONSOLE gdiplus.lib user32.lib gdi32.lib

echo "=== compile white background probe (玻璃效果的最苛刻背景) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/" -Fe"build/cpp/white_bg_probe.exe" \
  probes/white_bg_probe.cpp /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib

echo "=== compile notif diag probe (通知事件订阅 + 应用图标逐段诊断) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/" -Fe"build/cpp/notif_diag_probe.exe" \
  probes/notif_diag_probe.cpp /link /SUBSYSTEM:CONSOLE windowsapp.lib

echo "=== compile app icon probe (AUMID→开始菜单→exe 图标链路) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/" -Fe"build/cpp/appicon_probe.exe" \
  probes/appicon_probe.cpp /link /SUBSYSTEM:CONSOLE "${GUI_LIBS[@]}"

echo "=== compile lyrics probe (WinHTTP -> LRCLIB -> LRC 解析) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/" -Fe"build/cpp/lyrics_probe.exe" \
  probes/lyrics_probe.cpp /link /SUBSYSTEM:CONSOLE winhttp.lib

echo "=== compile hanzi probe (繁 -> 简，LCMapStringEx) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/" -Fe"build/cpp/hanzi_probe.exe" \
  probes/hanzi_probe.cpp /link /SUBSYSTEM:CONSOLE "${GUI_LIBS[@]}"

echo "=== compile cache LRU probe (TrimCache 淘汰最旧文件) ==="
cl.exe "${FLAGS[@]}" -Fo"build/cpp/" -Fe"build/cpp/cache_probe.exe" \
  probes/cache_probe.cpp /link /SUBSYSTEM:CONSOLE "${GUI_LIBS[@]}"

echo "=== run ==="
./build/cpp/contract_smoke.exe
./build/cpp/core_tests.exe
./build/cpp/rules_tests.exe
./build/cpp/render_tests.exe
./build/cpp/interaction_tests.exe
./build/cpp/agent_tests.exe
./build/cpp/browser_tests.exe
./build/cpp/editor_tests.exe
./build/cpp/hanzi_probe.exe
./build/cpp/cache_probe.exe
echo "C++ BUILD+TEST: PASS"
echo "可执行程序: build/cpp/balance_island.exe"
