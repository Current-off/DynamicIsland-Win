# 验证记录（2026-11-12）

本文件如实记录本轮实际执行的命令、环境、结果、发现的缺陷与剩余缺口。
遵循项目铁律：**自动化通过 ≠ 实机视觉验收**，不得以测试通过概括功能已完成。

## 环境

| 项 | 实测值 |
|----|--------|
| OS | Windows（win32），Git Bash |
| Python | 3.13.14（`C:\Users\ow1\.workbuddy\binaries\python\versions\3.13.12\python`） |
| git | 2.55.0.windows.3 |
| 开源编译器 `g++` / `clang++` / `gcc` / `zig` / `cmake`(PATH) | MISSING |
| **VS Community 2026** | **已安装，`D:\code\vs\vs`**（`vswhere.exe -all` 查得） |
| **MSVC 工具集** | **14.51.36231，`cl.exe` 存在（Hostx64/x64 等 4 组）** |
| **Windows SDK** | **10.0.26100.0，`D:\Windows Kits\10`（D 盘）** |
| VS 自带 CMake / Ninja | 存在于 `D:\code\vs\vs\Common7\IDE\CommonExtensions\Microsoft\CMake\` |

**环境结论更正**：本文初版记录「本机无 C++ 编译器」，该判断有误——只检查了 `PATH`
与 C 盘默认安装路径，漏掉自定义安装位置。用户指出后经 `vswhere` 复核，
确认 MSVC + SDK + CMake + Ninja 齐全，**无需安装任何东西**。详见 `docs/toolchain.md`。

因此第二轮把 C++ 从「仅接口契约」推进为**真实编译、真实运行的产品实现**。

## 命令与结果

### 1. 一键验证（首次）

```
cd DynamicIsland-Win && python tools/verify.py
```

结果：`RESULT: FAIL`，退出码 1。
`Ran 52 tests ... FAILED (errors=2)`；契约校验 `7 passed, 0 failed`。

**发现的真实缺陷**（未掩盖、未放宽验收）：
`providers/builtin.py` 中 `MediaProvider` / `SystemStatusProvider` 用 `@dataclass` 继承
非 dataclass 的 `Provider` 基类，自动生成的 `__init__` 覆盖了基类 `__init__`，
导致 `self.enabled` 未初始化，`poll()` 抛 `AttributeError`。

**修复**：把 `Provider` 基类改为 `@dataclass` 并声明 `enabled` / `sensitive` 字段，
在 `__post_init__` 保留「敏感 Provider 必须默认关闭」校验。
改动文件：`providers/base.py`（重写）、`providers/builtin.py`（字段标注调整）。

### 2. 一键验证（修复后）

```
cd DynamicIsland-Win && python tools/verify.py
```

结果：`RESULT: PASS`，退出码 0。

- 单元测试：`Ran 52 tests in 0.002s — OK`，failures=0、errors=0。
- 跨语言契约校验：`7 passed, 0 failed`
  （`kProtocolVersion=1`、`kMaxVisibleActivities=2`、`kAnimationMaxMs=2000`、
  `kCornerRadius=44.0`、`kConcentricMargin=20.0`、
  `kExpandedMinHeight=84.0`、`kExpandedMaxHeight=160.0`）。

### 3. 行为演示

```
cd DynamicIsland-Win && python demo.py
```

结果：退出码 0。可观察链路：
`idle → compact`（媒体开始）→ `minimal`（日历高优先级加入）→ 第三活动被抑制
→ 桥接离线后内置 Provider 仍显示 → 全屏避让隐藏且状态保留
→ 到达 `staleDate` 自动标记过期并触发 `activity.expired` 事件 → 重调度。

## 测试覆盖清单

| 模块 | 用例数 | 覆盖内容 |
|------|--------|----------|
| `test_activity.py` | 8 | 过期边界（含等号）、永不过期、终止态、不可变性、非法 id、时间序列、IPC 字段名 |
| `test_store_scheduler.py` | 13 | 幂等 upsert、终止态不可复活、过期只标记一次、确定性排序、同分决胜、抑制计数、扩展态 |
| `test_state_machine.py` | 7 | 合法路径、非法转移报错且状态不变、历史可追溯、reset |
| `test_eventbus_protocol.py` | 12 | 订阅顺序、故障隔离、重复订阅、协议往返、缺字段/畸形/类型范围反例、未知字段兼容、版本协商矩阵 |
| `test_tokens_runtime.py` | 12 | 同心圆角、≤2s 裁剪、Reduce Motion、高度边界、敏感默认关闭、变更去重、桥接离线降级、全屏避让、过期事件、生命周期状态机 |

合计 52 项（Python 可执行规约）。

---

## 第二轮：C++ 真实编译（2026-11-12 15:20 后）

### 4. 首次 C++ 编译（暴露契约缺陷）

```
cl.exe -nologo -std:c++17 -EHsc -W4 tests/cpp/contract_smoke.cpp
```

结果：`CL_EXIT=2`，**失败**。关键错误：

- `Types.hpp(12..55): error C2011 / C2374 / C2086` —— enum 与 struct 重复定义。
  根因：`contract_smoke.cpp` 用 `#include "../../contracts/Types.hpp"`，
  而 `IOverlayRenderer.hpp` 内部用 `#include "Types.hpp"`，
  两条不同路径使 MSVC 的 `#pragma once` 视为不同文件，头文件被包含两次。
- 连带引发 `std::optional<diw::Activity>` 实例化失败（`C2139` / `C2079`）。
- `warning C4819`：源码含中文注释，MSVC 按 936 代码页解释。

**修复**（未放宽验收、未跳过失败分支）：
1. 三个 `contracts/*.hpp` 全部改用传统 include guard
   （`#ifndef DIW_CONTRACTS_..._HPP`），不再依赖 `#pragma once`。
2. 冒烟测试改用 `#include "Types.hpp"` + 编译参数 `-I contracts`，
   不再使用 `../../` 相对路径。
3. 编译选项加 `/utf-8` 消除编码警告。

### 5. 修复后 C++ 编译与运行

```
cl.exe -nologo -std:c++17 -utf-8 -EHsc -W4 -Icontracts -Isrc/core \
  -Fobuild/cpp/contract_smoke.obj -Febuild/cpp/contract_smoke.exe \
  tests/cpp/contract_smoke.cpp
```

结果：`CL_EXIT=0`，运行 `RUN_EXIT=0`，`/W4` 无警告。
接口契约（含 `std::optional<Activity>`、纯虚接口、常量 `static_assert`）编译与链接通过。

```
cl.exe ... tests/cpp/core_tests.cpp   →  CL_EXIT=0
./tests/cpp/core_tests.exe            →  run=69 failed=0，RUN_EXIT=0
```

C++ 核心域覆盖：过期边界、Store 幂等与终止态保护、确定性排序与同分决胜、
三态分配、抑制计数、状态机合法/非法转移、设计 token 裁剪、
桥接离线降级、全屏避让状态保留、过期重调度。

### 6. 一键脚本

```
bash tools/build_cpp_msvc.sh
```

结果：`SCRIPT_EXIT=0`，输出 `C++ BUILD+TEST: PASS`。
脚本自动设置 `INCLUDE` / `LIB` / `PATH` 并编译运行两个目标。

### 7. CMake 路径（本机受限，如实记录）

| 尝试 | 结果 |
|------|------|
| `-G "Visual Studio 18 2026" -A x64`（Git Bash） | `No CMAKE_CXX_COMPILER could be found`，`CFG=1` |
| `-G Ninja -DCMAKE_CXX_COMPILER=<unix 路径>` | 编译器路径不被识别，`CFG=1` |
| `-G Ninja -DCMAKE_CXX_COMPILER=<Windows 路径>` | 编译器试用构建失败，`CFG=1` |

根因：CMake 的 MSVC 检测依赖 VS 注册信息与 MSBuild 集成，Git Bash 沙箱未完整提供；
**非源码问题**。`CMakeLists.txt` 保留供 VS 开发者命令行与 CI（`windows-latest`）使用，
本机已由原生 `cl.exe` 路径完成等价验证。

**可靠性提示**：CTest 在「配置未完成 / No tests were found」时返回退出码 0，
属于**假阳性**，不得作为构建成功证据；必须同时核对 configure 与 build 的退出码。

## 第三轮：桌面上屏渲染层（2026-11-12 15:43 后）

目标：把「核心域能跑测试」推进到「桌面上真的能看见胶囊」。

### 1. 新增实现

| 文件 | 作用 |
|------|------|
| `src/win32/layered_surface.hpp` | 32bpp DIB + `UpdateLayeredWindow`，含每帧 alpha 预乘、PNG 落盘、屏幕 BitBlt 抓取 |
| `src/win32/renderer.hpp` | 胶囊绘制：圆角矩形、投影、玻璃内高光、强调色图标块、律动条、进度条、三态布局 |
| `src/win32/overlay_main.cpp` | 窗口与消息循环、60fps 节奏、剧本投放、点击展开/收起、日志与快照开关 |
| `tests/cpp/render_tests.cpp` | 20 项断言：进度解析（含越界/畸形）、布局常量跨层契约、UTF-8 转换 |
| `tools/verify_visual.sh` | 一键生成三态截图 + 桌面实证 |

### 2. 编译过程中真实暴露并修复的问题

| 问题 | 现象 | 根因 | 修法 |
|------|------|------|------|
| C2589 ×14 | `std::min` / `std::max` 右侧非法记号 | `windows.h` 的 `min`/`max` 宏 | 全部 TU 在 `windows.h` 前定义 `NOMINMAX`，构建脚本加 `-DNOMINMAX` |
| GdiplusImaging 一片语法错 | `IStream` / `PROPID` 未定义 | `WIN32_LEAN_AND_MEAN` 下 `objidl.h` 未被引入 | `#include <objidl.h>` 置于 `gdiplus.h` 之前 |
| C4457 | 局部变量遮蔽参数 | HUD 变量名与形参同名 | 形参改 `with_hud`，局部改 `hud_text` |
| LNK4044 | `/SUBSYSTEM=WINDOWS` 未生效 | 参数用了 `=` 而非 `:` | 改为 `/SUBSYSTEM:WINDOWS`，重新链接确认无警告 |

最终 `cl /std:c++17 /utf-8 /EHsc /W4 /DNOMINMAX` **零警告**，四个目标全部链接成功。

### 3. 截图暴露的视觉缺陷（静态检查抓不到）

| 缺陷 | 表现 | 根因 | 修法 |
|------|------|------|------|
| 胶囊中部横向硬边 | 三态截图里都有一条横线贯穿胶囊 | 内高光被裁到 55% 高度，渐变在终点处收边 | 渐变覆盖整条胶囊并复用同一 capsule 路径 |
| 状态行被截断 | 底部显示 `expired...` | HUD 文案比胶囊宽，`DrawString` 用省略号裁剪 | 文案压短为 `Compact · vis 1 · sup 0 · exp 0` |
| 右侧时间贴边 | `1:42 / 4:29` 压到描边上 | 文本测量值含内边距，预留只有 14px | 右留白提到 16px，并按测量宽直接定位 |
| 展开态仍并列小胶囊 | 展开时右侧还挂一个圆 | 分离小胶囊的条件未排除 `expanded` | 展开时不显示分离小胶囊 |

### 4. 视觉证据

`bash tools/verify_visual.sh` 退出码 0，产物：

| 文件 | 内容 | 观察结论 |
|------|------|----------|
| `build/snap_compact.png` | 紧凑态 | 音符块 + 歌名 + 律动条 + 时间，无硬边 |
| `build/snap_minimal.png` | 极简态 | 日历抢占主位，媒体降为右侧分离小圆 |
| `build/snap_expanded.png` | 展开态 | 标题 / 时间地点 / 底部操作提示，无重叠 |
| `build/desktop_proof.png` | 桌面实证 | 从屏幕 DC `BitBlt` 抓回，胶囊确实叠在真实桌面之上 |

`desktop_proof.png` 抓的是显示器合成结果（能看到背后的应用界面），不是自身绘制缓冲，
因此可以作为「真的上屏了」的证据；但它**证明不了**主观观感与帧率。

### 5. 本轮新增断言

```
core_tests    run=69 failed=0
render_tests  run=20 failed=0
contract_smoke 退出码 0
```

## 第四轮：从剧本假数据改为接入真实系统（2026-11-12 15:53 后）

用户指出「这只是一个静态设置好的灵动岛，我要像 iOS 那样让日常的歌、日常的消息融入进来」。
这是产品定位问题，不是渲染问题。本轮把数据源从内置剧本换成真实系统。

### 1. 先验证可行性，再写产品代码

没有直接假设 API 可用，而是先写了三个探针（`probes/`）：

| 探针 | 问题 | 实测结果 |
|------|------|----------|
| `smtc_probe.cpp` | 未打包应用能否读系统媒体会话？ | **能**。`RequestAsync()` 拿到 manager，`GetSessions()` 可枚举，元数据与时间轴可读 |
| `notif_probe.cpp` | `UserNotificationListener` 在无 MSIX 身份时是否被拒？ | **未被拒**。`GetAccessStatus()` = `Allowed`，`GetNotificationsAsync()` 读到 **16 条真实 toast** |
| `media_inject_probe.cpp` | 媒体链路端到端是否通？ | **通**。用静音 WAV 建立真实 SMTC 会话，另一个进程读到完整元数据 |

`notif_probe` 的结论修正了我开工前的预判（原以为未打包必被拒、需要 MSIX 打包）。
这条如果不实测就会走错架构：会去写一套不必要的打包流程。

### 2. 新增实现

| 文件 | 作用 |
|------|------|
| `src/win32/live_sources.hpp` | 采集层：SMTC 媒体、`UserNotificationListener` 通知、电源、Core Audio 音量、剪贴板序列号、下载目录 mtime、全屏状态。独立工作线程 + 快照 + 变化去重 |
| `src/win32/overlay_main.cpp`（改） | 默认切真实模式，`--script` 才走剧本；新增活动同步与优先级映射 |
| `probes/*.cpp` | 三个可行性探针，保留在仓库内作为回归依据 |

### 3. 真实暴露并修复的问题

| 问题 | 现象 | 根因 | 修法 |
|------|------|------|------|
| C2338 静态断言 | 编译直接失败，报 `experimental/coroutine` 已弃用 | C++/WinRT 在 C++17 下走 `experimental/coroutine`，MSVC 14.51 已移除支持 | 全项目升到 `/std:c++20`（核心域语法本身仍 C++17 兼容） |
| C2039 `UserNotification` | 类型找不到 | 该类型在 `Windows::UI::Notifications`，不在 `Management` 子命名空间 | 修正命名空间 |
| C3779 返回类型不完整 | `AppInfo().DisplayInfo()` 无法推导 | 缺 `Windows.ApplicationModel.h` | 补 include |
| C2039 `CreateFromStorageFile` | `MediaPlaybackItem` 无此静态方法 | 该类型只有构造函数 | 改为 `MediaPlaybackItem(MediaSource::CreateFromStorageFile(f))` |
| 探针 SMTC 元数据为空 | 会话能读到，但 title/artist 为 `""` | 用 `player.SystemMediaTransportControls().DisplayUpdater()` 设置，被 WAV 空内嵌元数据覆盖 | 改为 `MediaPlaybackItem::GetDisplayProperties()` + `ApplyDisplayProperties()` |
| **`Upsert` 整条覆盖** | 若每帧刷新带 `stale_at` 的活动，通知将**永不退场** | `ActivityStore::Upsert` 是 `items_[id] = activity`，会把过期时间一起刷新 | 瞬时活动只在「当前不存在」时插入：`Store().Get(id) == nullptr` 才 `StartActivity` |
| `BuildView` 硬编码文案 | 塞进真实活动，屏幕仍显示「周杰伦 · 叶惠美 / 1:42 / 4:29」 | 副标题与 meta 是写死的字符串 | 改为从 `content_json` 解析（含 JSON 转义还原） |

`Upsert` 与硬编码这两条是**静态检查和编译都发现不了的**——前者是语义陷阱，后者是数据流断裂，
必须靠真实数据跑起来才能看见。

### 4. 端到端证据（本轮实跑）

真实剪贴板 + 真实下载文件：
```
live media=none (paused) vol=76% batt=100% ... clip_seq=90
[t=+0.06s] +clipboard 「已复制」          ← clip_seq 0→90 正是写入剪贴板的时刻
[t=+3.07s] state=Idle ... expired=1        ← 3 秒后自动退场
[t=+9.74s] +download 「项目周报_1112.xlsx」 ← 真实创建的文件名
```
截图 `build/proof_live.png`：胶囊显示「项目周报_1112.xlsx」，背景是真实桌面。

