# 全网同类项目调研与四维对比结论

> 调研日期：2026-11-14 · 服务于 `balance_island 平衡岛` v0.2.0 的更名与范式定义
> 覆盖代码托管、设计社区、产品展示、技术论坛、官方规范与上位理论六类来源。
> 结论摘要见本文 §7；落地项见 [改进项清单](../IMPROVEMENTS.md)。

---

## 1. 调研范围与核实口径

| 来源类型 | 具体平台 | 结果 |
|---|---|---|
| 代码托管 | GitHub（主战场）、Gitee、GitLab.com、Codeberg.org、SourceForge、git.hiwiki 等镜像/聚合站 | 20 个主项目核实，登记约 28 个条目；GitLab/Codeberg 未检索到同类活跃项目（如实标注） |
| 应用分发 | Microsoft Store、厂商官网 | Halo / WinIslands / Notchify / TopNotch 等；闭源与付费集中地 |
| 设计社区 / 产品展示 | Dribbble、Behance、Awwwards、站酷、Product Hunt、Mobbin、UI 中国 | 概念设计与产品条目；Apple HIG 官方页单独核实 |
| 技术论坛 / 媒体 | Windows Central、少数派、V2EX、Reddit、Hacker News、DEV、Medium、The Verge、Stack Overflow、CSDN | 24 条讨论 / 评测，六大议题 |
| 官方规范 | Apple HIG（Live Activities / Widgets）、MS Learn（Widgets / Badge / Fluent 动效）、GNOME HIG、W3C WCAG 2.2 | 数值均标注核实等级 |
| 上位理论 | Calm Technology、Ambient Displays、Glanceable Displays、Progressive Disclosure、Bounded Deferral | 119 条可迁移规则（R1–R119），其中 15 条为关键约束 |

**核实标记**：【核实】实际打开原页读到正文；【摘录】官方页正文由搜索直接返回；
【二手】博客 / 源码级解析 / 媒体转载；【未核实】查不到即留空，不编造 star 数、
许可证、功能或引用。**疑似 AI 生成的内容农场已排除**（如 tsight.io 的虚构
「微软把灵动岛搬进任务栏 / TaskbarCore.dll」），不作论据。

一个必须诚实交代的缺口：**Hacker News 上本议题客观稀疏**。用 Algolia API 实查
20 条 story，最高 44 分 / 9 评论，桌面端批评贴为零，唯一相关的是
Show HN: Crest（把 Claude Code 审批放进 MacBook 刘海，6 分 0 评论）。
真正的批评集中在媒体评测、设计博客、少数派、V2EX 与 Reddit。知乎 / 机核 /
远景论坛本轮未命中可核实页面。

---

## 2. 生态时间线：是断层，不是传承

| 阶段 | 事实 |
|---|---|
| 2021-10 | V2EX《做了个 Windows 上的刘海》（scung，约 7500 浏览 / 12 回复）：**闭源、无仓库、无 fork、OneDrive 链接已全部失效**，进程名 `Bangs`。最集中的差评是「咋关掉」，作者只能教用户去任务管理器杀进程 |
| 2022 | Apple Dynamic Island 发布；少数派《事后诸葛：灵动岛为什么不好?》与 The Verge「手势交互 backwards」大讨论 |
| macOS 源头 | NotchNook → **DynamicWin README 明确自承灵感来自 NotchNook**；Lost Island 有 macOS / Linux Sway 移植谱系 |
| 2026 | 开源潮集中爆发：AI 编码 Agent 会话上岛、液态玻璃材质、Tauri/Rust 原生路线。约 28 个条目，GitHub 一家独大 |

**结论**：2021 的孤例与 2026 的开源潮之间无代码传承。可追溯的 Windows 一脉源自
macOS 刘海生态 + 2026 的 AI Agent 需求。本项目更名与范式定义因此**没有历史包袱**，
也不必沿用「刘海 / notch / 灵动岛」的语义。

---

## 3. 技术路线与代表项目（六类）

