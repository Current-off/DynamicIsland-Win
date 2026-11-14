# 更名一致性报告：DynamicIsland-Win → balance_island 平衡岛

> 执行版本：`v0.2.0`（2026-11-14）
> 工具：`python tools/naming.py --apply` / `--check` / `--dry-run`

## 1. 命名约定

| 场景 | 用法 |
|---|---|
| 项目标识 / 包名 / 目录 / 可执行文件 | `balance_island` |
| 中文产品名 / 界面文案 | `平衡岛` |
| 合写（标题、首次出现） | `balance_island 平衡岛` |
| 技术标识前缀（窗口类、include guard、命名空间） | `BI_` / `bi` |
| 品类术语（**不是**产品名） | `灵动岛` 专指 Apple 功能；`同类桌面岛` 指这个品类 |

**「灵动岛」不被全局替换**，这是本次更名最重要的判断：文档里大量句子是在谈
Apple 的功能或整个品类（如「区别于对灵动岛的简单复刻」）。一刀切会写出
「区别于对平衡岛简单复刻」这种语义反转的句子。指代本项目的短语逐条人工判定，
见第 6 节。

## 2. 标识映射表

| 类别 | 旧 | 新 |
|---|---|---|
| 产品名 | `DynamicIsland-Win` | `balance_island` |
| 可执行文件 | `diw_overlay.exe` / `.obj` | `balance_island.exe` / `.obj` |
| 日志 | `diw-overlay.log` / `.crash.log` / `.crash.dmp` | `balance-island.*` |
| 安装包 | `DynamicIsland-Win-<ver>.zip` | `balance_island-<ver>.zip` |
| 安装目录 | `%LOCALAPPDATA%\DynamicIsland-Win` | `%LOCALAPPDATA%\balance_island` |
| 配置目录 | `%APPDATA%\DynamicIsland-Win` | `%APPDATA%\balance_island` |
| 缓存目录 | `…\DynamicIsland-Win\covers` | `…\balance_island\covers` |
| 快捷方式 | `DynamicIsland-Win.lnk` | `balance_island.lnk` |
| 自启项 | `DynamicIslandWin` | `BalanceIsland` |
| 单实例 mutex | `DynamicIsland-Win-Overlay-SingleInstance` | `BalanceIsland-Overlay-SingleInstance` |
| 主窗口类 | `DIW_OverlayWindow` | `BI_OverlayWindow` |
| 设置窗口类 | `DIW_SettingsWindow` | `BI_SettingsWindow` |
| 探针窗口类 | `DIW_WhiteBackground` | `BI_WhiteBackground` |
| include guard | `DIW_SRC_*` / `DIW_CONTRACTS_*` | `BI_SRC_*` / `BI_CONTRACTS_*` |
| C++ 命名空间 | `diw` / `diw::` | `bi` / `bi::` |
| IPC 管道 | `\\.\pipe\DynamicIslandWin-v1` | `\\.\pipe\BalanceIsland-v1` |
| 托盘提示 | `DynamicIsland-Win（…）` | `平衡岛 balance_island（…）` |
| 设置面板标题 | `灵动岛` | `平衡岛` |
| 首启提示 | `灵动岛正在运行` | `平衡岛正在运行` |

界面与文案里指代本项目的「灵动岛」共 13 处，全部改为「平衡岛」或按语境重写。

## 3. 兼容性处理（否则更名会造成真实损害）

| 风险 | 处理 |
|---|---|
| 老用户 `rules.json` 留在旧目录，升级后设置"凭空丢失" | `RulesStore::MigrateLegacyConfig()`：新目录首次启动且无配置时，从旧目录**复制**（不删除、不覆盖）。实测：写入旧配置 → 启动 → 新目录出现同内容 |
| 旧进程占用文件导致覆盖失败 | `tools/install.ps1` 先结束旧进程名 |
| 旧快捷方式 / 旧自启项残留成"双份" | 安装时移除旧 `.lnk` 与旧 Run 项 |
| 旧安装目录残留 | 安装时删除（**不含**配置目录，配置由程序迁移） |
| 管道改名破坏已有 IPC 客户端 | 见第 4 节，属 0.x 允许的 breaking change |

反面参照：调研中发现的 `fly-island` 项目从 FocuSD 更名后，数据目录仍是
`com.focusd.island`、脚本仍是 `focusd-agent-status.ps1`（改名只改了招牌）。
本次的 `naming.py --check` 就是为了避免这种"表面更名"。

## 4. 有意保留的旧称出现（审计白名单）

| 位置 | 原因 |
|---|---|
| `https://github.com/Current-off/DynamicIsland-Win` 及由其派生的 badge / Actions / Release URL | **仓库本身未改名**，这是外部真实地址，改动会让链接失效。若之后重命名仓库，需同步更新这些 URL、git remote 与 CI |
| `docs/verification.md` 第二十六轮之前的历史引述 | 史实记录里的用户原话与当时名称，改掉会破坏记录真实性。已在文件头加档案说明 |
| `BI_LEGACY_` 标记行的旧目录/进程/管道名 | 迁移与清理代码必须引用旧标识才能找到旧数据；要求标记与旧称在**同一行**，防止白名单被滥用 |
| `docs/RENAME.md` 本身 | 对照表需要引用旧称 |
| 「灵动岛」作 Apple 功能 / 品类术语 | 见第 1 节 |

**已知 breaking change**：IPC 管道名改为 `\\.\pipe\BalanceIsland-v1`。
`tools/send-activity.ps1` 已同步；自行写过客户端的用户需要改管道名。
依据是 `CONTRIBUTING.md §8`：`0.y.z` 阶段不承诺兼容，`1.0.0` 起才冻结协议 v1。

## 5. 一致性验证结果

| 检查 | 命令 / 方法 | 结果 |
|---|---|---|
| 旧称残留 | `python tools/naming.py --check` | ✅ 0 处（白名单外） |
| 替换范围 | `--dry-run` | 541 处 / 67 个文件 |
| 编译 | `tools/build.ps1` | ✅ 零警告，产物 `balance_island.exe` |
| 全量测试 | 同上 | ✅ 569 项断言全绿 |
| 配置迁移 | 造旧配置 → 启动 → 核对新目录 | ✅ 内容一致，旧目录保留 |
| 界面文案 | 逐文件人工判读 13 处 | ✅ 无产品名误用 |

`tools/naming.py` 保留在仓库中，可随时复跑；建议接入 `check_contracts.py`
或 CI，防止后续代码把旧名带回来。

## 6. 术语判读清单（人工逐条）

| 原文 | 判定 | 处理 |
|---|---|---|
| 「Windows 也有灵动岛」（README 首屏） | 指本项目 | 重写为「屏幕顶部，一枚保持平衡的岛」 |
| 「灵动岛正在运行」（首启 toast） | 指本项目 | 平衡岛 |
| 设置面板标题「灵动岛」 | 指本项目 | 平衡岛 |
| 「灵动岛实时通知测试」（探针） | 指本项目 | 平衡岛 |
| 「灵动岛通知回归」（历史日志样本） | 历史采样文本 | 保留（档案） |
| 「iOS 灵动岛的手感…ζ≈0.68」（源码注释） | Apple 参照 | 保留，改称 iOS 灵动岛 |
| 「灵动岛三态呈现（WWDC23 10194）」 | Apple 参照 | 改为「三态呈现（参照 iOS 灵动岛…）」 |
| 「Windows 开源灵动岛项目普遍缺失」 | 品类 | 改为「同类 Windows 开源项目」 |
| 「QQ / 微信来消息灵动岛没反应」（用户原话） | 历史引述 | 保留（档案） |