真实媒体会话（静音 WAV 走标准 SMTC 通道）：
```
live media=SMTC 链路验证 (playing) vol=76% batt=100%
[t=+0.02s] state=Compact visible=1
```
截图 `build/proof_media.png`：胶囊显示「SMTC 链路验证」与 **0:18 / 2:00**。
对比剧本模式的写死值 `1:42 / 4:29` —— 这个进度是随播放真实走动的。

首次启动时 `notif_seq=0`，说明系统里已有的 16 条历史通知**没有**被当成新消息刷出来，
基线比对逻辑生效。

### 5. 构建验证

```
bash tools/build_cpp_msvc.sh
→ contract_smoke / core_tests / render_tests / diw_overlay / smtc_probe 五个目标编译成功
→ core_tests run=69 failed=0
→ render_tests run=20 failed=0
→ /W4 零警告
```

## 明确未验证的部分（不可声称完成）

1. Windhawk mod 源码、命名管道实体、桥接进程、安装器 **均未实现**。
   （设置面板已于第五轮实现，见文末。）
2. 真实 Provider 已接入媒体/通知/电源/音量/剪贴板/下载/全屏，但仍有边界：
   - 媒体封面（专辑图）**未解码**，胶囊用矢量音符块占位
   - 通知只取标题与正文两段文本，未做图标与应用色提取
   - 通知的「新消息」判定依赖 `UserNotification` 的 id 集合差集，微信等应用撤回/更新同一条通知时可能不触发
   - 只在单机单用户下测过；多用户快速切换、通知服务重启未验证
3. 材质是「半透明 + 顶高光」模拟，不是 DWM 亚克力/云母；系统关闭透明效果时不会跟随。
4. 全屏避让已接 `SHQueryUserNotificationState`（第五轮），但**反作弊场景未验证**：
   不进 explorer.exe 就不会被反作弊拦，这是设计取舍而非已验证结论。
5. 多显示器：只取主显示器，运行中切换 DPI 不重新布局。
6. 未进行性能基线测量（常驻内存、空闲 CPU、动画帧间隔 P95）；长时间运行未做泄漏观察。
7. CMake 链路在 Git Bash 下未跑通，仅 CI 配置就绪、尚未在 `windows-latest` 实际执行。
8. C++ 侧尚未实现 IPC 协议编解码与 Provider 层（目前仅 Python 规约）。

## 下一步可直接接续

1. 把 `bridge/protocol.py` 的协议语义迁移为 C++ 实现，补畸形输入与版本协商测试。
2. 实现命名管道服务端/客户端与 `hello` / `heartbeat` 消息，补重连退避测试。
3. 接真实 Provider：媒体会话（含无媒体控件的音频回退）→ 通知 / 电池 → 下载 / 剪贴板。
4. 加性能埋点：帧间隔 P95、常驻内存、空闲 CPU，达标后再谈「高性能」。
5. 视需要再评估 Windhawk mod 形态（能拿任务栏数据，但要进 `explorer.exe`）。
6. 在 GitHub Actions `windows-latest` 上实际跑一次 `cpp-build` 作业，验证 CMake 链路。

---

# 第五轮（2026-11-12）：识别领域扩展 + 用户可配置规则

这一轮回答用户的原话：「复制了也可以识别，就是用户有操作，灵动岛有反应，
同时用户也可以自行设置哪些操作不需要反应、哪些应用不需要反应、哪些类型文件不需要反应。」

## 1. 做了什么

| 能力 | 实现位置 | 判定依据 |
|------|----------|----------|
| 复制识别（文本 / 图片 / 截图 / 文件列表） | `live_sources.hpp::PollClipboard` | 序列号变化后依次尝试 `CF_HDROP` → `CF_DIB` → `CF_UNICODETEXT` |
| 复制来源应用 | 同上（`ForegroundAppOf`） | `GetForegroundWindow` + `QueryFullProcessImageNameW`；Ctrl+C 不改焦点，此刻前台就是复制方 |
| 截图与图片区分 | 同上 | 剪贴板是 DIB 时按来源进程名判定（snipping / screenclip / snipaste…） |
| 文件落盘带扩展名 | `live_sources.hpp::PollFile` | 下载目录最新 mtime + `ExtensionOf` |
| 电源插拔 | `live_mapping.hpp` | `ac_online` 翻转（首次只建基线） |
| U 盘接入 | `WM_DEVICECHANGE` + `DEV_BROADCAST_VOLUME` | `dbcv_unitmask` 直接给出盘符 |
| 用户规则 | `src/core/rules.hpp` | 事件开关 → 免打扰 → 应用白/黑名单 → 扩展名白/黑名单 |
| 配置热加载 | `src/win32/rules_io.hpp` | 500ms 查 mtime；解析失败保留上一份可用配置 |
| 设置界面 | `src/win32/settings_ui.hpp` | 原生 Win32 控件，保存后写回配置文件 |

## 2. 单元测试

```
rules_tests:  run=137 failed=0
core_tests:   run=69  failed=0
render_tests: run=20  failed=0
```

新增 137 项断言覆盖：JSON 解析/转义/代理对、配置读写往返、坏配置回落、事件开关、
应用子串匹配（忽略大小写）、白名单排他、扩展名精确匹配（`png` 不误伤 `apng`）、
跨零点免打扰、敏感内容隐藏、同一次复制只产出一条活动、静音也提示、
电源插拔只在翻转时提示、全屏避让开关。

## 3. 实机验证（真实剪贴板 / 真实文件 / 真实配置改动）

复制一段中文 → 胶囊出现：

```
[t=+1.332s] +clipboard 「已复制」
[t=+1.33s]  state=Compact visible=1 blocked=0
[t=+4.55s]  state=Idle ... expired=1        ← 3.2 秒后自动退场
```

截图 `build/proof_clip_expanded.png`：展开态标题「已复制」，副标题是复制全文，
左下角是来源应用名，右下角是操作提示。

黑名单生效（`apps.block = ["workbuddy"]`，从该应用复制）：

```
[t=+1.684s] blocked: clipboard/app-blocked(workbuddy)
[t=+1.68s]  state=Idle visible=0 blocked=1   ← 没有 +clipboard，胶囊保持隐藏
```

扩展名黑名单生效（在下载目录落一个 `.tmp`）：

```
[t=+1.453s] blocked: file/ext-blocked(tmp)
```

换成 `.txt` 则正常提示 `[t=+1.328s] +file 「diw_probe_报告.txt」`（截图 `build/proof_file.png`）。

配置热加载（运行中把 `apps.block` 从 `["workbuddy"]` 改回 `[]` 后再复制）：

```
rules reloaded: ok
[t=+3.196s] +clipboard 「已复制」      ← 立刻恢复响应，没有重启进程
```

设置窗口截图 `build/settings.png`（200% DPI，逐张核对控件边界）。

设置窗口 → 配置文件这条链路单独验证过（`--selftest`，进程内填控件再走收集逻辑）：

```
$ build/cpp/diw_overlay.exe --selftest
selftest: app_block=2 first=wechat volume=0 quiet=1 22:30
selftest: wrote ...\build\cpp\settings_selftest.json
```

落盘结果核对：`"block": ["wechat", "迅雷"]`、`"volume": false`、
`"enabled": true`、`"from": "22:30"`、`"to": "07:15"` —— 中文与时间都正确。

> 为什么要有 `--selftest`：外部探针（`probes/settings_save_probe.cpp`）能验证
> 「点保存 → 勾选框落盘」，但**改不动编辑框** —— `SetWindowText` 跨进程对子控件无效
> （user32 不会替你编组 WM_SETTEXT 里的指针），读回来长度恒为 0。
> 这是探针的边界，不是产品的缺陷，所以文本框这条路必须在进程内走一遍。

## 4. 这一轮抓到的真实缺陷

1. **启动后的第一次复制被吞掉**：原先用 `clip_known` 门闩防止「启动时把旧剪贴板当新复制」，
   但采集层启动时已用 `clip_seq_seen_` 对齐过基线，门闩只会吃掉第一次真实复制。
   实测日志里 `clip=text/117` 却没有 `+clipboard` 才暴露。同样的门闩在通知和 U 盘事件上
   也有，一并移除；`power` 是状态型事件（每帧上报当前值），保留门闩。
2. **启动瞬间假报「音量 76%」**：音频端点就绪前 `volume_pct` 是 0，被当成基线，
   第一帧就成了「0 → 76」的变化。加 `volume_known`，未就绪不参与检测。
3. **设置窗口在 200% DPI 下把「保存」按钮裁掉**：控件按 96 DPI 基准确排，固定客户区高度不够。
   改为按最后一个控件的下沿自适应高度，并把窗口夹回工作区。
4. **设置窗口截图底部缺一条**：`PrintWindow` 从窗口左上角开始画，位图却按客户区高度分配，
   正好少一个标题栏。改为按窗口矩形分配。
5. 中文副标题一度显示成乱码 —— 排查后确认是**测试手法**的问题（管道写入走 GBK 控制台代码页），
   不是产品缺陷；改用显式 UTF-8 代码页写入后正常。

## 5. 本轮仍未验证 / 未实现

1. 通知的实时到达仍未实测（只验证了「历史通知不会在启动时刷出」）。
2. 图片剪贴板只读了尺寸，**没有解码位图**，胶囊仍用矢量图标而非缩略图。
3. 复制来源归因依赖前台窗口：资源管理器右键「复制」或全局热键代劳时会归到 `explorer.exe`。
4. Python 规约层**尚未**镜像规则引擎，跨语言一致性只覆盖原有 7 项常量。
5. 多显示器、混合 DPI 跟随、性能基线、长时间泄漏观察：仍未做。

---

# 第六轮（2026-11-12）：「产品还有什么问题」——实地体检 + 修掉 3 个致命缺陷

用户问的是问题，不是要新功能。所以这一轮先测、再判、最后只修必修的。
结论先说：**能跑，但离「能给别人用」还差一截**；其中三个缺陷会直接毁掉核心体验，
本轮已修，其余如实列出。

## 1. 实测基线（都是真跑出来的数）

| 项目 | 实测值 | 判读 |
|------|--------|------|
| 空闲 CPU（无活动） | 1.4% 单核 / 0.09% 全机 | 可接受，60fps 循环有限速 |
| 渲染中 CPU（剧本持续切换） | 15.3% 单核 / 0.96% 全机 | 偏高，GDI+ 全画布重绘的代价 |
| 工作集内存 | 29.0 → 29.3 MB / 8s | 平稳，无泄漏迹象 |
| 句柄 | 297 → 297 | 平稳 |
| 线程 | 10 | 采集线程 + WinRT + GDI+ |
| 同时启动两个实例 | **2 个进程，胶囊像素级重叠** | 缺陷，已修 |

## 2. 修掉的三个致命缺陷

### 2.1 每种瞬时事件在同一进程里只提示第一次（功能等于残废）

`ActivityStore::PurgeExpired` 只把过期活动标成 `Stale`、**不物理删除**（留给退场动画），
而主循环用 `Get(id) == nullptr` 判断「要不要插入新提示」。所有活动 id 都是固定字符串
（`live-clipboard` / `live-notif` / `live-file` …），于是第二次复制时 `Get` 拿到的是
**上一条已经过期的残留**，判断永远为假 —— 静默跳过。

实机证据（`build/expand_twice.log`）：

```
live ... clip=text/180 ...          ← 第一次复制，采到了
[t=+4.902s] +clipboard 「已复制」    ← 有提示
[t=+8.11s] state=Idle ... expired=1 ← 3.2 秒后过期
live ... clip=text/185 ...          ← 第二次复制，采集层明明采到了（180→185）
                                    ← 但没有任何 +clipboard
```

也就是说：**用户复制第二次起，灵动岛完全没有反应**。这正好是用户当初提的核心诉求。

修法：插入前先 `RemoveIfStale(id)`（新增方法），并在 `core_tests` 加 11 项断言锁住。
修复后实机（`build/fix_twice.log`）：第二次复制在 `t=+9.577s` 正常弹出。

### 2.2 「同一个 id 重新开始」会抛异常，直接把进程带走

媒体会话消失（关掉播放器）→ `EndActivity("live-media")` 标成 `Ended` →
再打开播放器 → `StartActivity("live-media")` → `Upsert` 命中
「终止态必须移除」的校验 → 抛 `logic_error` → 主循环没有 try/catch →
一路冒到 `wWinMain` 的 catch → 进程退出。**胶囊整个消失**。

修法：`StartActivity` 在 `Upsert` 前把 `Ended`/`Dismissed` 的记录清掉（那条校验本身有价值，保留）。
`TestRestartAfterEnd` 先在旧代码上跑出 3 个失败（`!threw` fail），修复后全绿。

### 2.3 展开意图残留，之后每条同类事件都满屏铺开

`expand_id` 在目标活动退场后没被清空。下次同类活动插入时（同一个固定 id），
`!expand_id.empty() && state != Expanded && Can(Expand)` 立刻成立 ——
**跳过 Compact 直接进 Expanded**。

实机证据（`build/expand_after.log`）：第一次 `Compact → auto expand → Expanded`，
第二次只有 `state=Expanded`，中间没有 Compact。

修法：`Tick` 之后检查 `expand_id` 是否还在 `frame.visible` 里，不在就清掉。
修复后（`build/expand_fix.log`）日志出现 `expand target gone: live-clipboard`，
第二次正确回到 Compact。

## 3. 顺手修的三个体验问题

1. **连续复制时岛上显示的还是上一段文字**：3.2 秒窗口内再复制，内容被完全忽略。
   改为「同 id 但 title/content_json 变了就原地替换并重新计时」。
   实机日志：`+clipboard` → `~clipboard` → `~clipboard`，三次内容都跟上，
   退场时间也正确落在最后一次 +3.2 秒（`build/fix_content.log`）。
   视觉证据 `build/proof_refresh.png`：副标题是最新那段，不是第一条。
2. **展开态右下角还写着「右键退出」**：第五轮已经把右键改成弹菜单（设置/暂停/退出），
   提示没跟着改，用户按提示点会以为点错。改为「左键收起 · 右键菜单」。
3. **胶囊内部有肉眼可见的横向色带**：白色 alpha 0.10→0 铺满整高，8bit 只有约 26 级，
   每级差 4~5px，深色底上一眼能数出七八条。把渐变跨度压到 42% 高度
   （`WrapModeClamp` 保证 42% 以下无硬边）。
   用 `png_probe` 读像素列做前后对比：

   | | 灰阶级数 | 单级持续 | 单调性 |
   |---|---|---|---|
   | 修改前 | 9 级 | 6~7 行 | **反复回升**（脏条纹） |
   | 修改后 | 26 级 | 2~4 行 | 单调递减 |

   注：这一条不是凭肉眼或图像模型下的结论 —— 图像模型两次都说「有条带」，
   所以专门写了 `probes/png_probe.cpp` 读真实像素值，避免把压缩伪影当真问题。

## 4. 补上的产品化缺口：单实例 + 「再点一次 exe 就现身」

原来同时跑两个实例会画两个完全重叠的胶囊（实测 `实例数量 = 2`），
两套采集线程都在读剪贴板/通知。而更实际的问题是：胶囊闲时 alpha 全 0，
鼠标点不到，又**没有托盘图标** —— 用户既看不出它在跑，也找不到退出入口。

现在的做法（比加托盘更轻）：`CreateMutexW` 做单实例；第二个进程不静默退出，
而是 `FindWindowW` 找到已有实例后 `PostMessage(kMsgPresence)`，
已有实例投一条 4.5 秒的「灵动岛正在运行 / 左键展开 · 右键可设置或退出」提示。

实测：第二次启动输出 `already running: presence ping sent, this process exits`，
进程数保持 1；第一个实例日志出现 `presence hint shown` 并弹胶囊
（桌面证据 `build/proof_presence.png`）。

## 5. 单元测试

```
core_tests:   run=87  failed=0   （新增 TestStaleReentry 11 项 + TestRestartAfterEnd 7 项）
rules_tests:  run=137 failed=0
render_tests: run=20  failed=0
```

## 6. 还没解决的问题（按严重度）