| 路线 | 代表 | 取舍 |
|---|---|---|
| A. 原生 C++ / Win32 / Direct2D（**本项目在此**） | lost-island-windows、Bunbun、devcode90(C++23 Windhawk)、NotchPeninsula(Win32+SkiaSharp+NAudio) | 单 exe、无运行时、30MB 内存 / 1–3% CPU、最高 360–500Hz、透明像素点击穿透；开发慢、star 偏低 |
| B. .NET / WPF / WinUI | WinIslands、BrimDeck、Halo、Drey0911、islebar | XAML 动效成熟、SMTC/WinRT 接入方便、可上 Store；绑定 .NET 8/9/10，包体大 |
| C. Tauri 2 + Rust + WebView2（数量最多） | Nectar、Fly Island、HeyyCzer、Mintu、quiera-island、windows-dynamic-island | UI 迭代快、包体可压到 5MB；每窗口独立 WebView2，多屏内存翻倍 |
| D. Electron / Python | Islet、OpenCode Notch、PyIsland、ItsMarekChinoy | 跨平台、易改；体积/依赖/常驻开销大 |
| **E. Rust + 原生 GUI（2026 增长最快）** | Lumen(Rust+Slint)、岛仔(Rust+egui)、Nusa | 无 WebView、无浏览器运行时、单机原生渲染；介于 A 与 C 之间 |
| F. 配置驱动 / 皮肤 / 上架闭源 | Rainmeter `.ini`、Windhawk mod、js-dynamic-island、Store 付费工具 | 零编译可改；能力受宿主限制，多屏/DPI 脆弱 |

代表性项目（完整 URL 与核实状态见 §9）：

- **NotchPeninsula** https://github.com/GEORGEWWWU/NotchPeninsula 【核实】Apache-2.0，
  Win32+SkiaSharp+NAudio+.NET 10，v2.2.2 仍活跃。**全生态最完整插件系统**
  （DLL 丢进目录自动加载 + `plugin.json` + 热重载 + 五类扩展点 + 插件市场）
  与 **MSP 本机节点**（外部程序可 ping / 弹通知 / 查询媒体 / 控制播放）。
- **WinIslands** https://github.com/JudeKwong/WinIslands 【核实】MIT，.NET 8 + WPF。
  `Island API` 本地 HTTP + WebSocket，v1/v3 双版本、心跳续期、优先级队列，
  明确承诺「推送不改变岛宽」；本地组件插件用独立 `.exe/.ps1` 进程隔离崩溃。
- **Lost Island** https://github.com/byte4day/lost-island-windows 【核实】MIT，
  C++/Win32/Direct2D，单 exe。**Live Activities 优先级模型**：主活动填满胶囊、
  次要活动变独立圆泡；手写 WinRT ABI；服务按 `src/services/*` 一一对应。
- **Nectar** https://github.com/kua8/Nectar 【核实】GPL-3.0，243★。镜像真实任务栏
  固定应用与顺序；「读不到电池就别显示假 100%」；明确无插件系统/公开 API；
  自认多屏每窗口一个 WebView2 导致内存翻倍。
- **岛仔 Daozai** https://github.com/jilespace/jilespace-daozai 【核实】MIT，37★，
  Rust+egui，645 测试。明确区分「已真实账号实连测试」与「仅验证代码未实连」。
- **Bunbun** https://github.com/iaryasharma/Bunbun 【核实】C++20+Win32+Direct2D，
  约 30MB 内存 / 空闲 1–3% 单核；Agent hook「先备份、只加自己条目、关闭即失效、
  绝不代批准」。
- **Fly Island** https://github.com/flyl1u/fly-island 【摘录】MIT，Tauri 2，
  5.2MB NSIS 包；主动掐掉「读微信正文」这条能力边界。**更名反面教材**：从 FocuSD
  改名后数据目录仍是 `com.focusd.island`、脚本仍是 `focusd-agent-status.ps1`。
- **Developer-Island** https://github.com/maximilian467/Developer-Island 【核实】MIT。
  380+ 测试放在无 UI 项目；**snapshot 模式用离屏 PNG + 假数据渲染所有 UI 状态**，
  README 截图由此生成，刻意不抓真实屏幕。
- **OpenCode Notch** https://github.com/fraches05/opencode-notch 【核实】MIT。
  「only gets loud when it needs you」；岛上直接批准/拒绝权限请求；
  **窗口尺寸不可由内容派生**（`100vw` 把展开面板裁成 208×20 的经典坑）。
