cmake_minimum_required(VERSION 3.20)
project(BalanceIsland LANGUAGES CXX)

# C++/WinRT 在 C++17 下依赖 experimental/coroutine，该头已被 MSVC 14.51 弃用。
# 读真实媒体/通知必须用 C++20；核心域本身仍是 C++17 兼容语法，不受影响。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC：源码为 UTF-8（含中文注释），启用 /W4。
# NOMINMAX 必须定义：windows.h 的 min/max 宏会破坏核心域里的 std::min / std::max（C2589）。
if(MSVC)
  add_compile_options(/utf-8 /W4 /EHsc /DNOMINMAX /DUNICODE /D_UNICODE)
endif()

enable_testing()

add_executable(contract_smoke tests/cpp/contract_smoke.cpp)
target_include_directories(contract_smoke PRIVATE contracts)
add_test(NAME contract_smoke COMMAND contract_smoke)

add_executable(core_tests tests/cpp/core_tests.cpp)
target_include_directories(core_tests PRIVATE contracts src/core)
add_test(NAME core_tests COMMAND core_tests)

# 渲染层可测部分：引入 Windows 头文件但不创建窗口、不初始化 GDI+。
# 规则引擎 + 实时活动映射：纯 C++（不碰 Win32），可直接在 CI 的主机上跑。
add_executable(rules_tests tests/cpp/rules_tests.cpp)
target_include_directories(rules_tests PRIVATE contracts src/core)
add_test(NAME rules_tests COMMAND rules_tests)

add_executable(render_tests tests/cpp/render_tests.cpp)
target_include_directories(render_tests PRIVATE contracts src/core src/win32)
if(MSVC)
  target_link_libraries(render_tests PRIVATE gdiplus user32 gdi32)
endif()
add_test(NAME render_tests COMMAND render_tests)

# 桌面胶囊可执行程序（GUI 子系统）。
# 注意：这是「独立进程 + 分层窗口」形态，不注入 explorer.exe，也不依赖 Windhawk。
add_executable(balance_island WIN32 src/win32/overlay_main.cpp)
target_include_directories(balance_island PRIVATE contracts src/core src/win32)
if(MSVC)
  # windowsapp.lib：WinRT 激活（SMTC / UserNotificationListener）
  # ole32：Core Audio，用于读系统音量
  target_link_libraries(balance_island PRIVATE user32 gdi32 gdiplus shell32 ole32 windowsapp)
endif()