**A. 会让用户困惑**
1. **通知中心为空时，第一条通知会被吞**。`PollNotifications` 用
   `seen_ids_.empty()` 判断「是否首次采样」，通知中心被清空过就永远为空，
   于是下一条通知被当成基线跳过。这是纯代码路径判定（清空 API 被系统拒绝
   0x80070490，无法实机复现），但逻辑上确定。
2. **通知有最高 2 秒延迟**：`kNotifEveryTicks = 20`，2 秒轮询一次。
   iOS 是即时的。应改 `UserNotificationListener.NotificationChanged` 事件驱动。
3. **没有托盘图标**，也没有开机自启、没有安装器。现在只能命令行启动。

**B. 视觉/交互打磨**
4. 8bit 量化的**残留色带**：26 级在深色底上仍可辨，彻底解决要引入抖动或换 Direct2D。
5. **没有投影**：浅色/复杂壁纸上胶囊与背景的分离度依赖白色 0.16 描边，
   在浅色壁纸上这圈描边会变成一圈浅灰过渡，看起来像脏边。
6. **图片剪贴板只读尺寸不解码**，胶囊里始终是矢量图标，不是缩略图；专辑封面同样只有占位音符块。
7. 展开态**没有可点的操作**：点通知不跳转到对应应用，也没有「清除」按钮。

**C. 工程债**
8. 只取主屏（`MONITOR_DEFAULTTOPRIMARY`），运行中改 DPI 不重排。
9. 日志每次启动 `trunc`，崩溃现场会被下一次启动清掉。
10. Python 规约层没有镜像规则引擎和新的状态机语义，跨语言一致性只覆盖原有 7 项常量。
11. `build/` 已累积 2.2 GB（截图与日志），没有清理机制，也没进 `.gitignore` 的产物目录约定。

---

# 第七轮（2026-11-12）：视觉重做 + 专辑封面

用户原话：「继续解决，美化形象」。所以这一轮只有两件事：把观感做到 iOS 级别，
顺手补掉上一轮列出的几个明确缺陷。

## 1. 找到「怎么调都脏」的真正原因：GDI+ 的 gamma 混合

之前胶囊内部总蒙着一层灰雾，把白色高光的 alpha 从 0.10 降到 0.075 也没用。
用 `png_probe` 读像素才发现：写 7.5% 的白，实际输出是 **RGB(64,64,64)**。

原因：`CompositingQualityHighQuality` 走的是 gamma 空间混合。底色 (10,10,13)
在线性空间里几乎为 0，叠 7.5% 的白后线性值被放大，转回 sRGB 就是七十多。
**对深色底来说，半透明白是最不可控的画法。**

改法：受光改成**不透明径向渐变**（中心 #24242C → 边缘底色）。写多少就是多少，
可以用探针逐像素核对。改完同一位置从 64 降到 **25~36**，灰雾消失。

## 2. 消除 8bit 色带：有序抖动

深色胶囊上的柔光是白色 alpha 渐变，8bit 一共二十几级，每级在屏幕上占 4~6 行，
于是渲染出一圈圈同心色带 —— 这就是"廉价塑料感"的来源。

GDI+ 没有抖动开关，所以在像素缓冲上补：`LayeredSurface::Dither()`，4x4 Bayer，
幅度 ±1 LSB。为什么用 Bayer 而不是随机噪声：随机噪声每帧图案都不同，静止画面上会"闪"；
Bayer 与坐标绑定，画面静止时图案也静止。

## 3. 视觉细节（逐条都有前后对比）

| 项 | 之前 | 现在 | 依据 |
|---|---|---|---|
| 投影 | 3 层，能看到同心硬边 | 7 层、每层 alpha≈0.04、越外越下偏 | 截图 |
| 描边 | 整圈同亮度，像塑料贴纸 | 上 78 → 下 9 的四点插值，只用顶部 20% 表现受光 | 截图 |
| 描边位置 | Pen 以路径为中心，一半落在路径外，浅色壁纸上是一圈灰线 | `SetClip(&path)` 裁到内侧 | 截图 |
| 图标 | 纯平色块 | 上亮下暗 + 内部不透明柔光 | 截图 |
| 节奏感 | 指数逼近（"长出来"） | 弹簧 ζ≈0.68，过冲约 5%（"甩出来"） | 代码 + 手感 |
| 文字层级 | 半透明白，gamma 后被放大，副标题接近主标题亮度 | 不透明分级色（150 / 108 / 248） | 截图 |
| 展开态分隔线 | 横贯全宽的线，页脚只有一句话，显得吵 | 去掉，靠间距分层 | 截图 |

## 4. 专辑封面（从「色块 + 音符」到真实封面）

链路：`SMTC Thumbnail` → `IRandomAccessStreamReference` → `DataReader` 读字节 →
`SHCreateMemStream` + GDI+ 解码 → 中心裁成正方形、缩到 160px →
`%LOCALAPPDATA%\DynamicIsland-Win\covers\<fnv1a>.png` → 渲染层按路径加载并缓存。

为什么不塞进 `content_json`：那是跨层文本契约，塞二进制要么 base64 膨胀 33%，
要么让 `Activity` 依赖平台类型。用路径让契约保持纯数据。

**端到端实测**（不是"应该能用"）：

```
[inject] thumbnail attached: img0.jpg          ← 探针注入带封面的真实 SMTC 会话
[1] session app=media_inject_probe.exe
[2] title=SMTC 链路验证
[3] Thumbnail reference OK
[4] stream size=1602752 bytes  content_type=image/jpeg
RESULT: cover chain reachable
```

然后跑灵动岛，`covers/` 下出现 `329c7ecdbdaa36a9.png`（1.6MB JPEG → 60KB PNG），
截图 `build/proof_media_cover.png` 里左侧就是这张真实封面。

## 5. 这一轮抓到的两个缺陷

1. **媒体展开态没有进度条**。`content_json` 只写了 `position_ms` / `end_ms`，
   没有 `progress` 字段；而 `BuildView` 靠 `ParseProgress` 决定「要不要进度条、用多高的面板」，
   于是媒体展开后是没有进度条的矮面板 —— 截图里只有时间没有条。
   补上 progress（clamp 到 0..1）后，面板高度和进度条同时回来。
2. **给会话挂封面时 `GetFileFromPathAsync("C:////Windows////...")` 返回 0x800700A1**。
   桌面进程对受保护目录没有 StorageFile 访问权，先复制到 temp 再引用。
   这个错只影响探针（产品拿的是系统给的流，不涉及路径），但会让人误判成"封面读不到"。

## 6. 顺手修的两个遗留项

- **通知中心被清空后，下一条通知会被当基线吞掉**：原来用 `seen_ids_.empty()` 判断
  「是不是首次采样」，用户点过「清除所有通知」后集合就是空的，于是永远吞掉下一条。
  改成显式 `notif_primed_` 标志（只在真正第一次采样时建基线）。
  注：清空 API 被系统拒绝（0x80070490），本条仍无法实机复现，属代码路径修正。
- **日志每次启动 trunc**：崩溃现场会被下一次启动擦掉。改为追加 + 超 2MB 滚动成 `.old`，
  连 `FATAL:` 那一行也不再截断。

## 7. 回归

```
core_tests 87 / rules_tests 137 / render_tests 20   全绿，六目标零警告
```

真实链路复跑（`build/final.log`）：

```
[t=+7.859s] +clipboard 「已复制」       ← 第一次复制
[t=+11.98s] ~clipboard 「已复制」       ← 第二次复制，内容更新
[t=+15.29s] +notification 「灵动岛通知回归」  ← 通知实时到达
```

## 8. 仍未做

1. 封面缓存只增不减：听了多少张专辑就留多少个 PNG（每张约 60KB）。
   需要 LRU 或启动时清理 30 天未访问的文件。
2. 通知的应用图标仍然没有：`AppInfo().DisplayInfo().Logo()` 是同样一条
   「引用 → 解码」的链路，封面这轮已经跑通，图标可以复用，但没做。
3. 展开态仍然没有可点的操作（播放/暂停、上一首、清除）。
4. 多显示器、混合 DPI、托盘/自启/安装器：仍未做。
5. Python 规约层没有镜像这一轮的渲染参数（它本来就不含渲染）。

# 第八轮（2026-11-12）：磨砂玻璃 —— 背景透出 + 模糊 + 光影

用户反馈「还不够美观，不是 Mac 那种透明玻璃效果」。本轮把纯不透明深色胶囊
改成磨砂玻璃：透出背后的屏幕内容（实时模糊）+ 顶部受光 + 镜面亮线。

## 1. 技术路线：为什么必须自绘模糊

- DWM 亚克力（`SetWindowCompositionAttribute`）与 `UpdateLayeredWindow`
  逐像素合成不兼容 —— 分层窗口拿不到系统模糊，只能自己截屏 + 自己模糊。
- 自绘的好处是全部参数可控：模糊半径、去饱和量、染色深度都能调，
  而且不受系统主题 / 性能开关影响。

## 2. 捕获语义（两类，日志里能区分）

- **epoch**：胶囊刚要出现、屏幕上还没有它时，截一块干净背景。
- **refresh**：胶囊可见期间每 2 秒重截一次；此时截屏里含有胶囊自己，
  用最近一次 epoch 的像素修补被胶囊遮挡的矩形后再模糊 —— 无闪烁。
- 实测（white_bg_probe，白底+四块高饱和色块）：
  `glass: epoch 904x348 at (1468,12) raw=178.9 blur=85.2` —— 模糊+染色
  把 178.9 压到 85.2，方向正确（白底压暗、彩块变柔和色晕）。

## 3. 本轮最大的 bug：模糊输出整图全黑

- 现象：胶囊内部亮度均值 8-10（≈纯染色），玻璃一直是黑的。
- 逐层排查（方法论：探针先行、像素定论）：
  1. 误判 CAPTUREBLT → 移除后 raw 仍是 0.5，排除；
  2. 加 raw/blur 双诊断 → raw=254、blur=0.5，锁定模糊算子；
  3. 写独立 `probes/backdrop_probe.cpp` 复现 → 同样 0.5；
  4. 写 scratch 程序只跑模糊 → `[H] writes=19662` 但 mid 像素 (0,0,0,0)。
- **根因**：`BoxBlurH` 里 `dst[0]` 固定写同一位置，不随 x 前进 ——
  除首像素外整行被下一次覆盖前的值留在原地，输出全黑。
  修复：`out = dst + x*4` 指针随 x 推进。修后 blur=141.8 与 raw 一致。

## 4. 两个连带发现

- **CAPTUREBLT 必须禁用**：带上它之后，被本进程分层窗口覆盖的区域
  抓成纯黑（raw=0.5）。纯 `SRCCOPY` 正确。已写进代码注释。
- **盒模糊边缘除数**：边缘样本不足却按满窗宽除 → 四边压暗一圈。
  改为按实际样本数 count 相除。

## 5. 玻璃渲染配方（实测调出，别乱动）

| 层 | 参数 |
|---|---|
| 模糊 | 降采样 4x 块平均 + 3 趟可分离盒模糊半径 14（σ≈27 物理像素）+ 35% 去饱和（向亮度混合） |
| 染色 | 70% 深色 (14,14,19) |
| 顶部受光 | 3-stop 线性渐变 alpha 0.12 → 0（45% 高度处归零，避免硬边） |
| 底部压暗 | alpha 0.20 |
| 底缘反光 | alpha 0.05 |
| 顶边镜面线 | 裁剪到 path ∩ 顶部 36% 带，白 alpha 0.40，线宽 0.028×高度 |

受光渐变两次失败的教训：线性收在 42% 留横向硬边；路径渐变整块提亮
（黑底均值 44→85，像乳白塑料）。**玻璃的明暗应来自背后透出什么，
自己不发光** —— 半透明白覆盖整个胶囊在暗底上必然整体发灰。

## 6. 验证

- **因果对照测试**（`render_tests.cpp` `TestGlassBody`）：
  同一内容分别压在红 / 蓝 / 白 / 黑四块背景上渲染 ——
  glass 模式红蓝差 >4（跟随背景），opaque 模式 <1.5（无视背景），
  白黑差 >25，白底 <200（染色压暗成立），两路径逐路径不同。
- **实机**（white_bg_probe，白底+四块高饱和色块压在胶囊正后方）：
  彩色块以柔和色晕形式透出、顶边镜面亮线清晰、白字可读；
  暗底（终端）下呈深色玻璃不泛灰（黑底均值 24.6）。
- 回归：core 87 / rules 140 / render 25 断言全绿，13 个构建目标零警告。
- 证据：`build/light_desktop.png`（彩色背景以柔和色晕透出 + 顶边镜面亮线）。

## 7. 入口与开关

- `rules.json` 新增 `"glass": true`（缺省开，老配置无此键回落 true）。
- 设置窗口新增勾选框「磨砂玻璃效果（透出背后的屏幕内容）」。
- 命令行 `--hud` 打开调试 HUD（画在深色小胶囊底板上，默认关）。

## 8. 仍未做（本轮新增/延续）

1. refresh 捕获每 2 秒一次，快速移动的窗口背景会有最长 2 秒延迟。
2. 多显示器 / 混合 DPI 下捕获矩形仍按主屏计算。
3. 延续：封面缓存 LRU、通知应用图标、展开态可点操作、托盘/自启/安装器。

# 第九轮（2026-11-12）：通知图标 / 封面时有时无 / QQ 微信不触发

用户反馈三条：① 有时只显示默认图标，没有应用图标和专辑图标；② 有时封面能显示；
③ QQ / 微信来消息灵动岛没反应。

## 1. 根因（每条都有探针或日志证据）

| 现象 | 根因 |
|---|---|
| 通知永远只有默认图标 | 采集层从没取过 `AppInfo().DisplayInfo().Logo()`；补上后发现 **WinRT 三条路线全堵死**（见第 2 节） |
| 专辑封面 / 歌名时有时无 | `PollMedia` 只在 **app_id 变化**时才重读属性；同一个播放器切歌 app_id 不变 → 歌名和封面永远停在上一首，且封面是否为「上一首的缓存」取决于这次会话是怎么起来的 |
| QQ / 微信消息不触发 | 通知 2 秒轮询一次；这类应用「消息一到、聊天窗一点开就把 toast 从通知中心撤掉」，撤得比轮询快就整条错过 |

## 2. 应用图标：WinRT 全堵死 → Win32 AUMID 路线

用 `notif_diag_probe` 逐段打印 HRESULT，三条 WinRT 路线全部排除：

- `AppDisplayInfo::GetLogo(Size)`：16/24/32/44/48/96/256 **全尺寸一律 null 引用**，
  连打包应用（WorkBuddy，DisplayInfo 完整）也是；
- `AppInfo::GetFromAppUserModelId(AUMID)`：`0x80070490`（ERROR_NOT_FOUND），
  对打包 / 未打包一律失败；
- 通知的 `Visual()/Binding` 只有 `GetTextElements`，**没有图片元素**；
  `Notification()` 转 `IToastNotification` 得 `E_NOINTERFACE`，拿不到 XML。

改走 Win32，两级路线（`AppIconForAumid`）：

1. **隐式 AUMID 解码**：形如 `{1AC14E77-…}\WindowsPowerShell\v1.0\powershell.exe`。
   探针实测：`Get-StartApps` 能列出该 AUMID，但**对应的 .lnk 在文件系统里根本不存在**
   （Windows PowerShell 目录里只有 ISE 两个快捷方式）——这类 AUMID 是系统对
   「无显式 AUMID 的快捷方式」合成的，GUID 就是已知文件夹。把 GUID 喂
   `SHGetKnownFolderPath` 再拼相对路径即得 exe。
2. **开始菜单 .lnk 显式 AUMID**：枚举用户 + 全体开始菜单（本机 249 个 .lnk，其中 48 个
   带 `System.AppUserModel.ID`），命中后取目标 exe 图标。打包应用的 AUMID 形态是
   `PFN!AppId`，与通知中心给的 `PFN` 用前缀 + `!` 容忍匹配。

图标统一转 **48px PNG**（`SHIL_EXTRALARGE` → `DrawIconEx` 到 32bpp DIB → 反预乘 → GDI+ 存盘），
与专辑封面共用 `covers` 目录和 Fnv1a 缓存键（`appicon|<AUMID>`），同一应用只解析一次。

验证（`appicon_probe`）：PowerShell（隐式 AUMID）与 WorkBuddy（打包 PFN）都成功落盘，
目视为正确的应用图标、边缘干净无黑底；产品链路日志 `notif: id=2068 … icon=ok`，
截图 `build/cpp/notif_icon.png` 里胶囊左侧就是蓝色 PowerShell 图标。