- **Halo** https://github.com/phoseinq/Halo 【核实】MIT，.NET 9。
  「Any tool can join by writing a small JSON file」——无端口、无守护服务。
- **PrismGlass** https://github.com/liburce0412-alt/prismglass-desktop 【核实】MIT。
  九张桌面卡片**默认一个都不加**，「默认状态不改变任何东西、退出即回普通桌面」。
- **DynamicWin** https://github.com/FlorianButz/DynamicWin 【核实】README 原文
  「only exists to host the releases. There is no source code here」——
  **严格说不算开源项目**，不计入开源数；但它是「灵感来自 NotchNook」的书证。

另有 BrimDeck、HeyyCzer、Drey0911、Mintu、PyIsland、Rainmeter 皮肤、
Gitee `go-dynamic-lsland`（0★、停更）、SourceForge 目录条目等，见 §9 清单。

---

## 4. 四维对比

### 4.1 功能覆盖 / 数据来源

**「真实系统活动」是这个赛道的立身之本**：几乎所有项目都强调「不造假数据」
「读不到就显示真实状态」。媒体统一走 **SMTC / Windows 全局媒体会话**
（+ 窗口标题兜底 + 网易云/QQ 音乐特化）；脚本注入只用于两类：
① Agent 会话（Claude Code hooks / statusline / transcripts / 写 JSON 文件）
② 第三方推送（HTTP/WS/文件/IPC）。

| 能力 | 同类覆盖 | 本项目 |
|---|---|---|
| 媒体 + 走带控制 | 全部 | ✅ 且接真实 SMTC |
| 编码 Agent 会话 | 2026 新标配（Crest / OpenCode Notch / Developer-Island / BrimDeck / Halo / Lost Island / Bunbun） | ✅ 多编辑器/Agent |
| AI 额度看板 | **已拥挤**（至少 4 家） | ❌ 刻意不做，避免同质 |
| 浏览器下载进度 | 少数（Halo / Lost Island / Drey0911） | ✅ |
| 编辑器当前文件 | **几乎无人作为一等实时活动** | ✅ 差异位 |
| 会议状态 | 零星（WinIslands 静音助手 / Bunbun 会前自检） | ✅ 差异位 |
| 剪贴板 / 电量 / 通知 | 人人都有 | ✅ |
| 剪贴板管理器、待办、翻译、播放器列表 | 多数项目膨胀方向 | ❌ 明确不做（范式边界） |

**结论**：项目「真实系统活动聚合的完整度」在同类里几乎无人同时做全；
而「AI 额度看板」已拥挤。差异化应落在前者 + 低打扰 + 工程完成度，
而不是再做一个额度条。

### 4.2 交互逻辑

**已趋同的事实标准**：hover 展开 / 离开或 Esc 收起、全屏与锁屏自动隐藏、
托盘常驻、`Ctrl+Alt+…` 全局快捷键、解锁拖动 + 边缘吸附、文件中转站（只存路径）、
滚轮切页、`Notch / Capsule / Indicator` 三收起态。

| 关键差异 | 同类 | 本项目 |
|---|---|---|
| 点击语义 | 多数「点击 = 展开面板」；把「点击回原应用」做成跨所有类型的一等语义**仍无人做全**（NotchPeninsula 只对媒体/通知做了） | ✅ 媒体→播放器、下载→浏览器、会议→客户端、文件→编辑器、Agent→终端 |
| 让路 / 穿透 | 多为**设置项**（BrimDeck 鼠标穿透模式、Nectar Smart/Peek、Lumen idle click-through、NotchPeninsula 穿透模式） | ✅ **按住 Ctrl 即时让路**，不需进设置——范式独有语义 |
| 自动隐藏粒度 | NotchPeninsula 三模式可任意组合（焦点离开 / 暂停播放后 / 全屏），关键内容期间禁止隐藏；出厂不注册热键 | 🟡 全屏退让已有；其余待补（见改进清单） |
| 安静态 | 多数仍是胶囊；少数收成细线 | ✅ 呼吸点（「可确认存在但不常驻占用」） |
| 多显示器 | **全行业持续痛点**：Nectar 内存翻倍、Lumen 列为未来改进、Notchify 5.0 专修、Rainmeter 要外挂脚本 | ❌ 仅主屏（issue #3，待补） |
| 键盘可达 | WinIslands 全套热键、OpenCode Notch `Ctrl/Cmd+K` 命令面板 | ❌ 无障碍不达标，待补 |

