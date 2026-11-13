# 编译器与环境（2026-11-12 实测）

## 结论：本机无需安装任何编译器

用户机器已装 **Visual Studio Community 2026**，工具链完整。此前「无 C++ 编译器」的判断有误，
原因是只检查了 `PATH` 与 C 盘默认安装路径，漏掉了自定义安装位置。

## 实测清单

| 组件 | 路径 / 版本 |
|------|-------------|
| VS 产品 | Visual Studio Community 2026，18.7.3（Dev18） |
| VS 安装目录 | `D:\code\vs\vs`（由 `vswhere.exe -all -products *` 查得） |
| MSVC 工具集 | `14.51.36231` |
| `cl.exe` | `D:\code\vs\vs\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\cl.exe`（另有 Hostx86/x86 等 4 组） |
| 环境初始化脚本 | `D:\code\vs\vs\VC\Auxiliary\Build\vcvars64.bat` / `vcvarsall.bat` |
| Windows SDK | `10.0.26100.0`，位于 `D:\Windows Kits\10`（D 盘，非 C 盘默认位置） |
| CMake | `D:\code\vs\vs\Common7\IDE\CommonExtensions\Microsoft/CMake/CMake/bin/cmake.exe` |
| Ninja | `D:\code\vs\vs\Common7\IDE\CommonExtensions\Microsoft/CMake/Ninja/ninja.exe` |

## 三种启用方式

**方式 A（本项目推荐，已验证）**：直接用一键脚本，无需手工配置环境变量。

```bash
cd DynamicIsland-Win
bash tools/build_cpp_msvc.sh
```

脚本内部设置 `INCLUDE` / `LIB` / `PATH` 后调用 `cl.exe`。
换机器时改脚本顶部的 `MSVC_WIN` / `KIT_WIN` / `CL_BIN` / `SDK_VER`。

**方式 B（VS 官方）**：从开始菜单打开「x64 Native Tools Command Prompt for VS 2026」，
`cl.exe` 与 `INCLUDE`/`LIB` 已自动配置好；或在普通终端执行：

```
"D:\code\vs\vs\VC\Auxiliary\Build\vcvars64.bat"
```

注意 `.bat` 需在 `cmd.exe` 环境执行，Git Bash 不能直接 `source`。

**方式 C（CMake，推荐用于 IDE / CI）**：在 VS 开发者命令行或 CI 的 `windows-latest` 环境：

```
cmake -S . -B build/cmake -G "Visual Studio 18 2026" -A x64
cmake --build build/cmake --config Release
ctest --test-dir build/cmake -C Release --output-on-failure
```

## 本机 CMake 限制（如实记录）

在 Git Bash 沙箱内直接调用 VS 自带 CMake 时，编译器识别失败：

- `-G "Visual Studio 18 2026"`：`The CXX compiler identification is unknown` /
  `No CMAKE_CXX_COMPILER could be found`
- `-G Ninja -DCMAKE_CXX_COMPILER=...`：编译器试用构建失败

根因是 CMake 的 MSVC 检测依赖 VS 安装注册与 MSBuild 集成，Git Bash 环境未完整提供；
**不是源码或 CMakeLists.txt 的问题**。原生 `cl.exe` 直接编译两个目标均成功，
因此 CI 使用 `windows-latest`（自带完整 MSVC 环境）执行 CMake 路径，本地用方式 A 验证。

另需注意：CTest 在「配置未完成、No tests were found」时返回退出码 0，
这是**假阳性**，不能作为构建成功的证据；必须同时核对 CMake 配置与构建的退出码。

## 编译选项约定

| 选项 | 用途 |
|------|------|
| `/std:c++17` | 项目标准 |
| `/utf-8` | 源码含中文注释，禁用系统 936 代码页解释（消除 C4819 警告） |
| `/W4 /EHsc` | 高警告级别 + C++ 异常 |
| `-I contracts -I src/core` | 模块包含路径；**不使用 `../../` 相对路径** |

## 头文件约定

所有头文件使用传统 include guard（`#ifndef ... #define ... #endif`），不用 `#pragma once`。
原因：同一头文件经由 `Types.hpp` 与 `../../contracts/Types.hpp` 两条路径包含时，
MSVC 会视为不同文件导致类型重复定义（2026-11-12 首次真实编译暴露并已修复）。