## 3. 封面 / 歌名：属性重读条件错误

`need_props` 原来只有 `!props_loaded_ || app_id 变化`。修法三条互补：

1. 订阅 `MediaPropertiesChanged`（会话对象变化时重挂，revoker 保活），事件置脏；
2. `media_props_dirty_` 置脏即重读；
3. 每 ~4.8s 兜底重读一次（个别播放器不触发属性事件）。

验证：`media_inject_probe` 新增切歌实验（同一 MediaPlayer、`--title2` + `--switch-at`），
日志出现两行：

```
media: props "第一首歌" cover=ok
media: props "第二首歌" cover=ok
```

截图 `build/cpp/switch_song.png`：封面已换、标题「第二首歌」、进度 0:00 / 2:00。

## 4. QQ / 微信：事件订阅不可用 + 枚举太贵 → 双速轮询

- `UserNotificationListener::NotificationChanged` 订阅返回 **`0x80070490`**，
  `GetAccessStatus` 为 Allowed 也一样；StackOverflow 同类问题（WPF）的结论是
  必须加 MSIX 打包身份。不为这个上打包，放弃事件。
- 但也不能简单改成高频轮询：`notif_diag_probe --bench` 实测
  通知中心 28 条时**单次 `GetNotificationsAsync` 平均 130ms**。
  A/B 实测空闲 CPU：2s 轮询 1.88% 单核，500ms 轮询 **10.94%** 单核。
- 最终方案：**常态 2s + 捕获到新通知后 10 秒内 500ms 快速窗口**。
  聊天消息成串到达，第一条靠 2s 抓住，串内后续快速抓。

实测（`build/cpp/burst_test.log`）：

```
notif: id=2074 … title="第一条" icon=ok
[t=+4.646s] +notification 「第一条」     ← 发送于 ~3.0s，延迟 1.65s（常态 2s 轮询）
notif: id=2075 … title="第二条" icon=ok
[t=+9.331s] ~notification 「第二条」     ← 发送于 ~9.0s，延迟 0.33s（快速窗口 500ms）
```

空闲 CPU 回到 **2.34%** 单核。

## 5. 附带修复

- 采集线程跨线程写日志：`ofstream` 非线程安全 → `note()` 加锁（`log_mutex`）。
- 新增采集层日志钩子 `SetLogger`：`notif:`（采到哪条通知、图标是否解析到）、
  `media:`（属性变化 + 封面状态）—— 这两行是上面所有结论的证据来源。
- 清掉两个遗留 `C4189` 警告（`backdrop.hpp` 里未使用的 `win`）。

## 6. 回归

```
core 87 / rules 152（+12 通知映射断言）/ render 25   全绿，13 目标零警告
```

新增 `TestNotificationMapping`：icon 字段进 content_json、无标题回落应用名、
icon 缺失时 json 仍合法、事件开关关闭不产生活动。

## 7. 仍未做

1. 封面缓存与图标缓存只增不减（每张约 60KB / 10KB），需要 LRU。
2. 通知在系统里存活 <2s（快速被撤）仍可能错过——受 `NotificationChanged` 不可用限制。
3. 应用自己不发系统 toast 的场景（只走应用内弹窗）无法覆盖，已在 README 写明自查步骤。
4. 延续：展开态可点操作、多显示器/混合 DPI、托盘/自启/安装器。

## 第十轮补：玻璃马赛克修复（2026-11-12 19:32 后）

## 1. 现象与根因

用户截图：展开态玻璃里透出饱和红/绿/蓝马赛克，与真实桌面（米色壁纸）完全不符。

诊断链（全部有证据）：

1. `--snapshot` + `--grab` 复现：截屏原始缓冲 raw=253（白桌面），模糊后 blur=112，
   渲染出马赛克 —— 损坏发生在 Blur() 内部，与截屏内容无关（连 epoch 都一样）。
2. 放大快照确认马赛克严格限制在胶囊圆角路径内 → 只可能来自
   `DrawGlassBody` 的 backdrop DrawImage。
3. `backdrop_probe` 独立复现（raw=248 → blur=119，纹理即垃圾）→ 问题锁定在
   `backdrop.hpp` 的 BoxBlurH/V。

根因：滑动窗口盒模糊的**左边缘处理错误**。窗口在 x < radius 时是「扩张」的
（lo 钉在 0），旧代码每步仍按 `x-radius` 减元素——即反复减掉同一个 `v[0]`，
半径 14 累计 15 次。累加和带上 `-15*v[0]` 的恒定偏差；桌面左上角像素是浅色时
（v[0]≈253）偏差达 -3800，除完落在负数，转回 `uint8_t` 回绕成随机饱和彩块。
这也解释了「为什么变成这样了」：v[0] 就是捕获区左上角那颗像素的颜色，
之前测试时它是深色（偏差小看不出），这次桌面背景是浅色 → 立刻爆发。

## 2. 修复

`BoxBlurH/BoxBlurV` 合并为 `BoxBlurAxis(Buffer&, bool horizontal, int radius)`：
`clo/chi` 两根指针，只在元素真正进出窗口时加减，除数恒等于窗口内实际样本数。
与本轮投影重写时 `renderer.hpp` 的 `BoxBlurAxis`（正确实现）同源。

## 3. 回归

- `backdrop_probe`：raw=249.9 → blur=246.9（亮度守恒），纹理为干净浅色柔焦。
- 整窗复现（--expand-at=3000 --snapshot-at=7000）：raw=252.3 → blur=249.7，
  玻璃为均匀磨砂面，无任何彩块（fixed_snapshot.png）。
- 构建+测试全绿：core 87 / rules 152 / render 25，零警告。
- 同一轮已先修复：投影「洋葱圈」毛边（离屏实心圆角矩形 + 盒模糊 + 缓存，
  renderer.hpp 的 DropShadow/BoxBlurAxis/RenderShadowBitmap）与一览模式
  （双击/右键展开全部并行任务，--overview-at=ms 回归）。

## 4. 仍未做

延续上轮：托盘/自启/安装器、缓存 LRU、多显示器/混合 DPI、展开态可点操作、
README 状态表断言数同步。

## 第十一轮：一览点岛拉起应用 / 缓存 LRU / 多显示器 DPI（2026-11-12 20:00 后）

### 1. 一览点击岛直接打开对应应用

数据链路：媒体/通知活动的 `content_json` 补 `app_id`（SMTC 来源 / 通知发送方 AUMID），
剪贴板补 `app_path`（前台进程完整 exe 路径）→ `OverviewItem::launch` → 一览命中测试
`OverviewHitIndex`（客户区坐标按 scale 换算，与 renderer 横排几何同源）→ `LaunchApp`。

`LaunchApp` 三级判定（**按文件是否真实存在，而不是字符串里有没有反斜杠**）：
① `GetFileAttributesW` 命中 → 完整 exe 路径直接 ShellExecute；
② 隐式 AUMID → `ExeFromImplicitAumid` 还原 GUID 对应已知文件夹 + relpath → ShellExecute；
③ 打包 AUMID → `IApplicationActivationManager::ActivateApplication`（已在运行则前置），
   失败回落 `shell:AppsFolder\<AUMID>`。

窗口保留 `WS_EX_NOACTIVATE`：该风格只是不抢焦点，鼠标消息照常收到，因此点岛拉起应用
时不会把正在播放的播放器焦点抢走，比放开 NOACTIVATE 更合适。

无人值守回归注入参数（不传时行为完全不变）：
`--overview-at=ms`、`--click-at=ms --click-island=N`、`--display-at=ms`、
`--overview-launch=<target>`（覆盖一览项目标，剧本模式的 content_json 本无 app_id）。

实测日志（每条都是 `ok`）：
```
overview launch #0 target=C:/Windows/System32/notepad.exe ok
overview launch #1 target={1AC14E77-...}\WindowsPowerShell\v1.0\... ok
overview launch #0 target=Microsoft.WindowsCalculator_8wekyb3d8bbwe!App ok
overview=off (click)                       # 点空白收起
display change: re-layout scale=2.000000 workarea=3840x2160 glass=2512x348 at (664,12)
```
`final_overview.png`：两个独立岛并排、玻璃干净无彩块。

### 2. 本轮实测暴露并修掉的三个真 bug

1. **隐式 AUMID 被当成路径**：旧 `LaunchApp` 用「含 `\`」判断路径，隐式 AUMID 也含
   `\`，直接 ShellExecute 静默失败 → 未打包播放器的岛点了没反应。改为 `GetFileAttributesW`
   判定 + 隐式 AUMID 解析提前。
2. **打包 AUMID 激活 FAILED**：主线程没有 COM apartment（只有采集线程 init 了 MTA），
   `ActivateApplication` 的 CoCreateInstance 直接失败。按调用用 RAII 临时起 STA，
   并加 `shell:AppsFolder` 回落。修复后计算器 AUMID 由 `FAILED` → `ok`。
3. **一览开着却点不动**：剧本 `Action::ExpandOn` 把 runtime 推到 `Expanded`，而
   `BuildView` 一览分支要求 `overview && !expanded` → `view.items` 为空、命中无目标。
   改为 `expanded = !overview && state == Expanded`（一览优先于展开，落实互斥语义）。

### 3. 缓存 LRU

`TrimCache(dir, keep=96)`：每次写入新 PNG 后按 `ftLastWriteTime` 淘汰最旧的，
缓存命中直接返回不扫目录。新增 `probes/cache_probe.cpp` 并纳入构建脚本的 run 段，
造 120 个时间戳严格递增的假 PNG，8 项断言全过：120→96、删最旧 24、保留最新 96、
恰好在上限时幂等不删、96→10 只留 f110..f119、目录不存在不崩。

### 4. 多显示器 / 混合 DPI

`WM_DISPLAYCHANGE` / `WM_DPICHANGED` 只置 `g_display_changed`；主循环 `relayout()`
重算 `WindowScale`、`MonitorFromWindow(MONITOR_DEFAULTTONEAREST)` 的工作区、玻璃捕获
矩形（一览最大宽度 × scale），作废旧 epoch 纹理并下一帧重截。窗口位置每帧 present
自动跟随新 `screen_cx`。`--display-at=5600` 注入验证：日志输出重排后的
`scale=2.0 workarea=3840x2160 glass=2512x348 at (664,12)`。

### 5. 构建与回归

`bash tools/build_cpp_msvc.sh` 全绿：core 87 / rules 155 / render 25 / cache_probe 8，
零警告。README 状态表断言数由 69/137/20 同步为 87/155/25，并新增三行功能状态、
四条踩坑；S7 路线图标注混合 DPI 已完成部分。

### 6. 仍未做

托盘图标 / 开机自启 / 安装包、按显示器 EDID 记住偏好、全屏与反作弊避让、
展开态内可点操作（播放/暂停等）、玻璃 refresh 2s 延迟、缓存按字节而非按文件数淘汰、
圆角 6 倍放大下的 GDI+ 单像素 AA 台阶（1:1 不可见，不做超采样）。

## 第十二轮：展开态可操作 / 在线歌词 / 休眠时钟（2026-11-12 21:00 后）

用户反馈：点开的大界面「看不到歌词、不能关闭、不能拖进度、也没有休眠状态」。
核对代码确认当时展开态是纯展示层：WndProc 只有 LBUTTONUP / DBLCLK / RBUTTONUP，
没有按下与移动、没有按钮字段，Idle 时 alpha 直接压到 0（完全透明）。
方向由用户选定：**在线歌词 API** + **时钟小胶囊休眠态**。

### 1. 在线歌词（LRCLIB，免密钥）

SMTC 只给歌名 / 歌手 / 专辑 / 封面 / 进度，**系统层不提供歌词**，必须另找来源。
`src/win32/lyrics.hpp`：WinHTTP GET `lrclib.net/api/search?track_name=&artist_name=`，
取 `syncedLyrics`（带 [mm:ss.xx] 时间轴），`ParseLrc` 解析成 `{ms, text}`，
`SelectLyricLines` 按 `position_ms` 取上一句 / 当前句 / 下一句。
独立 worker 线程 + 条件变量 + 按 `artist|title` 缓存；切歌丢弃在途结果；
超时 5s / 总 10s；`loading / ok / none / error` 四态都有界面占位。

**实测暴露一个真 bug**：WinHTTP 首次返回 `ok=0`，阶段诊断给出
`WinHttpSendRequest err=12002`（超时）。查本机：`HTTP_PROXY=http://127.0.0.1:3876`，
直连 LRCLIB 8s 超时、走该代理 200 成功，而 .NET / 浏览器读的正是环境变量；
WinHTTP 的 `AUTOMATIC_PROXY` / `DEFAULT_PROXY` 都拿不到它。改为显式读
`HTTPS_PROXY`/`HTTP_PROXY`，优先 `WINHTTP_ACCESS_TYPE_NAMED_PROXY`，失败回落直连。

修复后 `lyrics_probe` 实测：
```
http ok=1 bytes=204787 error=[]
lrc bytes=2045 lines=42
  [28900] 故事的小黃花
  [32729] 從出生那年就飄著
  [36240] 童年的盪鞦韆
at 35s: prev=[故事的小黃花] cur=[從出生那年就飄著] next=[童年的盪鞦韆]
```

### 2. 展开态输入层与媒体控制

`ExpandedHitAreas(pill, scale)` 由绘制与命中测试共用同一份几何（右上角关闭、
封面=播放暂停、加高 9px 的进度条命中带）。WndProc 新增 `WM_LBUTTONDOWN`
（判定命中 + `SetCapture`）、`WM_MOUSEMOVE`（拖动预览）、`WM_LBUTTONUP`
（提交 seek / 触发按钮，且不产生普通单击）。主循环每帧用本帧真实胶囊尺寸刷新命中几何。

SMTC 控制走命令队列，在已 `init_apartment` 的采集线程消费：
`TryChangePlaybackPositionAsync(int64_t)`（**100ns 单位，不是 TimeSpan**，传 TimeSpan 会 C2664）、
`TryTogglePlayPauseAsync()`。关闭按钮用「活动指纹」抑制，直到换歌 / 新通知才重新出现，
否则持续类活动下一帧就被 `StartActivity` 重建，表现为「关不掉」。

`--mouse-at=ms --mouse=close|play|progress` 向窗口自身投递真实鼠标消息序列，
走完整 WndProc 链路（比直接置标志更可信）。实测日志：
```
inject mouse=close (810,86) -> (810,86)      / close: dismissed "晴天"
inject mouse=progress (144,320) -> (620,320) / seek: 71875ms / 100000ms
inject mouse=play (110,110)                   / media: toggle play/pause
```
剧本模式无真实曲目时长，故补 `--seek-end-ms` 注入用于验证进度→毫秒换算。

### 3. 休眠时钟

无活动且未被暂停 / 全屏避让时，`BuildView` 返回 `idle_clock=true` 的小药丸
（96×46，只画一枚圆点 + `HH:MM`），alpha 目标保持 1.0 而非 0。
新事件到来立刻回到正常胶囊。帧预算在休眠态降到 10fps，避免为一个每分钟才变的数字
让 60fps 绘制循环常驻。`idle_clock` / `lyrics` 两个配置开关，关闭后完全不发网络请求 /
回到完全透明。实测日志 `idle clock: 21:20 (sleeping capsule visible)`，
截图 `e2e_idle.png` 确认形态。

### 4. 视觉与回归

- `e2e_lyric.png`：三行歌词（暗/亮/暗）、右上角 ×、封面右下暂停角标、底部进度条、
  页脚「拖进度 · × 关闭 · 右键菜单」，玻璃干净无彩块。
- `e2e_idle.png`：蓝点 + 21:20 的小药丸，画布其余透明。
- 新增 `tests/cpp/interaction_tests.cpp`（45 项断言：LRC 多时间戳 / offset / 排序、
  选行前奏与末尾、URL 编码、JSON `\uXXXX` 与代理对、命中几何与进度 clamp）。
- 新增 `probes/lyrics_probe.cpp`，构建脚本加 `winhttp.lib`。
- 全量构建全绿：core 87 / rules 155 / render 25 / interaction 45 / cache 8，零警告。

### 5. 注入参数一览（不传时行为不变）

`--mouse-at` / `--mouse` / `--mouse-progress`、`--lyric-lrc` / `--lyric-pos-ms`、
`--seek-end-ms`，加上此前的 `--overview-at` / `--click-at` / `--click-island` /
`--display-at` / `--overview-launch` / `--expand-at` / `--snapshot-at` / `--duration`。