### 4.3 视觉表达

**已趋同**：弹簧物理、60fps（高端做到 360–500Hz）、专辑封面取色、
液态玻璃 / 毛玻璃 / Acrylic / Mica、Auto/Light/Dark、Reduce Motion。

三条更正后的重要判断（避免高估空白位）：

1. 原生自绘渲染派**已有人做到完整三材质**：NotchPeninsula 有「实体 / 亚克力 /
   液态玻璃」+ 玻璃质量四档 + 12 项参数。空白位应缩小为
   **「纯 C++20 + Win32/GDI+ 技术栈里，三材质 + 深浅色 + 可调玻璃参数仍无先例」**。
2. **「玻璃 ≠ 必然卡顿」**，社区已有成熟解法：DXGI Desktop Duplication（静止零开销）、
   **抓屏放后台线程**、内容哈希跳过、仅内容变化时 4Hz 重算、失败自动降级纯色、
   抓屏瞬间自隐身约 28ms（不把自己拍进去、截图录屏照常拍到岛）。
   NotchPeninsula 实测折叠态约 3ms / 展开态约 27ms；Notchify 把 idle CPU
   从约 56% 修到约 6%，并消除了「system-wide keyboard lag」。
3. 材质分档的社区标准答案：**低档纯色/静态纹理（0 抓屏）→ 中档 Mica 式一次采样
   → 高档 Acrylic 式实时模糊（仅限独显/插电/用户显式开启）**；
   省电模式自动降级，全屏白名单关闭玻璃。

本项目的双层描边（整圈均匀淡描边 + 顶部高光带、禁止整圈等高亮）、
按高度分档的展开尺寸（80/124/196，按内容而非统一大容器）是差异位；
但**抓屏仍在主线程**是明确短板（issue #1 已缓解但未根治）。

### 4.4 扩展能力

第三方接入方式现已列全为 **7 类**：HTTP/WS localhost（WinIslands、HeyyCzer）、
写 JSON 文件（Halo、Lost Island hook）、Claude hooks/statusline、命令行参数、
**直连本机既有服务**（OpenCode Notch）、**MSP 本机节点 + 工具树**（NotchPeninsula）、
**命名管道**（本项目，全生态检索下来是唯一）。

| 机制 | 评价 |
|---|---|
| 命名管道 IPC（本项目） | 零端口、无 CORS/防火墙/浏览器依赖、任意语言三行代码；但只是**点对点管道**，对标 MSP 的「本机节点 + 双向事件订阅 + 可发现工具树」仍有差距 |
| NotchPeninsula 插件系统 | 全生态最完整：manifest + 影子拷贝热重载 + 五类扩展点 + 插件市场；短板是插件与宿主版本强耦合、分发渠道不开放 |
| WinIslands 独立进程插件 | 崩溃隔离最干净 |
| Rainmeter 分层扩展 | INI（低门槛）→ Lua 脚本 → 原生 DLL，社区 20 年验证「分层 + 市场 + 文档」最被认可 |
| PowerToys 工程范式 | Runner 单实例 + 模块 DLL + **命名管道 JSON 双向消息**连独立设置进程；per-user 配置；用户态、不装驱动、不注入系统进程 |

社区更认可「**分层 + 市场 + 公开协议**」而非单一 IPC；但也有
（Lumen、OpenCode Notch）明确选择「极简内核 + 清晰边界」。
本项目按范式边界选择后者：公开 IPC 协议，不内置插件市场。

---

## 5. 社区批评：为什么必须脱离「复刻」

| 批评 | 出处 | 对范式的约束 |
|---|---|---|
| 「灵动岛是为了遮丑设计的」「无任务时黑色胶囊仍常驻，违背低打扰」 | 少数派《事后诸葛》【核实】 | 安静态必须存在；每个数据源可单独关；默认偏向隐藏 |
| 「A pointer is not a thumb」「Media keys beat hovering an island. Every time」「把整个 iPhone 行为搬过来 **feel like a costume**」 | Droppy 官方博客【核实，作者利益相关已声明】 | 桌面原生交互优先；不模仿拇指触达；价值锚定「固定地址 + 一瞥即得」 |
| 「tapping … just switches you back … To get the expanded widget you have to tap and hold. **This feels exactly backwards**」 | The Verge / Nilay Patel，经 mjtsai 汇总【核实】 | 交互语义要可配置、不做隐式手势；默认选最直觉的 |
| 「the island is intrusive … marketing trick to justify a hardware compromise」「Windows 11 doesn't wrap around the app」 | Windows Central Notchify 评测【核实】 | 全屏冲突是结构性缺陷，必须默认退让；位置/显示器要可选 |
| 「最烦的不是切窗口…一个下午切了四十次」「少切一次窗口」 | 微信公众号 Fly Island 长文【摘录】 | 产品叙事用「少切一次窗口」，不用「更炫的岛」 |
| 「competes for attention」「too many modules … another notification area to manage」 | Position is Everything【核实】 | 默认少模块、只显示当前最要紧的一件、可一键退回 |
| 「算不算没苦硬吃」「实用性不强」 | 少数派评论区 / 招财猫论坛【快照】 | 留存靠每天真会用到，不靠动效 |
| 2021 V2EX「咋关掉」 | V2EX 镜像【核实】 | 托盘 + 明确退出 + 开机自启可关是**不可省**的一等交互 |
| 「好的不学!不学好的!」「要不把刘海做在底下」 | 同上 | 顶部常驻与「任务栏在下方」的心智冲突真实存在；提供退路 |

---

## 6. 上位理论与官方硬约束（可写成断言的部分）

理论内核是 **Horvitz 的 bounded deferral**：论文原题即
*Balancing Awareness and Interruption*——「平衡」是可工程化的目标函数，不是隐喻。
配套：Calm Technology（Weiser & Brown, 1995）8 原则、Ambient Displays（CHI'98）、
Glanceable Peripheral Displays（DIS'06）、Progressive Disclosure（Xerox Star 1982 /
Nielsen 2006）、Notification Fatigue / Attelia breakpoint 投递。

已核实的官方数值（节选，完整 R1–R119 见调研原始交付）：

| 来源 | 数值 | 本范式采用 |
|---|---|---|
| Apple HIG Live Activities | 任务 ≤8h、动画 ≤2s、Minimal 同时 ≤3、受限宽度 28pt 圆、Expanded 84–160pt、字重 ≥medium | 只作参照；**不照搬**（见 §4.3 与 PARADIGM.md 的历史说明） |
| WinUI3 Timing & easing | `ControlNormal=250ms`、`ControlFast=167ms`、Enter decelerate / Exit accelerate | ✅ 进入 250 / 退场 167，入慢退快 |
| WCAG 2.2 SC 1.4.3 / 1.4.11 | 正文 ≥4.5:1、大文本 ≥3:1、非文本 ≥3:1；阈值**不得四舍五入** | ✅ `contrast_min=4.5` |
| MS Learn Badge | 数值 1–99、>99 显示 99+、0 清除，glyph 为封闭枚举 | 待补（未做徽章） |
| MS Learn Widget providers | provider 生命周期与显示单元解耦、启动从持久化状态恢复、回调返回后不持有对象 | ✅ Provider 接口隔离 |
| Win11 shell 边界 | **无受支持的第三方任务栏 UI 扩展点**；未文档化注册表值不可写 | ✅ 禁注入 `explorer.exe`，独立 topmost 窗口 |
| 全屏检测 | `SHQueryUserNotificationState` 三 busy 态 OR 前台窗口矩形==所在屏 `rcMonitor`；轮询约 800ms | 🟡 已有基础，待补三态与逐屏 |
| 多显示器 | 虚拟屏幕坐标系允许**负坐标**；Per-Monitor V2；`GetDpiForWindow` 逐屏；禁用 `GetDesktopWindow()` 判全屏 | ❌ 待补 |

---

## 7. 结论：空白位与本项目的独立定位

### 7.1 现有品类占住的四个角