### 6. 仍未做

托盘图标 / 开机自启 / 安装器、按显示器 EDID 记偏好、全屏与反作弊避让、
展开态内上一首/下一首、歌词手动纠正与多候选源、缓存按字节淘汰、
圆角 6 倍放大下的 GDI+ 单像素 AA 台阶（1:1 不可见，不做超采样）。

## 第十二轮补：越界与休眠形态修正（2026-11-12 21:45）

用户截图指出两点：展开态「有些东西越界了」，且休眠样式不是想要的
「半透明小扁条待机」——居中一块会挡住内容。

### 1. 越界的根因：圆角半径取 Height*0.5

`DrawPill` 里 `radius = r.Height * 0.5`。紧凑态 46 高时半径 23，正好是药丸，
一切正常；但本轮把媒体展开态高度提到 196（容纳三行歌词）后，半径变成 **98**，
胶囊两端成了半圆，而封面 chip、左下时间、右下提示仍按**矩形**布局 ——
它们全部落到圆弧外面，视觉上就是「内容越界」。

修法：`radius = min(Height * 0.5, 28 * scale)`。紧凑 / 休眠态仍是药丸（23），
展开态固定 28 逻辑像素的圆角矩形。这是典型的「只有把高度从小改到大才暴露」的
几何耦合 bug，单看代码完全合理。

### 2. 休眠形态：从居中药丸改成贴顶半透明扁条

- 尺寸 96×46 → **132×26**（高度压到状态栏厚度）。
- 整体 alpha 乘 **0.55**（主循环 `drawn.alpha = anim.alpha * kIdleClockAlpha`）。
- **强制走深色底、不走玻璃**：玻璃会透出背景，浅色壁纸下整条变浅灰、白字看不清；
  待机条要求任何壁纸下都读得清。
- **不投投影**：26px 的条再拖个影子反而更显眼。
- 内容改为一枚小圆点 + 左对齐 `HH:MM`，像一条状态条而不是缩小的播放器。

### 3. 歌词失败的可诊断性

用户截图里只有「歌词获取失败」，日志查不到原因。`LyricFetcher` 增加 `LastError()`
（代理 / 直连各自返回的错误），主循环在状态变化时输出一行：
`lyrics: status=error (proxy ...; direct WinHttpSendRequest err=12002) lines=0`。
注意：从桌面双击启动时进程**不继承终端的环境变量**，若本机依赖 `HTTP_PROXY`
才能出网，需要把该变量设为用户级环境变量，否则歌词会走直连超时。

### 4. 回归

- `e2e_lyric3.png`：圆角矩形，封面 / 标题 / 三行歌词 / 进度条 / 时间 / 页脚 / ×
  全部在边界内且有余量。
- `e2e_idle3.png`：深色半透明细扁条，蓝点 + `21:45` 可读，无投影。
- 日志 `idle clock: 21:45 (sleeping capsule visible)`。
- 构建全绿：core 87 / rules 155 / render 25 / interaction 45 / cache 8，零警告。

## 第十二轮再补：待机把手 + 歌词代理（2026-11-12 21:55）

用户三点反馈：① 展开态没有歌词；② 不知道怎么点出多窗口；③ 休眠样式要
「贴边、没有时间、就是一条透明扁条」。

### 1. 歌词为什么还是失败：IPv6 优先导致 12002

日志证据：`lyrics: status=error (direct WinHttpSendRequest err=12002)`，**没有 proxy 前缀**
—— 说明进程里根本没读到 `HTTP_PROXY`。用户是从桌面启动的，不继承终端环境变量。

网络实测：
| 探测 | 结果 |
|---|---|
| 127.0.0.1:7890（注册表里的代理） | **关闭** |
| 127.0.0.1:3876 | 开放 |
| IPv4 172.67.167.238:443 | **可达** |
| IPv6 2606:4700:3031::ac43:a7ee:443 | **不可达** |

即：直连时解析出 IPv6 优先，连不通就 12002 超时；IPv4 其实通，但 WinHTTP 没有
「强制地址族」选项，用 IPv4 字面量连接又会导致 TLS SNI/证书不匹配。所以**能出网的
唯一稳定路径是代理**。

修法（三路兜底 + 显式配置）：
`lyrics_proxy`（rules.json）→ `HTTPS_PROXY`/`HTTP_PROXY` 环境变量 →
`WinHttpGetIEProxyConfigForCurrentUser`（Internet 选项）→ 直连。

用户配置已写入 `"lyrics_proxy": "http://127.0.0.1:3876"`，实例重启后验证：
```
lyrics: fetch "DynamicIsland-Win - SMTC 链路验证"
lyrics: status=none lines=0          ← 不是 error，HTTP 通了，只是这首歌没歌词
```
（用 `media_inject_probe` 造真实 SMTC 会话验证，避免依赖用户当时是否在放歌。）

### 2. 休眠态：改成「贴边的把手」

- 96×46 药丸 → 132×26 状态条 → **220×8 的极淡长条**（本次终态）。
- **不显示时间、不画圆点**：一旦放了内容它就又变成挡视线的面板。
- 整条 alpha **0.22**，只在顶部留一道 30% 白的极淡高光，让它像「可按的细条」。
- **真正贴边**：canvas 顶部有 `kMargin` 透明留白，present 时把窗口上移同样的距离，
  细条顶边正好压在 `rcWork.top` 上。屏幕实拍 `e2e_grab_idle.png` 确认。
- 休眠时不抓玻璃（不需要背景模糊，省掉每 2s 一次抓屏）。

### 3. 多窗口（一览）的触发条件

双击胶囊或右键「展开全部任务（N）」。**前提是同时有 ≥2 件任务** ——
只有 1 件时双击不会铺开（没有第二件可铺）。README 已补这段说明，
并指出右键菜单里那条「展开全部任务（N）」本身就是「当前有几件事」的指示器。

### 4. 回归

- 构建全绿：core 87 / rules 155 / render 25 / interaction 45 / cache 8，零警告。
- `e2e_grab_idle.png`：屏幕抓取，细条贴在最顶边、无文字。
- `e2e_lyric_live2.png`：真实会话展开态，歌词区显示「暂无歌词」占位，布局无越界。

## 第十二轮四补：视频应用的图标（2026-11-12 22:00）

用户反馈：B 站这类视频软件左侧不显示应用图标，只有一个粉色方块 + 播放三角；
并问「大方块 / 小方块是不是该换成更适合视频的样式」。

### 1. 根因：UWP 应用两条路都不通

- `AppIconForAumid("com.bilibili.bilibiliPC")` 返回空：开始菜单 249 个 .lnk 里
  48 个带 AUMID，**匹配 0**。
- 查 `Get-StartApps`：B 站的 AppID 是 **`BiliBiliPC`**，而 SMTC 报的是
  **`com.bilibili.bilibiliPC`** —— 两个字符串毫无关系，任何前缀匹配都不可能成立。
- 更根本的是：UWP 应用的快捷方式**不在文件系统里**（`shell:AppsFolder` 是虚拟
  命名空间），扫开始菜单目录永远扫不到。

### 2. 四次尝试才拿对图标（每步都有实测依据）

| 尝试 | 结果 |
|---|---|
| 放宽 `AumidMatches` 支持 `_` 前缀 | 仍 `matched: 0`（不是前缀问题） |
| 枚举 AppsFolder + `PKEY_AppUserModel_ID` | `apps total=314 with_aumid=**0**` —— 这 314 项根本不实现该属性 |
| 改用 `SHGetNameFromIDList(SIGDN_PARENTRELATIVEPARSING)` | 仍 0 |
| 改用 `IShellFolder::GetDisplayNameOf(SHGDN_FORPARSING)` | ✅ 314 项全部拿到，`[9] BiliBiliPC` |
| `SHCreateItemFromIDList` + `GetImage` | 图标是**通用白纸**（pidl 是相对的，指向了别处） |
| `SHCreateItemWithParent` + `GetImage` | ✅ 正确的 B 站 logo |

### 3. 最终实现

`AppIconForAumid` 增加路线 2（在 .lnk 扫描前）：
枚举 `shell:AppsFolder` → `GetDisplayNameOf(SHGDN_FORPARSING)` 取 AUMID →
「包族名末段 vs AppID」匹配（`bilibiliPC` ≈ `BiliBiliPC`）→
`SHCreateItemWithParent` 建项 → `IShellItemImageFactory::GetImage(48×48, ICONONLY)` →
`SaveHBitmapPng`（**反预乘**后写 PNG，否则边缘发黑）。

同时媒体活动补 `icon_path`（`content_json` 的 `icon` 字段），渲染时
`cover → icon → 纯色占位` 三级回落。之前只有通知活动有图标，媒体一直画占位色块。

### 4. 结果

`appicon_probe com.bilibili.bilibiliPC` → `covers\848f07f4ae20151f.png`，
肉眼确认是粉底 + 白色 `bilibili` 字标 + 天线的官方 logo。
`e2e_bsite2.png`：展开态左上角显示该 logo（圆角裁剪 + 暗边），带暂停角标。

### 5. 关于「更适合视频的样式」

目前的做法是「有封面用封面，没封面用应用图标」。B 站这类播放器**不通过 SMTC
提供视频缩略图**（`Thumbnail` 为空），所以拿不到真正的画面帧。若要做 16:9 横版
缩略图位，只能放 logo 居中、两侧留白 —— 反而比正方形更空，故暂保持正方形，
这一点已在回复里向用户说明并留待其选择。

## 第十二轮五补：路径式 AUMID / 无歌词收起 / seek 反馈（2026-11-12 22:15）

用户三点：① 腾讯视频这类软件没有图标；② 视频拿不到缩略图、音乐没有歌词时应该
自动换成更美观的样式；③ 「连拖进度这种操作都做不了」。

### 1. 腾讯视频：AUMID 直接是 exe 路径

`Get-StartApps` 显示腾讯视频的 AppID 是
`D:/multi_media/video/tx_video/QQLive/QQLive.exe` —— **不是 AUMID，是完整路径**。
`AppIconForAumid` 的四条路线全是按 AUMID 设计的（隐式 / AppsFolder / .lnk / 目标 exe），
对路径输入全部无效，所以永远画占位色块。

修法：在缓存命中之后、路线 1 之前加「路线 0」——输入含 `\` 或 `:` 就当作 exe 路径
直接 `SaveExeIconPng`。以原始反斜杠路径实测：
`AppIconForAumid → covers\7185a5bf075a51d0.png`，肉眼确认是腾讯视频的彩色播放三角 logo。

（注意：用 Git Bash 传 `D:/...` 会被 MSYS 转成正斜杠 `/d/...`，`SHGetFileInfoW`
就找不到文件了。这类路径参数要用不做路径转换的终端调用。）

### 2. 没有歌词 / 缩略图时自动收起

之前媒体展开态固定 196 高，只为容纳三行歌词；视频和纯音乐没有歌词时，
就变成「一大片空白 + 一句暂无歌词」，既难看又白白挡窗口。

改为：`BuildView` 新增 `lyric_ready`，只有**真取到歌词**时才启用三行区并把高度
提到 196；否则回到常规 124。主循环用 `lyric_fetcher.Status() == "ok"` 判断
（便宜，不拷贝整首歌词）。歌词异步到位后高度由弹簧动画过渡，不是硬跳。

`e2e_nolyric.png`：无歌词时是紧凑卡片，封面 / 标题 / 副标题 / 进度条紧挨着，
没有空白，也没有「暂无歌词」。

### 3. 拖动进度：链路本来是通的，缺的是反馈

日志里其实有 `seek: 35368ms / 96666ms`、`seek: 47451ms`、`seek: 44808ms`
—— 用户确实拖动并提交过 seek。问题在「提交后进度条立刻跳回原位置」，
看起来就像没拖动。

两处改进：
- **乐观更新**：松手后进度条先停在目标位置 1.5s（`seek_hold_progress`），
  等采集层读到播放器的新位置再交还真实值。
- **播放器拒绝要能看见**：`TryChangePlaybackPositionAsync` 的返回值以前被忽略，
  现在 false 时记 `seek: player rejected position change (unsupported)` ——
  部分播放器不支持 SMTC 定位，那是播放器侧限制，至少要在日志里说清楚。

### 4. 顺带补的诊断

`PollMedia` 在 app_id 变化时记一行
`media: app_id="…" icon=ok|none` —— 「某个播放器不显示图标」这类问题，
以前只能靠猜，现在一眼能看出它报的是什么 ID、图标有没有取到。

### 5. 回归

构建全绿：core 87 / rules 155 / render 25 / interaction 45 / cache 8，零警告。

## 第十二轮六补：无时长媒体的「幽灵进度条」（2026-11-12 22:20）

用户截图：展开态明明没有进度条，但**拖动时会出现一条，松手后又消失**。

### 1. 成因链（三步，全部可复现）

1. B 站不通过 SMTC 提供总时长（`end_ms = 0`）→ `progress = position/end` 无效 → -1
   → `DrawExpanded` 的 `if (v.progress >= 0)` 不成立 → **不画进度条**（这一层是对的）。
2. 但命中区 `ExpandedHitAreas` 是**无条件生成**的 → `g_hit.progress` 一直有效
   → 点/拖这个区域仍会进入 `g_dragging`。
3. 拖动时主循环用 `g_drag_progress` 覆盖 `view.progress` → 于是「画」出一根
   本不存在的进度条；松手后 `g_drag_progress = -1`，且 `end_ms <= 0` 不会设置
   `seek_hold_progress` → 条又消失。

### 2. 修法

`HitState` 增加 `progress_draggable`，取值来自**采集层真实进度**
（`view.progress >= 0`，必须在拖动预览覆盖之前取，否则「正在拖」会被误判成「有时长」）。
`WM_LBUTTONDOWN` 里只有 `progress_draggable` 为真才进入拖动。
`g_hit` 在岛隐藏时也一并复位。

顺带：没有进度条时页脚提示从「拖进度 · × 关闭 · 右键菜单」改成
「× 关闭 · 右键菜单」，不再暗示一个不存在的手势。
无时长但已播放过的源，左下角显示 `0:06 / --:--`，而不是退化成 AUMID 字符串。

### 3. 回归（新增 `--no-duration` 注入）

真实 B 站不好复现，所以加了 `--no-duration` 强制把 `progress` 置 -1：

| 场景 | 结果 |
|---|---|
| 有时长（`media_inject_probe`，end=120s）+ 拖进度 | `seek: 83906ms / 120000ms` —— 正常提交 |
| `--no-duration` + 拖进度 | 有 `inject mouse=progress`，但**没有 `seek:`** —— 拖动被正确忽略 |
| `--no-duration` 截图 | 无进度条，展开态是紧凑卡片，页脚只写「× 关闭 · 右键菜单」 |

### 4. 回归

构建全绿：core 87 / rules 155 / render 25 / interaction 45 / cache 8，零警告。

## 第十二轮七补：叉掉之后「新岛不再生成」（2026-11-12 22:30）

用户反馈：打开软件 A → 叉掉灵动岛 → 再打开软件 B 不会生成新岛，
**直到把 A 的进程结束掉**。

### 1. 根因：抑制只作用于「当前会话」，而当前会话一直是 A

上一轮为「点 × 能关掉」加的抑制是：记住被叉活动的内容指纹，只要还在上报就跳过。
问题在于 SMTC 侧同时只能看到**一个**「当前会话」（`GetCurrentSession()`）：

1. 叉掉 A → 采集层继续把 A 当作当前会话上报 → 被指纹跳过 → 岛上什么都没有（符合预期）。
2. 用户打开 B。**B 刚启动通常处于暂停**，抢不到「当前会话」，
   于是采集层仍然只看到 A —— 而 A 被抑制着，界面上就一直没有新岛。
3. 用户结束 A 的进程 → 会话交给 B → 新岛才出现。这就是「必须杀进程」的由来。

也就是说：**抑制的语义被写成了「不要让这个应用出现」，但实现成了
「让整个媒体源都不出数据」**。A 被抑制 ≠ B 不能出现。

### 2. 修法

**采集层**（`live_sources.hpp`）：
- 新增 `SuppressMedia(app, title, ttl)`：被叉掉的那一条传给采集层。
- `PollMedia` 在建好 `next` 之后判断：若它属于抑制项 →
  - 叉掉时是暂停、现在用户主动播起来了 → **解除抑制并放行**（新意图）；
  - 否则遍历 `manager_.GetSessions()`，跳过被抑制的 `app`，取第一个非抑制且标题
    非空的会话（属性读取按「会话变化 / 事件置脏 / 每 ~1.6s」节流，
    播放状态与进度是同步属性、每拍都读），并把事件挂钩与命令路由一起切过去
    （否则 seek / 播放暂停会发给被抑制的那个）；
  - 一个都没有 → 媒体活动退场（**不是隐藏整个岛**，通知 / 复制照常出现）。
- 抑制项用**列表**（上限 8）而不是单槽：并行两条任务时用户会连着叉两个，
  单槽会把前一条挤掉，表现为「叉掉第二个，第一个跳回来」。
- 媒体会话彻底消失时清空抑制：关掉软件再打开是新的意图。

**主循环**（`overlay_main.cpp`）：
- 指纹由 `kind|标题` 改为 `kind|app_id|标题`（否则两个应用同名会互相顶掉）。
- `dismissed` 改为列表；每条在「本帧数据里已不存在」或超时（30 分钟）后删除。
- 叉掉媒体时调用 `sources.SuppressMedia(app_id, title)`。
- 顺带把 `--expand-at` 改成一次性触发：以前关掉活动后 `expand_id` 变空，
  它会立刻又把新活动展开，回归日志全是 `auto expand`，盖掉真正要看的东西。

### 3. 回归（用本机真实两个会话，未用任何桩）

本机当时的 SMTC 状态正好是这个 bug 的现场：两个会话 ——
`com.bilibili.bilibiliPC`（B 站，暂停）与 `QQMusic.exe`（在播）。

```
live media=搁浅 (playing)          ← 当前会话 = QQ 音乐
[t=+4.013s] inject mouse=close
close: dismissed "一个又一个"
[t=+4.03s]  state=Idle visible=0   ← 岛按预期消失
media: current session suppressed, switched to "com.bilibili.bilibiliPC"
live media=科普｜吃什么食物能够提升颜值 (paused)
[t=+4.15s]  state=Compact visible=1 ← 122ms 后接着显示另一个应用
```

修复前同样操作的结果是 `visible=0` 一直持续，直到结束 QQ 音乐进程。
`fix_final.png` 确认此时胶囊左侧是 B 站 logo、标题为 B 站视频名。

### 4. 构建

全绿零警告：core 87 / rules 155 / render 25 / interaction 45 / cache 8。

## 第十三轮：休眠语义重做 / 多任务打不开（2026-11-12 22:40）

用户三点：① 多任务还是打不开；② 休眠状态下点不开灵动岛；③ 休眠不是「没有岛」——
没有岛时就应该什么都不显示，休眠是「有岛但被藏起来」，触发条件是应用全屏、
长时间没点击等，且点一下要能把小岛叫出来。

### 1. 休眠语义：把「没有岛」和「睡着了」分开

旧实现只有一根筋：`frame.visible.empty()` → 画一条 220×8 的待机条。
于是**恰好反了**：没有岛时挂着一条把手（看着像卡住），有岛但全屏避让时
（`suspended` 让 Tick 返回空 frame）反而被当成「没有岛」，alpha 直接压到 0 ——
既看不见也点不到，这就是「休眠状态下点不开」。

新语义：

| 状态 | 判定 | 表现 |
|---|---|---|
| 没有岛 | store 里没有存活活动 | 窗口全透明、点不到（不画任何东西） |
| 休眠 | 有岛 且（全屏避让 或 距上次交互 > `auto_sleep_sec`） | 贴顶 220×10 细条（26% 透明、无文字、只有一个握把） |
| 正常 | 有岛且醒着 | 紧凑胶囊 / 展开 / 一览 |

关键实现点：
- 「有没有岛」要看 **store**，不能只看 `frame.visible`：全屏避让时 frame 是空的，
  活动仍在 store 里（`live_count()` 统计非 Ended/Dismissed 的条数）。
- 入睡时清掉 `expand_id` / `overview`，醒来是紧凑态（不是一睁眼就铺满半个屏幕）。
- 点击处理里 **先判断「刚才睡着没」再刷新 `last_interaction`**：顺序反了
  `now - last` 恒为 0，`is_sleeping()` 恒 false，这一下点击会被当成普通点击直接展开
  —— 第一版就是这么翻车的（日志里 `sleep=off` 紧跟着 `state=Expanded`）。
- 新内容（主活动 id + 标题变化）也算一次交互：睡着时换歌 / 来通知要自动醒，
  否则提示会被闷在细条后面。全屏避让不受影响（suspended 直接返回睡）。
- 配置：`sleep_bar`（默认 true，关掉则休眠=完全隐藏）、`auto_sleep_sec`（默认 60，0=不自动睡）；
  老的 `idle_clock` 仍作为别名读入，升级不会突然变行为。

### 2. 多任务：双击在只有 1 件时会「展开又收起」

旧逻辑：双击 → 无条件 `overview = true` 且 `expand_id.clear()` → 同一帧稍后被
「`visible < 2` 就自动收起」撤销。净效果是「先展开、再收起」，
用户看到的就是**双击没反应**。这正是用户反复报的「多任务打不开」。

现在：`<2 件` 时双击 = 展开那一件（`overview=on (single task -> expand)`）；
`≥2 件` 才铺成一排独立岛。另外补了 `--mouse=dbl` 注入（向窗口投递
down/up/dblclk/up 四连），第一次用**真实双击消息**验证这条路径，
而不是绕过 WndProc 的 `--overview-at`。

### 3. 回归（全部实机，含真实双击消息）

```
# 全屏 → 休眠条
[t=+13.20s] sleep=on (fullscreen)
sleep_fullscreen.png / final_sleep_fs.png：顶部一条细条 + 握把，无文字