「全给」（Rainmeter / Übersicht，无限自由零节制）、
「全藏」（Bartender / Ice，减法与遮挡规避）、
「系统限定」（Live Activities / Widgets，有节制零调节）、
「随便改」（ExplorerPatcher / TaskbarX，注入 shell 但脆弱）。

### 7.2 三个明确未被占据的交叉点

1. **有节制的可编程常驻态** —— 允许外部定义内容，但层级 / 并发 / 对比度 /
   动效由系统常量硬锁
2. **持续状态（无始无终）** —— Live Activities 只服务 ≤8h 事件，Widgets 明确
   不支持连续实时更新；「持续存在、低幅度波动的状态」无对应品类
3. **把「显示多少」本身当作被设计的量** —— 现有工具都把显示量当用户配置项，
   而非系统核心不变量

### 7.3 平衡岛如何占据

以 bounded deferral 为理论内核，把平衡写成可验证守恒断言：

```
attention_cost   <= CAP_A     （注意力不被透支）
information_loss <= CAP_I     （信息不丢失）
```

- **不是复刻**：不模仿挖孔、不模仿拇指触达、不把岛做成应用启动器；
  桌面原生交互（媒体键、任务栏、托盘）优先。
- **真实系统活动聚合**是差异位（媒体 / 下载 / 会议 / 编辑器文件 / 剪贴板 / 电量），
  避开已拥挤的「AI 额度看板」。
- **低打扰**做成一等交互（呼吸点、全屏退让、按住 Ctrl 让路、托盘可退出），
  而非设置里的一个开关。
- **工程完成度**（多屏 / DPI / 无障碍 / 签名 / 自动更新 / 安装包）是社区反复
  点名的行业痛点，也是可信度差异位。
- 与「脆弱 shell 改造」和「无节制皮肤生态」同时划界：独立进程、不注入、
  命名管道、配置原子写与迁移、可断言的硬常量。

---

## 8. 明确排除与未核实项

- **排除**：tsight.io 关于「微软把灵动岛搬进任务栏 / TaskbarCore.dll」的
  虚构内容；SEO 内容农场仅作线索与交叉印证，不作观点权威。
- **不计入开源数**：DynamicWin（仅托管 releases，无源码）、TopNotch（闭源 EULA）、
  NotchClean / Notchify / Win Dynamic Island（Store 付费）、V2EX 2021 Bangs（闭源孤例）。
- **未核实**：多数项目的 star 数（尤其 BrimDeck 镜像站称 634★，未在官方页核实）、
  GitLab/Codeberg 是否真的全无同类、Lumen / Nusa / NSD 官方仓 URL、
  知乎 / 机核 / 远景的具体讨论帖、多数页面的热度数据。
- **不采用为常量**：广传的「23 分 15 秒重新专注」经复核与 resumption lag 混淆；
  Fluent 旧 UWP 150/300/500ms 标为历史值；`WDA_EXCLUDEFROMCAPTURE` 官方条款未核实。
- **更名消歧**：检索未发现与本项目更名前完全同名的公开仓，但近名项目极多
  （`Dynamic-Island-for-Windows`、`windows-dynamic-island`、`DynamicIsland-Windows`），<!-- BI_CITATION_ 第三方仓库真实名称 -->
  已在 [更名报告](../RENAME.md) 里处理。

---

## 9. 代表性链接索引（完整可点）