# 双击（2 件并行）→ 一览
[t=+4.023s] inject mouse=dbl (452,128)
[t=+4.055s] overview=on
dbl_overview.png：两个独立岛并排（产品站会 / 晴天·周杰伦·叶惠美）

# 双击（1 件）→ 展开
overview=on (single task -> expand) → state=Expanded

# 自动休眠（--sleep-after-ms=3000）+ 点一下叫醒
[t=+3.017s] sleep=on (idle)
[t=+5.409s] wake: click on sleep bar
[t=+5.409s] sleep=off            （停住紧凑态，不再顺手展开）

# 睡着时换歌自动醒
[t=+3.667s] sleep=on (idle)
[t=+8.319s] sleep=off            （QQ 音乐切歌触发）
```

构建全绿零警告：core 87 / rules 155 / render 25 / interaction 45 / cache 8。

### 4. 仍未做

托盘图标 / 开机自启 / 安装包、按显示器 EDID 记偏好、展开态内上一首/下一首、
缓存按字节淘汰、圆角 6 倍放大下的 GDI+ 单像素 AA 台阶（1:1 不可见）。

## 第十四轮：叉掉之后「再也不生成岛」（2026-11-12 23:00）

用户反馈：打开 QQ 音乐 → 叉掉岛 → 再打开 QQ 音乐（或别的软件）都不再生成岛；
并且仍然不知道怎么打开多任务。

### 1. 根因：抑制是「用清空数据」实现的

上一轮为了「叉掉后这条先别显示」，在采集层写了 `next = MediaState{}`。
它把 `title` 一起清掉了，而下一帧「这次不必重读属性」的分支又把这个**空标题**
拷回 `next`。日志里能直接看到两个状态交替抖动：

```
live media=none (paused) ...
live media=(no title) (playing) ...
live media=none (paused) ...
live media=(no title) (playing) ...
```

映射层的条件是 `present && !title.empty()`，于是媒体活动被丢掉；用户看到的就是
「叉掉之后什么都不再出现」。这不是抑制「太强」，而是**抑制把数据搞坏了**。

修法：采集层只在 `MediaState` 上打 `suppressed` 标记（数据保持完整），
由映射层决定要不要生成活动。抑制解除后标题立刻可用，不需要等属性重读。

### 2. 让它能「回来」的四条路（之前只有一条，还很难触发）

| 触发 | 说明 |
|---|---|
| 换歌 / 新内容 | app + 标题不匹配即放行（原来就有） |
| 播放状态变化 | 叉掉时暂停、后来播放；或抑制期间出现「暂停→播放」（本轮新增 `saw_pause` 检测） |
| 应用退出 | 每拍用 `GetSessions()` 比对，抑制项的应用已不在列表就删除（本轮新增，对应「关掉播放器再打开」） |
| 双击 exe | presence 提示的同时**强制清空所有抑制**（本轮新增，是岛完全不可见时唯一可用的入口） |

抑制时长也从 30 分钟收到 **90 秒**：叉掉后不会立刻弹回来，但不会「永久消失」。

### 3. 多任务：给一个看得见的入口

- 紧凑胶囊右侧新增 **+N 角标**（3 件以上才有；2 件时右侧本来就是分离小圆岛，
  不再重复标注）。
- 右键菜单的「展开全部任务（N 个）」以前在 N<2 时**整项消失**，用户完全没有线索；
  现在常驻：N≥2 显示「展开全部任务（N 个）」，N=1 显示「展开当前任务」，
  N=0 灰显「暂无任务」。

### 4. 回归

```
# 叉掉 → 切到另一个会话（不再有空标题抖动）
close: dismissed "可复现歌"
state=Idle visible=0
media: current session suppressed, switched to "QQMusic.exe"
state=Compact visible=1

# 叉掉被抑制的播放器 → 杀掉它 → 重新起同标题会话 → 立即恢复
live media=可复现歌 (playing)     ← 未被 suppressed 命中

# 双击 exe 强制恢复
presence: cleared dismissed (hidden) items
presence hint shown

# 角标：2 件时无 +N（右侧已是分离小圆岛）
state=Minimal visible=2
```

构建全绿零警告：core 87 / rules 155 / render 25 / interaction 45 / cache 8。

## 第十五轮：点岛跳转应用 / 隐藏条件（2026-11-12 23:20）

用户两点：① 点 QQ、QQ 音乐这些岛不会跳转；② 隐藏条件应改为「打开新界面或全屏时」。

### 1. 点击跳转：五类目标字符串，任何一条单独覆盖都会漏

新增：**展开态点「图标 + 标题」→ 打开对应应用**（原来只有一览里点岛才有这动作，
紧凑态单击是展开，用户自然会以为「点岛=回应用」，所以给了这个入口；页脚提示同步改成
「点标题打开应用 · 拖进度 · × 关闭」）。

`LaunchApp` 重排为按可靠性递减的五条路线：

| 顺序 | 形态 | 例子 | 手段 |
|---|---|---|---|
| ① | 完整路径 | `D:/…/QQMusic.exe` | `ShellExecute` |
| ② | **正在运行的进程名**（本轮新增） | `QQ.exe`、`QQMusic.exe` | `CreateToolhelp32Snapshot` 找同名进程 → `QueryFullProcessImageName` 取其 ImagePath 再开 |
| ③ | 隐式 AUMID | `{GUID}\relpath\app.exe` | GUID → 已知文件夹 → exe |
| ④ | AppsFolder 里的 AppID（本轮新增） | `QQ`、`BiliBiliPC` | 枚举 AppsFolder，按「原名 / 去掉 .exe / 包族名末段」匹配 |
| ⑤ | 打包 AUMID | `Microsoft.WindowsCalculator_…!App` | `ActivateApplication` |

同时排除「宿主进程」：腾讯视频在 SMTC 里报的是 `msedgewebview2.exe`，
把用户「打开」到这个进程毫无意义，还会多开一个渲染进程 —— 这一类直接判失败。

实测（`--launch-test=<target> --launch-dry-run`，只解析不启动）：

```
QQ.exe                                       ok  via=shell:AppsFolder\QQ
QQMusic.exe                                  ok  via=D:/multi_media/music/QQMusic/QQMusic.exe
com.bilibili.bilibiliPC                      ok  via=shell:AppsFolder\BiliBiliPC
Microsoft.WindowsCalculator_8wekyb3d8bbwe!App ok  via=shell:AppsFolder\…!App
msedgewebview2.exe                           FAILED via=(unresolved)   ← 宿主进程，故意不猜
```

踩到的两个坑（都有日志/实机证据）：
- `SHCreateItemFromParsingName(L"shell:AppsFolder")` 在主线程没有 COM apartment 时
  **静默失败**：`Get-StartApps` 里明明有 `QQ`，代码却解析不到。把它移到 `ComApartment`
  之后立刻通过。绑定方式也只能用 `IShellItem::BindToHandler(BHID_SFObject)`。
- `ShellExecute` 对 `shell:` 虚拟路径**返回值不可信**：计算器确实被拉起来了，
  返回值仍 ≤32。按返回值判定会写假 `FAILED`。

### 2. 隐藏条件：打开新界面就让路

新增「前台窗口变化」触发：每帧比较 `GetForegroundWindow()`，一变就把
`last_interaction` 归零（等价于「立刻睡」）。下一次点击休眠条叫醒后，
会一直醒着，直到再切窗口。原来的两条（全屏 / 空闲超时）保留。

实测（打开记事本）：

```
[t=+3.171s] sleep=on (new window)     ← 记事本起来的那一帧
[t=+7.496s] sleep=off                 ← 关掉后回来
```

### 3. 构建与回归

全绿零警告：core 87 / rules 155 / render 25 / interaction 45 / cache 8。
新增注入：`--mouse=app`（点图标+标题）、`--launch-test=` / `--launch-dry-run`（逐条验证解析）。

## 第十六轮：真正的多岛（多播放器并行）/ 进度路由（2026-11-12 23:28）

用户三点：① 打开腾讯视频 + QQ 音乐时不触发多岛，很多应用都这样；② B 站是不是永远
不能拉进度；③ QQ 音乐拉进度也无效。

### 1. 多岛：根因是「只取当前会话」

`PollMedia` 一直用 `manager_.GetCurrentSession()` —— SMTC 同时只给一个 session。
同时开着 QQ 音乐和腾讯视频时，另一个**根本不在数据里**，于是：
活动只有 1 条 → `visible=1` → 一览不足 2 件自动收起 → 用户怎么双击都看不到多岛。
这不是「双击打不开」，而是**根本没有第二件任务**。

改成全量遍历 `manager_.GetSessions()`，`Snapshot` 增加 `media_all`，
映射层「一个应用一条活动」：

- id = `live-media-<slug(app_id)>`。**不能带序号**：序号会随播放/暂停的排序变化，
  id 一变活动就被当成新的重建，岛会闪。
- 排序：正在播的在前，再按 app_id，保证顺序稳定。
- 属性读取按应用节流（正在播 1.6s / 其余 4.8s；有 `MediaPropertiesChanged` 时全量重读），
  否则多播放器会串行等一串异步调用。
- 抑制、图标、封面全部按应用独立（图标仍共享一份 `app_icon_*` 缓存）。

实测（本机同时开着 QQ 音乐与腾讯视频）：

```
media: sessions=2
state=Minimal visible=2
overview=on
```
`multi_media.png`：两个岛并排 —— 左「腾讯视频」，右「爱与希望 / 林俊杰…」。

### 2. 进度命令：以前发给「当前会话」，可能不是用户拖的那一个

`SeekToMs` / `TogglePlayPause` 以前固定用 `media_session_`。多播放器时当前会话
可能已经换成别人，用户拖 QQ 音乐的进度实际发到了腾讯视频 —— 现象就是「拖了没反应」。
现在命令带上活动的 `app_id`，采集线程按 id 找到对应 session 再发；日志也带上目标：

```
seek: 8264ms / 15041ms app=msedgewebview2.exe
```

### 3. B 站为什么永远没有进度条

B 站（和多数网页壳播放器）在 SMTC 里 `EndTime` 恒为 0。没有总时长就算不出进度，
所以**不画进度条、也不允许拖动**（这是上一轮修「幽灵进度条」时定下的正确行为，
不是 bug）。要显示视频进度只能绕开 SMTC 注入播放器进程，不在本项目范围内。

QQ 音乐是给了时长的（日志里 `15xxxms`），所以拖得动；若某首仍不跳，
日志会写 `seek: player rejected position change (unsupported) app=…` —— 那是播放器
侧拒绝，不是我们把命令发错了。

### 4. 回归

构建全绿零警告：core 87 / rules 155 / render 25 / interaction 45 / cache 8。

---

## 第十八轮：编码 Agent 岛（2026-11-12 深夜）

目标：主流编码 Agent（Claude Code / Codex / Gemini / WorkBuddy）也要有岛 ——
小岛显示运行状态，点开大岛能看到它此刻在干什么。

### 1. 为什么从「会话文件」读，而不是接各家 SDK

四家都不提供统一的「当前状态」接口，但**都会把每一步落盘成 JSON 行**：
工具调用（`Edit` / `Bash` / `update_plan`）、模型回复、任务清单全在里面。
只读文件的好处：零侵入、不用配置 key、离线可用、可单元测试（给一段固定 JSONL，
断言解析出的动作与清单）。代价是要处理四家格式差异 —— 这层差异全部收在
`src/core/agent_session.hpp` 的 `ParseAgentJsonLine()` 里。

首轮只读「头 64KB + 尾 1MB」：头部有任务与项目，尾部有当前动作与清单，
中间几十 MB 的历史对「现在在干什么」没有价值。之后按 offset 增量读新增字节。

### 2. 实测：本机真实会话

`agent_probe` 直读 `%USERPROFILE%`：

```
[workbuddy] 2026-11-12-14-12-37  (running)
  model   : DeepSeek-V4.1-Flash
  task    : 查看 ZenDesktop 开源项目
  action  : 正在执行 ./build/cpp/diw_overlay.exe --duration=900…
  detail  : Bash · ./build/cpp/diw_overlay.exe --duration=900…
  elapsed : 10h03   tools=140
    · Bash  ./build/cpp/diw_overlay.exe --duration=900…
    · Edit  win32/agent_sources.hpp
    · Read  win32/agent_sources.hpp
```

`--agent-home=<fixture>` 可以指向一份假会话目录，用来做可重复的视觉回归
（`tools/make_agent_fixture.py`，`--two` 生成双 agent，`--idle=N` 生成等待态）。

### 3. 三态截图

| 文件 | 内容 |
|------|------|
| `agent_compact_fixture.png` | 小岛：橙色六芒星 + `DynamicIsland-Win` + `正在执行 tools/build_cpp_msvc.sh` + `2/4` + 三点心跳 |
| `agent_waiting.png` / `agent_overview_waiting.png` | 等待输入：状态点转琥珀，心跳停 |
| `agent_todos.png` | 大岛（Claude Code）：任务 4 条，2 done / 1 in progress / 1 pending，页脚 `Bash · tools/build_cpp_msvc.sh` |
| `agent_codex.png` | 大岛（Codex）：`api-server · 06:05 · gpt-5-codex`，`update_plan` 清单 3 条 |
| `agent_overview.png` | 一览：Claude（橙）+ Codex（青绿），品牌与动作各自独立 |
| `agent_live.png` | 真实会话：无 TodoWrite，走动作时间线模式 |
| `agent_grab2x.png` | 从桌面 DC 抓取，证明三个岛在真实桌面上背景连续、无黑块 |

### 4. 踩到的坑

1. **两套时间基准**：`BuildActivities` 的 `now_ms` 是「进程启动至今」，
   而会话文件里是 epoch。混用 → `quiet` 恒为负 → 所有 agent 永远 running、
   时长永远为空。修法：`Snapshot::wall_now_ms` 单独传。
2. **首轮头尾重叠**：小文件时头段已读完全文，尾部再从 0 读会把事件应用两次
   （工具次数翻倍、时间线重复）。修法：`read_from = max(tail_from, head_consumed)`。
3. **`kMaxVisibleActivities = 2`**：放歌 + 看视频就把 agent 挤掉了。提到 4
   （与 `kOverviewMax` 对齐），契约、契约测试、`check_contracts.py` 同步。
4. **紧凑态被撑成大面板**：动态高度写在了 agent 分支里却没判 `expanded`，
   小胶囊按 233 逻辑高度绘制，内容散成一片。
5. **一览点第二个岛展开的是第一个**：`BuildView` 一直取 `visible.front()`。
   加 `lead_id` 参数 + `LeadOf()`，所有单活动操作（关闭/播放/拖进度/切窗口）
   都按展开的那一件走。
6. **画布变宽后玻璃右半漆黑**：epoch 的覆盖范围是上一次的窗口尺寸，宽度变化
   后修补区右侧贴不到干净像素。修法：记录 `glass_canvas_w/h`，尺寸变化强制重拍 epoch。

### 5. 回归

构建全绿零警告：core 89 / rules 155 / render 25 / interaction 45 / agent 105 /
hanzi 6 / cache 8，契约 7 项通过。

---

## 第十九轮：补齐 Pi / Cursor / ZCode（2026-11-13 凌晨）

用户点名：「cursor、trae、glm 的 deepseek harness、pi 还没适配」。

### 1. 先把「能不能读」查清楚，再动手

本机实测（`%USERPROFILE%` 与 `%APPDATA%` 逐目录看一遍）：

| Agent | 落盘位置 | 结论 |
|-------|----------|------|
| Pi | `~/.pi/agent/sessions/--<cwd>--/<ts>_<uuid>.jsonl` | 明文 JSONL，**可解析** |
| Cursor | `%APPDATA%\Cursor\User\globalStorage\state.vscdb` | SQLite，`composerHeaders` + `cursorDiskKV` 全是明文，**可解析** |
| ZCode（GLM / DeepSeek CLI） | `~/.zcode/cli/db/db.sqlite` | SQLite，`session`/`message`/`part`/`todo` 四张表，**可解析** |
| Trae / Trae CN | `%APPDATA%\Trae\ModularData\ai-agent\database.db` | 前 8 字节 `2a 76 9b a7 fa e8 86 62`，**不是 SQLite**，加密/私有格式 → 不支持 |
| ZCode 桌面版 | `%APPDATA%\ZCode\session\Local Storage\leveldb` | 只有草稿、模型选择（`modelSelection: GLM-5.3`）、窗口布局；正文在云端 |
| DeepSeek Harness Desktop | `%APPDATA%\@deepseek-ai\dsh-desktop\Local Storage\leveldb` | 只有 `{"sessionId":…}` 元数据，正文在云端 |

所以这一轮做 3 家，另 3 家写清「为什么读不到」——
**用户最怕的是「我配了却没反应」，而不是「明确告诉我这家不行」。**

### 2. 实现

新增 `src/win32/sqlite_lite.hpp`：动态绑定系统自带的 `winsqlite3.dll`
（Windows 10+ 必有），只读 URI 打开，`prepare/bind/step` 三个动作，
不引入任何第三方库 —— 项目「clone 下来 cl.exe 就能编」的约定不能破。

- `PollZCode`：查 `session`（最近 6 小时更新的前 3 条）→ `part`（最近 16 条，
  按时间**正序**回放，动作靠后写覆盖前写）→ `todo` 表直接给清单。
  实测：`model=glm-4.6`、`todo=2/4`、`action=正在做 补分页参数的单元测试`。
- `PollCursor`：`composerHeaders` 拿 `lastUpdatedAt/workspaceId/status`，
  `workspaceStorage/<hash>/workspace.json` 的 `folder` 解出真实项目路径（URI 解码），
  再取最后一条 AI bubble 的 `toolFormerData`。
  跳过 `isDraft` 的空草稿（否则界面上会出现一条「Cursor · 1」）。
- Pi：走已有的 JSONL 通道，只加一个 `ParsePiLine` 与一行 provider 注册。

### 3. 踩到的坑

1. **`Patch` 边界留下竖线**：玻璃刷新用 epoch 修补「胶囊挡住的那块矩形」，
   矩形正好卡在画布边缘时，模糊后两边差半个像素 —— Agent 面板右边缘一条淡竖线。
   修法：修补矩形向外扩 `kMargin`。
2. **玻璃捕获高度没跟上**：`glass_h` 原按歌词态 196 算，Agent 面板 254，
   底部 50 逻辑像素没有纹理 → 一条纯黑带。改成 `max(kExpandedLyricH, kAgentExpandedH)`。
3. **`std::mutex` 重入自锁**：`sql_sessions_` 一开始用 `mutex_`，而 `Publish`
   末尾也锁 `mutex_` → 主循环直接卡死。拆成独立的 `sql_mutex_`。
4. **`SetMaxIslands` 把调试上限夹到 4**：`--all` 模式下想一次看全，
   结果被 `min(n,4)` 挡住。放宽到 40（产品侧由 rules 校验 1..4）。

### 4. 截图

- `agent_pi_zcode.png`：主胶囊 = Pi（紫 π），分离小圆 = ZCode（蓝 Z）
- `agent_more_overview.png`：一览四个岛，两个 agent 各自的品牌图形与状态点
- `agent_zcode.png` / `agent_zcode_grab2x.png`：ZCode 展开态（4 条清单）+ 桌面实拍

### 5. 回归

构建全绿零警告：core 89 / rules 155 / render 25 / interaction 45 / **agent 119** /
hanzi 6 / cache 8，契约 7 项通过。

---

## 第二十轮：小圆点击路由 + 「一大两小」（2026-11-13 凌晨）

用户截图（主胶囊 = WorkBuddy agent，右侧小圆 = 腾讯视频）：「我点击旁边的小的会路由到大的的地方，
修复这个 bug，同时加上一个大的旁边有两个小的那种样式，最多就那样」。

### 1. 为什么点小的会展开大的

紧凑态只有一条点击路径：`WM_LBUTTONUP` → 不在展开态按钮里 → `g_click = true` →
主循环 `else if (demo.expand_id.empty()) demo.expand_id = visible.front().id`。

小圆画在胶囊右侧外面，**从来没有命中矩形**，所以点它和点胶囊空白处没有区别，
兜底分支就把主活动展开了。用户看到的「路由到大的」其实是「根本没识别到小圆」。

### 2. 改法

- `CapsuleView` 的 `detached_kind/cover/provider` 三个摊平字段换成
  `std::vector<DetachedItem>`，每项自带 `activity_id`、`state`（agent 状态点）。
- `Layout::kDetachedMax = 2`：主胶囊 + 两个小圆是极限，第三件之后只留 `+N`。
- 布局从「一个 46 宽的圆」改成「n 个 46 的圆 + (n-1) 个 8 的间距」，
  `detached_w` 仍是总宽（弹簧动画照旧）。
- 命中：`HitState::detached` 每帧用与 renderer **逐字相同**的公式算矩形，
  点击置 `g_detach_click`，主循环在 `g_click` 之前消费 —— 顺序反了会「点小的顺手展开大的」。
- 回归注入：`--click-detached=N`（与 `--click-at` 共用时间点，互斥）。

### 3. 实测

```
[t=+2.201s] inject click detached=0 pt=(694,78)
[t=+2.218s] click detached #0 -> expand live-agent-pi-...
```
```
[t=+2.212s] inject click detached=1 pt=(802,78)
[t=+2.230s] click detached #1 -> expand live-agent-zcode-sess-...
```

点第 1 个圆展开 Pi，点第 2 个展开 ZCode —— 主胶囊是 Claude，没有被误触。

### 4. 截图

| 文件 | 内容 |
|------|------|
| `agent_two_small.png` | 一大两小：Claude 主胶囊 + Pi（紫 π）+ ZCode（蓝 Z），两圆各带状态点 |
| `agent_four.png` | 4 件活动：主胶囊右侧 `2/4` + `+1` + 心跳，仍只有两个小圆 |
| `agent_click_small.png` | 点第 2 个圆 → 展开 ZCode 面板（不是 Claude） |

### 5. 回归

构建全绿零警告：core 89 / rules 155 / render 25 / interaction 45 / agent 119 /
hanzi 6 / cache 8。

---

## 第二十一轮：浏览器适配（2026-11-13 凌晨）

用户：「还有那些 app 没有适配，我看浏览器都没有适配，要给浏览器一个好的格式，各类的主流浏览器」。

### 1. 先承认「没有适配」是什么意思

浏览器此前只有一条存在路径：它播视频时 SMTC 会给一条「媒体」活动，岛上的样子和
QQ 音乐、腾讯视频完全一样 —— 同一个播放三角、同一行歌名，用户根本看不出那是在看网页。
这正是「没有适配」。

所以这一轮要的不是「加个浏览器图标」，而是**一类新的活动形态**：
它回答的是「我在哪个站、看的什么、还在下什么」，而不是「在放什么」。

### 2. 数据源：三条腿，每条都能降级

| 层 | 来源 | 断了会怎样 |
|----|------|-----------|
| 标签页标题 | `GetForegroundWindow` + `GetWindowTextW`，剥 ` - Google Chrome` 后缀 | 什么都不能显示（这层是所有浏览器的公约数） |
| 域名 | `History` 表的 `urls.title` 精确匹配，失败退到「最近 24 条里互相包含」 | 只剩标题，第二行空着 |
| 站点图标 | `Favicons` 表的 `icon_mapping` + `favicon_bitmaps.image_data` | 退成品牌色 + 品牌字形 |

下载进度来自 `downloads` 表（`state=0` 即进行中）。

全部只读。库在浏览器运行期间被独占，所以先用 `immutable=1` 打开（不打扰浏览器、
不加锁），失败才把文件拷到临时目录读副本。

`image_data` 是 BLOB，而现有的 `SqliteLite::Query` 按文本取值会把 PNG 里的 `\0`
截断 —— 用 `hex()` 转文本再自己解回来，比给 sqlite 层加一套 blob 取值分支省事。

### 3. 品牌识别：20 家

按**进程名**识别（`chrome.exe` / `msedge.exe` / `360se.exe` / `QQBrowser.exe` …），
认不出来再按**窗口标题后缀**兜底。标题后缀同时用于剥离：
只在「右段确实等于某个已知品牌名」时才剥，否则页面标题自己带的 ` - `
（`问题 - Stack Overflow`）会被剪掉一半。

Chromium 系还有 profile 发现：`User Data\Default` / `Profile 1` 里挑 History 最新的那个。

### 4. 布局

```
小岛： [站点图标] 当前标签页标题            62%
                 哔哩哔哩 · bilibili.com    ▬▬▬▬▬───

大岛： [图标] 完整标题
             https://...
             ─────────────────────────────
             TRAE_CN-Setup-x64.exe      62%
             ▬▬▬▬▬▬▬▬▬▬▬────────
             Microsoft Edge   点标题切回浏览器 · × 关闭
```

高度分两种：**没有下载时 92（矮面板）**，有下载时 `111 + n*26`。
第一版写成固定 118，没有下载时中间留了一条 33px 的空带，看着像没加载完；
分隔线也在暗示「下面还有东西」，而下面什么都没有 —— 现在只在真有列表时画。

### 5. 踩到的坑

1. **`msedgewebview2.exe` 把浏览器吃掉了**
   去重靠「媒体 app_id 含浏览器进程名」。WorkBuddy 这个 WebView2 壳播内嵌音频时，
   SMTC 的 app_id 正是 `msedgewebview2.exe`，含 `msedge` → 判定「Edge 在放媒体」
   → 浏览器岛被当成重复项丢弃。现象是「浏览器明明开着，却永远不出岛」，
   而日志里连一条 blocked 都没有（它是在映射层被 `continue` 掉的）。
   修法：`app_id` 含 `webview` 一律不算浏览器本体。

2. **玻璃捕获宽度估小了**
   `glass_w` 按 `kOverviewMax` 铺开估，实际画布把 margin 也算进了内容宽，
   差 70 逻辑像素 → 最右边那个岛没有纹理，是纯黑的。
   这是**第二次**因为「估宽度」翻车（第一次是加分离小圆）。改成按整个工作区宽度捕获。

3. **一览里浏览器岛只有图标、右边一片空**
   给 browser 分支写 chip 之后顺手 `return` 了，而 `DrawIsland` 的标题/副标题
   都在 chip 之后画。改成 if/else 只包住 chip 绘制。

4. **首次截图连续失败，日志只有 media 行**
   单实例互斥：前一个实例还没退（`--duration` 尚未到），新进程启动后发现自己
   是第二个，直接退出。表现是「命令跑完了但没有截图」。排查时先 `tasklist`。

5. **嵌套三元加到第四层，括号数不清**
   展开态高度判断原来是一个四层嵌套三元，加 `browser_lead` 时少一个右括号，
   编译器只报一行看不懂的错。改成平铺的 if/else。

### 6. 实测

真实会话（非强制模式，Chrome 在前台）：

```
browser: first poll -> 1 page(s);
[t=+0.65s] state=Minimal visible=3

小岛：[Chrome 图标] bilibili
                  哔哩哔哩 · bilibili.com