**代码托管**：[GitHub topic: dynamic-island-windows](https://github.com/topics/dynamic-island-windows) ·
[NotchPeninsula](https://github.com/GEORGEWWWU/NotchPeninsula) ·
[WinIslands](https://github.com/JudeKwong/WinIslands) ·
[Lost Island](https://github.com/byte4day/lost-island-windows) ·
[Nectar](https://github.com/kua8/Nectar) ·
[岛仔](https://github.com/jilespace/jilespace-daozai) ·
[Bunbun](https://github.com/iaryasharma/Bunbun) ·
[Fly Island](https://github.com/flyl1u/fly-island) ·
[Halo](https://github.com/phoseinq/Halo) ·
[BrimDeck](https://github.com/MoFeng2223/BrimDeck) ·
[HeyyCzer](https://github.com/HeyyCzer/windows-dynamic-island) ·
[Developer-Island](https://github.com/maximilian467/Developer-Island) ·
[OpenCode Notch](https://github.com/fraches05/opencode-notch) ·
[Mintu](https://github.com/HuseynDeveloper/Mintu) ·
[PrismGlass](https://github.com/liburce0412-alt/prismglass-desktop) ·
[DynamicWin](https://github.com/FlorianButz/DynamicWin) ·
[devcode90](https://github.com/devcode90/Dynamic-Island-for-Windows) ·
[Gitee go-dynamic-lsland](https://gitee.com/sun_yunlong/go-dynamic-lsland) ·
[Rainmeter 皮肤](https://github.com/KrazyManJ/rainmeter-dynamic-island)

**论坛 / 媒体**：[Windows Central: Notchify](https://www.windowscentral.com/software-apps/notchify-brings-macos-style-dynamic-island-flair-to-windows-11) ·
[少数派：灵动岛为什么不好](https://sspai.com/post/76350) ·
[少数派：NotchPeninsula 派评](https://sspai.com/post/118833) ·
[Droppy: Dynamic Island on the Mac](https://getdroppy.app/blog/dynamic-island-for-mac) ·
[V2EX 2021 镜像](https://global.v2ex.co/t/809467) ·
[mjtsai: Using Gestures Inside the Dynamic Island](https://mjtsai.com/blog/2022/10/07/using-gestures-inside-the-dynamic-island/) ·
[Reddit: Lumen](https://www.reddit.com/r/desktops/comments/1u67o89/) ·
[DEV: OpenCode Notch](https://dev.to/mrsdsxp/i-built-a-windows-notch-overlay-for-my-opencode-sessions-3enl) ·
[DEV: Mintu](https://dev.to/huseyn_huseynov_42/building-a-cross-platform-dynamic-island-with-tauri-rust-and-typescript-introducing-mintu-v022-d56) ·
[Position is Everything](https://www.positioniseverything.net/i-tried-the-dynamic-island-on-my-macbook-heres-how) ·
[Hacker News: Crest](https://news.ycombinator.com/item?id=49819868) ·
[codeDB Lens: Windows warning](https://lens.codedb.com.br/windows-warning) ·
[Nostos: Distribution and code signing](https://github.com/rA9-001/Nostos/blob/main/docs/distribution.md) ·
[Flow Launcher](https://flow-launcher.com/) ·
[Rainmeter 5.0 changelog](https://www.rainmeter.net/changes-5.0/) ·
[Stack Overflow: topmost](https://stackoverflow.com/questions/14989062/set-a-window-to-be-topmost/31170225)

**官方规范 / 理论**：[Apple HIG Live Activities](https://developer.apple.com/design/human-interface-guidelines/live-activities) ·
[Apple HIG Widgets](https://developer.apple.com/design/human-interface-guidelines/widgets) ·
[MS Learn Widget providers](https://learn.microsoft.com/en-us/windows/apps/develop/widgets/widget-providers) ·
[MS Learn Badges](https://learn.microsoft.com/en-us/windows/apps/develop/notifications/badges) ·
[WinUI3 Timing and easing](https://learn.microsoft.com/en-gb/windows/apps/design/motion/timing-and-easing) ·
[GNOME HIG Principles](https://developer.gnome.org/hig/principles.html) ·
[WCAG 2.2 SC 1.4.3](https://www.w3.org/WAI/WCAG22/Understanding/contrast-minimum.html) ·
[WCAG 2.2 SC 1.4.11](https://www.w3.org/WAI/WCAG22/Understanding/non-text-contrast.html) ·
[Calm Technology](https://calmtech.com/about.html) ·
[Calm Tech Institute](https://www.calmtech.institute/calm-tech-certified) ·
[UCB Glanceable Peripheral Displays](https://www.eecs.berkeley.edu/Pubs/TechRpts/2006/EECS-2006-113.pdf) ·
[MSR Bounded Deferral](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/11/bdef_studies.pdf) ·
[Attention-Sensitive Alerting (UAI'99)](https://arxiv.org/pdf/1301.6707v1) ·
[Microsoft PowerToys](https://github.com/microsoft/PowerToys) ·
[Wallpaper Engine SceneScript](https://docs.wallpaperengine.io/en/scene/scenescript/reference.html)