```

探针直读本机两家的结果（`browser_probe --brand=`）：

| 浏览器 | 标题 | 域名 | 图标 |
|--------|------|------|------|
| Edge | DeepSeek 开放平台 | platform.deepseek.com | 已落盘 32×32 PNG |
| Chrome | Google 翻译 - Chrome 应用商店 | chromewebstore.google.com | 已落盘（实为 Chrome 彩球，已放大确认） |

### 7. 截图

| 文件 | 内容 |
|------|------|
| `br_real.png` | 真实场景：Chrome 在前台看 bilibili，小岛 = 站点图标 + 标题 + 域名 |
| `br_expanded.png` | 展开态（无下载）：标题 → URL → 页脚，矮面板 |
| `br_dl.png` | 展开态（两条下载）：文件名 + 进度条 + 百分比 |
| `br_dl_compact.png` | 小岛带下载：右侧 62% + 进度条 |
| `br_overview.png` | 一览 4 个岛：浏览器 + 腾讯视频 + 两个 agent |

### 8. 回归

构建全绿零警告：core 89 / rules 155 / render 25 / interaction 45 / agent 119 /
**browser 28** / hanzi 6 / cache 8。
新增 `probes/browser_probe.cpp`（`--check` 列已装浏览器、`--front` 看前台判定、
`--brand=` 单独验证采集链路）与 `tests/cpp/browser_tests.cpp`。
新增注入参数 `--browser-brand=` / `--browser-fake-dl=N`（后者用于截「有下载」的布局，
真实下载不可控）。

---

## 第二十二轮：设置面板重做 + 透明度/位置 + 会议适配（2026-11-13 凌晨）

用户：「优化设置栏，使其看起来和灵动岛统一风格，同时增加功能，可以更改透明度，
当前为均衡，设置全透明和不透明两档，再增加一个功能，可以选择灵动岛的位置，
有三个选项居中、靠左、靠右，注意适配，不要越界，然后适配你认为需要适配、
能适配的软件，进行调试之后，汇报给我」。

### 1. 设置面板为什么必须自绘

原来是「系统灰底对话框 + groupbox + 系统复选框」。它作为工具完全够用，但和顶上那颗
磨砂玻璃胶囊是两套视觉语言 —— 打开设置像打开了另一个软件。

更关键的是：这次要加的两个功能（透明度档位、停靠位置）**本身就是视觉选择**。
用系统控件只能做成两个下拉框，用户选「全透明」时完全不知道会发生什么。
自绘可以把效果直接画进选项里（见 `Segment` 的 `SegIcon`）。

代价：自己处理命中、悬停、拖窗口、Esc/Enter。加起来不到 100 行，
换来零原生控件。**黑名单的两个多行文本框因此退出面板** —— 深色自绘里塞原生 EDIT
会立刻露馅。它们移到「打开配置文件」，本来就是低频操作。

### 2. 透明度三档

| 档 | 画法 | 适合 |
|----|------|------|
| 0 全透明 | 本体一个像素都不画（连边框和投影也不画），文字加两层黑投影 | 深色壁纸 / 录屏 |
| 1 均衡 | 磨砂玻璃（`glass=false` 时深色实心） | 默认 |
| 2 不透明 | 永远深色实心 | 浅色壁纸 / 要最高可读性 |

全透明刻意不画描边：留一圈细边会在深色壁纸上显出一个"空壳"，比不画更怪。

第一版文字投影只有一层 55% 黑影，实测在浅色桌面上仍然发飘、读长标题吃力。
改成两层（贴边 85% + 偏移 2 倍 45%），有字幕感，浅底也可读。

### 3. 位置三档

`anchor_x(width)` 一个函数算三种情况，并且**一律夹在工作区内**：
靠右时如果岛比工作区还宽（4 个岛铺开在窄屏），右对齐会把左边推出屏幕 ——
用户就只能看到半个岛、还点不到关闭按钮。那种情况直接贴左边。

靠边留白 28px（`kMargin` 尺度），三档都按 `rcWork` 算，不按屏幕算 ——
否则任务栏在左侧的机器上会被挡住。

### 4. 会议适配

选它是因为它是「看不见就得切窗口确认」的典型，而且**有可靠判据**。

不看窗口标题：Zoom / 腾讯会议「开着客户端」和「正在会中」的标题几乎一样。
真正的判据是**麦克风占用** —— 枚举默认麦克风上 `AudioSessionStateActive` 的会话，
拿到 PID 再看 exe 名。只知道「谁在用麦克风」，不碰音频内容。

- 开着客户端但已离会 → 没有 Active 会话 → 不出岛；
- 切共享屏幕导致会话短暂中断 → 25 秒宽限，岛不会反复闪；
- 支持腾讯会议 / VooV / Zoom / Teams / 钉钉 / 飞书 / WeLink / Discord。

### 5. 自检

`--settings --selftest` 现在验两件事：

```
selftest: zones=25 unclickable=0 clamp=opacity2/anchor0
```

- **每个可点区域的中心必须命中它自己**。这条抓的是「被拖动区盖住」「区域重叠」
  「矩形算反了」—— 它们在用户眼里只是「这个开关点不动」，靠截图永远看不出来。
  写完立刻发现：`ZoneAt` 正序遍历时，整块标题拖动区会先命中，× 按钮永远点不到。
- **越界值要被夹住**：配置里写 `opacity=9 / anchor=-3`，读回来必须是 `2 / 0`，
  而不是整块外观回退默认。

### 6. 踩到的坑

1. **命中表正序查**：× 被拖动区吃掉 → 改倒序（后加入的更具体，优先）。
2. **紧凑态通用布局只画一行标题**：媒体靠右侧时间和进度条表达信息，
   所以通用路径从来没画过副标题。会议直接套上去只剩「腾讯会议」四个字。
   补了 `DrawCompactMeeting`（两行 + 右侧呼吸红点）。
3. **`DrawGlyph` 加了分支却不生效**：它带默认参数 `provider`，我按记忆里的
   四参数签名写替换，匹配失败而脚本静默通过 —— 编译通过、运行时还是画成圆点。
   教训：改签名敏感的函数，替换断言必须带完整签名。
4. **免打扰六个控件挤在一行**：开关 − + 到 − + 贴着，时间文字被压成零宽。
   拆成两行（标题+开关 / 起止时间）。
5. **链接失败 LNK2019 缺 shell32**：不是参数问题，是上一个实例还占着 exe。

### 7. 截图

| 文件 | 内容 |
|------|------|
| `settings.png` | 设置面板整体（深色、圆角、分段带示意图、开关、免打扰两行） |
| `vis_grid.png` | 3×3：三档透明度 × 三个位置 |
| `vis_pos.png` | 桌面实拍：靠左 / 居中 / 靠右 + 全透明 / 不透明 |
| `visg_o0_2x.png` | 全透明放大：背景完全消失，文字带投影仍可读 |
| `meeting.png` | 会议岛：红色话筒 + 腾讯会议 + 正在开会 · 12:35 + 呼吸红点 |

### 8. 回归

构建全绿零警告：core 89 / rules 155 / render 25 / interaction 45 / agent 119 /
browser 28 / hanzi 6 / cache 8。设置自检 25 区域全可点、夹取正确。
新增注入参数 `--opacity=` / `--anchor=` / `--meeting=`（都只用于视觉回归）。

---

## 第二十三轮：液态玻璃 + 删掉操作提示（2026-11-13 清晨）

用户截图（一行「点标题打开应用 · 拖进度 · × 关闭」）：「这几个文字好像没啥意义啊，
还有就是这个全透明太夸张了。应该是那种液态玻璃效果」。

### 1. 操作提示为什么是噪音

「点标题打开应用 · 拖进度 · × 关闭」这句话把三件**用户本来就会做**的事写了一遍：

- × 就在右上角，看得见；
- 进度条本身就长得像能拖的；
- 标题可点这件事，第一次用过就知道，第 100 次看它只是一行灰字。

而它占的恰好是展开态最贵的一行 —— 页脚。三处全部删掉（媒体 / 浏览器 / agent），
页脚只保留有信息量的内容：`03:12 / 04:15`、`Microsoft Edge`、`gpt-5-codex · 143 步`。

顺带把媒体页脚的 `meta` 宽度从 58% 放到 100%，反正右边空出来了。

### 2. 「全透明」错在哪

第一版这一档的语义是「一个像素都不画，只留文字 + 双层投影」。截图里看到的是几行白字
浮在壁纸上 —— 没有材质、没有边界，浅色壁纸上还发虚。

**透明度的反义词不是"没有"，是"更薄"。** 液态玻璃（iOS 26 那套）的本质是：
玻璃依然在，只是薄到你能看清背后，而它的存在感完全由**结构**提供 ——
边缘折射的亮环、顶部那道镜面线、左上角聚过来的反光、底边的一点点厚度。

### 3. 三次失败的过程（都留在代码注释里）

**第一次**：把染色从 0.70 降到 0.32，其余不动。
→ 浅米色壁纸被压成一片均匀的灰，像灰色塑料。

**第二次**：染色降到 0.18、改冷蓝调，加强左上反光。
→ 更灰了。采样发现胶囊内 (194,195,202) 是中性灰，而壁纸是暖米 (227,208,192) ——
色相被彻底洗掉了。

**第三次**：把所有高光改成中性白（不再有蓝分量），染色降到 0.13。
→ 暖色回来了（内部 R>G>B），但顶部受光铺满 52% 高度，把中段又去饱和了。
把光斑 focus 收到 0.30/0.22、受光带收到 28%，才稳定下来。

真正的根因在 `backdrop.hpp`：`Desaturate(small_, 0.35f)` —— 均衡档为了让背后的彩色
窗口不抢内容，把整个背景去饱和 35%。暖米色进去，出来就是灰的，**再怎么调染色都救不回来**。

修法：backdrop 出两份纹理。
- `small_`：3 趟 ×14 半径模糊 + 去饱和 35%（均衡档）
- `lite_`：降采样后仅 1 趟 ×9 模糊 + **不去饱和、反而提饱和 20%**（液态玻璃档）
  玻璃聚光，透过去的颜色本来就比背景更浓。

### 4. 参数最终值

| 层 | 值 | 说明 |
|----|----|------|
| 染色 | 0.09 · (20,21,26) | 中性深色，只压一点点 |
| 左上反光 | 0.17 中性白 · focus 0.30/0.22 | 光斑聚在角落，不铺满 |
| 上半受光 | 0.15 → 0 于 28% 高度 | 凸面玻璃的顶部受光 |
| 底部压暗 | 0.06 | 比均衡档（0.20）淡得多，否则上轻下重像砖 |
| 折射环 | 0.46 内缩 2px，线宽 0.9 | 0.62 会变成硬白描边（霓虹灯管） |
| 外缘暗线 | 0.26 | 浅壁纸上"没边"是最致命的 |
| 投影 | 实心款的 55% | 有影才浮得起来，太浓会拖灰雾 |

实测采样（1200×420 抓图）：

```
壁纸      (246,240,239)
胶囊内    (205,187,171)   ← 暖调保留，R>G>B
```

### 5. 设置面板

- 文案：`透明度` → `材质`；`全透明` → `液态玻璃`
- 说明：`液态玻璃最透，均衡最稳，不透明任何壁纸下都读得清`
- 图标：原来是空心方块（读成"什么都没有"），改成浅透底 + 亮边；
  且**选中格是白底，浅色图标会消失** —— 按 `on` 反色

### 6. 实测

| 文件 | 内容 |
|------|------|
| `liquid_2x.png` | 紧凑态液态玻璃：透出暖色壁纸、边缘亮环、文字带投影仍清晰 |
| `liquid_exp_panel.png` | 展开态（带两条下载）：材质明显，页脚只剩 `Microsoft Edge` |
| `mat_cmp.png` | 三档对比：液态玻璃（浅透）/ 均衡（深磨砂）/ 不透明（纯黑） |
| `set_mat.png` | 设置面板「材质」三个图标都可见 |

### 7. 回归

构建全绿零警告：core 89 / rules 155 / render 25 / interaction 45 / agent 119 /
browser 28 / hanzi 6 / cache 8。
设置自检：`zones=25 unclickable=0 clamp=opacity2/anchor0`。

---

## 第二十四轮：编辑器适配 + 可传播（2026-11-13 上午）

用户：「网盘同步…一直进行你觉得需要做的东西，然后把这个做成可复用、可传播、
方便部署完整的项目，提交到 github」。

### 1. 为什么没做网盘，改做编辑器

先查本机：OneDrive 只剩一个空目录（`OneDrive.exe` 都没装），百度网盘只有
`BaiduYunKernel/Config/*.s3dbe`（自研私有格式，不是 SQLite）。**没有正在运行的实例
就没有可验证的数据源** —— 而「写一个读不到东西的采集器」正是这个项目一直在避免的事。

改做**编辑器当前编辑的文件**，理由：
- 本机 Trae CN 正在运行，链路可实测
- 数据源只有窗口标题，零文件 IO、零权限
- 它和 agent 岛互补：agent 是「AI 在改」，它是「我在改」

### 2. 标题解析是这个功能里唯一的难点

窗口标题是人写的字符串，三种分隔符（`-` / `–` en dash / `—` em dash），五种形态：

```
renderer.hpp — DynamicIsland-Win — Trae CN        有工作区
● app.ts — web — Cursor                           有未保存修改（VS Code 系）
*main.cpp – order-service – IntelliJ IDEA 2024.1  JetBrains，星号 + 版本号
README.md - Notepad++                             无工作区
hosts - Notepad++ [Administrator]                 管理员标记
```

规则是**从右往左剥**：最后一段必须能匹配品牌表（否则整句都不认），倒数第二段是项目，
剩下全是文件名（文件名自己可能含 ` - `）。`editor_tests` 59 项断言钉住这些。

两个真踩到的坑：
1. **`Trae CN` 要赢过 `Trae`** —— 品牌表按匹配长度取最长，否则中文版会被判成国际版。
2. **`VS Code` vs `Visual Studio Code`** —— 官方标题是后者，有些发行版写前者。
   `title_tail` 因此支持 `|` 分隔多候选。

### 3. 权重与「不出岛」的克制

编辑器权重 **56**：低于媒体（60）和浏览器前台（64），高于等待输入的 agent（50）。
理由：正在编辑是**持续状态**，不是事件；但那确实是用户眼睛盯着的窗口。

**只在前台是编辑器时出岛**，切走 2.5 秒收起。编辑器一开就是一整天，常驻就变成噪音了。

### 4. 工程化

| 新增 | 作用 |
|---|---|
| `tools/build.ps1` | 裸 PowerShell 就能编（不需要开 VS 命令行）。工具链路径从 `vswhere` + 注册表推导 —— 本机 SDK 在 D 盘，硬编码 `C:\Program Files (x86)\Windows Kits\10` 会直接失败 |
| `tools/install.ps1` | 装到 `%LOCALAPPDATA%`、建开始菜单快捷方式、可选 `-Autostart`、`-Uninstall` 卸载（配置默认保留） |
| `tools/publish.ps1` | 设 remote + 推 main，支持 `owner/repo` 简写 |
| `.github/workflows/ci.yml` | **改成跑 `build.ps1`** —— 早期 CI 走 CMake、用户走 bash 脚本，两条路径必然分叉，表现是「CI 全绿但 clone 下来编不过」 |
| `docs/images/` | README 截图（8 张） |
| README | 重写开头：三十秒安装 + 能显示什么 + 材质 + 交互 + 隐私；技术细节挪到后面 |

`build.ps1` 自己踩了两个坑：
- 变量名不能叫 `$args`（PowerShell 自动变量），覆盖后 splat 出去是空数组，
  cl.exe 每个目标都回「缺少源文件名」，看起来像编译器坏了
- `@('-Fo' + $obj, ...)` 在数组字面量里做拼接会被逗号优先级拆开 —— 改逐项 `+=`

### 5. 实测

真实链路（Trae CN 未打开工作区时标题只有 "Trae CN"）：

```
editor: foreground=Trae CN title=Trae CN → 不出岛（不是编辑器或无障碍信息）
```

注入链路（`--editor="Trae CN|*renderer.hpp - DynamicIsland-Win - Trae CN"`）：

```
editor: Trae CN 「DynamicIsland-Win」 renderer.hpp (C++)
```

| 文件 | 内容 |
|------|------|
| `editor_2x.png` | 紧凑态：`< >` 图标 + renderer.hpp + `DynamicIsland-Win · C++` |
| `editor_dirty_2x.png` | 带未保存圆点 |
| `editor_exp_crop.png` | 展开态：文件名 + ● → 项目 · 语言 → 页脚 Trae CN |

### 6. 回归

`build.ps1` 一键跑完：89 / 155 / 25 / 45 / 119 / 59 / 28 全绿，零警告。
