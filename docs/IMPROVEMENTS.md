# 改进项清单（v0.2.0）

> 依据 [全网调研对比结论](research/landscape.md) 与 [平衡岛范式](PARADIGM.md)。
> 原则：**最小改动、不破坏现有接口、每条改进都能对照范式说出「维持了哪条平衡」**。
> 状态标记：✅ 已落地 / 🟡 部分落地 / ⬜ 待落地 / ⛔ 明确不做。

---

## 1. 本轮已落地（可验证）

| # | 改进 | 调研依据 | 验证方式 |
|---|---|---|---|
| 1 | **全项目更名** `balance_island 平衡岛`，含 exe / 配置目录 / 管道 / 窗口类 / 命名空间 / 文档 / 界面文案 | Fly Island「改名只改招牌」的反面教材；近名项目极多需消歧 | `python tools/naming.py --check` → 0 处残留；编译 + 569 项断言 |
| 2 | **配置自动迁移**：新目录首次启动从旧目录复制 `rules.json`，只复制不删除 | Ice `MigrationManager`；更名不得让用户设置丢失 | 造旧配置 → 启动 → 新目录内容一致（实测通过） |
| 3 | **旧安装清理**：结束旧进程、移除旧快捷方式/自启项/安装目录，保留配置 | V2EX 2021「咋关掉」；避免升级后双份 | `tools/install.ps1` |
| 4 | **范式三方契约**：C++ `BalanceRules.hpp` ↔ Python `core/balance.py` ↔ JSON `balance-paradigm.json` | 规则必须是可断言数字，不能只写在文档里（R91 原子写、R96 硬阈值同源思想） | `check_contracts.py` 由 7 项增至 **53 项全过** |
| 5 | **按住 Ctrl 让路**：`WS_EX_TRANSPARENT` + `SWP_FRAMECHANGED` + 降透明至 35%，松开由全局键状态恢复 | BrimDeck/Nectar/Lumen 把穿透做成设置项；本范式把它升为即时手势（控制权轴） | 编译通过、交互语义写入 JSON 契约；**按键注入实测被安全策略拦截，视觉与穿透待人工实机验证** |
| 6 | **静止桌面跳过抓屏**：32×8 缩略采样算 FNV 指纹，未变则跳过 2.3M 像素 BitBlt + 降采样 + 3 趟模糊 | NotchPeninsula DXGI + 内容哈希「静止零开销」；issue #1 未做建议 #2 | 实测 9 秒内出现 `glass: skipped (unchanged desktop)` |
| 7 | **WinUI3 动效常量**：进入 ≤250ms、退场 ≤167ms（入慢退快） | WinUI3 `ControlNormal/ControlFast`；R38 | 三方契约校验 |
| 8 | **每版发布附 SHA256SUMS** + README 教用户 `Get-FileHash` 核验、明确不叫用户关 Defender | codeDB Lens / Nostos / PrismGlass / Flow Launcher 四家口径一致 | 打包实测清单与实际哈希一致；`release.yml` 同步上传 |
| 9 | **范式宪法与理论锚点**：bounded deferral 守恒断言、三个空白位、外部依据对照表 | Horvitz *Balancing Awareness and Interruption*；Calm Tech；范式调研 §6 | `docs/PARADIGM.md` |
| 10 | **README 重定位**：首屏改为「注意力配重，不是状态舞台」，交互表补让路 | Droppy「照搬 = costume」；The Verge「交互 backwards」 | 文档 |
| 11 | 上一轮已交付：epoch 节流（阻塞 15.2%→1.7%）、配置原子写、IPC 队列上限与关停死锁、issue #2 的 10 条修复 | issue #1 / #2 | 569 项断言 |
| 12 | **设置面板分四页扩展**：外观 / 行为 / 性能 / 预设，新增 20 个个性化字段与 68 条断言 | 「设置只有这些功能」的用户反馈；调研里 WinIslands/BrimDeck/NotchPeninsula 的可配置粒度 | `--selftest`：zones=31、unclickable=0；637 项断言全绿 |
| 13 | **新设置全部接线生效**（不是存了没用的假开关）：主题、强调色、字号、圆角、顶部距离、三种自动隐藏、安静态形态、让路键、减少动效、省电降级、玻璃质量、抓屏间隔、目标显示器 | Rainmeter「配置→脚本→插件」分层；调研中「默认少模块 + 可调」的社区共识 | 逐项接入 `overlay_main.cpp` / `renderer.hpp`，编译与渲染测试 33 项全绿 |
| 14 | **主线程帧耗时探针** `perf: max_ui_work`（每 2 秒窗口最大值） | 「自动化通过 ≠ 实机验收」；Notchify 把 idle CPU 56%→6% 的量化口径 | 10 秒实测：8.6 / 12.6 / 13.8 / 19.8ms |
| 15 | **修好 `v0.3.0` 设置面板「控件只在布局里、没画到屏幕上」**：绘制与命中统一由 zones 表驱动，新增 `HandleHit()` 覆盖全部命中区；四页 20 个字段全部可见可点 | 用户截图反馈（只看到三行、Tab 空白）；死代码 `DrawTab` 等 5 个助手从未被调用 | `settings_probe`：576 区域 / 隐形 0 / 失败 0；715 项断言全绿 |
| 16 | **窗口高度随页变化 + 内容区滚动**：短页不再留大片空白；1080p + 150% 下行为页放不下时滚动而不是把「保存」顶出屏幕 | 自测发现固定 880 逻辑高度在小屏会把底部按钮裁掉 | 四档 DPI × 有无滚动共 16 组探针；受限高度下按钮必在窗口内 |
| 17 | **预设页做成真功能**：极简 / 均衡 / 沉浸 / 节能 + 恢复出厂，且只改外观与行为，不碰事件开关、黑名单、代理、Agent 权限 | 「换个皮肤顺手改掉用户的定制」是最难察觉的一类回归 | `settings_tests` 78 条断言（含「预设保留用户选择」14 条） |

---

## 2. 待落地（按优先级，附理由）

### P0 — 阻断 v1 的两项

| 项 | 现状 | 要做 | 为什么现在没做 |
|---|---|---|---|
| **抓屏 + 模糊移出 UI 线程** | ✅ **v0.3.0 已根治**：`glass_async.hpp` 后台线程 + 单槽请求/结果交换，主线程只消费纯像素快照；实测后台 42–50ms 而主线程单帧 8.6–19.8ms，无尖峰传导。详见 verification.md 第二十七轮 | 已落地并实测 |
| **多显示器 + PMv2 DPI**（issue #3） | 🟡 目标显示器三档（主屏 / 光标所在屏 / 指定序号）已接线，虚拟坐标允许负值、逐屏 `GetMonitorInfo`，光标跨屏 500ms 节流重算。**只有单屏实测**，多屏行为与 DPI 迁移仍需真实硬件验证 | 部分落地 |

### P1 — 分发与信任（社区一手证据最充分）

| 项 | 做法 | 依据 |
|---|---|---|
| **构建溯源 / winget / Scoop / 安装包 / 自动更新** | `actions/attest-build-provenance`；包管理器 manifest（免费、hash 可审计）；NSIS / WiX MSI；验签后替换的更新器 | ✅ SHA256SUMS 已落地；其余仍待做。codeDB Lens、Nostos、PrismGlass、Flow Launcher 口径一致。未签名时「每次发布信誉归零」，自动更新须与签名身份一起规划 |

> 明确**不做**：自签名证书、单独用 Sigstore/cosign（Windows 不认 Authenticode）、
> 让用户关 Defender/加白名单、UPX 加壳、.NET single-file。

### P1 — 低打扰状态机

| 项 | 做法 | 依据 |
|---|---|---|
| **三种自动隐藏可任意组合** | 焦点离开 / 暂停播放后 / 全屏时，独立开关；关键内容展示期间禁止隐藏 | NotchPeninsula |
| **「无活动时完全隐藏」开关** | 默认偏向隐藏，可选「呼吸点 / 把手 / 完全不显示」 | 少数派「无任务时仍常驻违背低打扰」；V2EX 位置之争 |
| **默认少模块** | 首次运行只开 1–2 类，其余按需开启 | Position is Everything「too many modules → another notification area」；Calm Tech R1 |
| **场景触发器** | 会议中/拔电/公共网络自动切换显隐 | Bartender Triggers |

### P2 — 工程完成度

| 项 | 做法 | 依据 |
|---|---|---|
| **全屏检测三态** | `SHQueryUserNotificationState` 三 busy 态 OR 前台矩形按所在屏比较；约 800ms 轮询 + `EVENT_SYSTEM_FOREGROUND` | R115–R119 |
| **z 序重新断言**（issue #2 #12） | `EVENT_OBJECT_REORDER` 防抖 + 周期巡检 | R107 / Stack Overflow 高票答案 |
| **配置 `schemaVersion` + 有序迁移链** | JSON 根节点带版本；未知更高版本拒绝并保留原文件 | R92–R95；Ice |
| **分层扩展** | IPC 协议公开成文档，升级为可发现/可订阅的本机节点（对标 MSP），默认关闭 | NotchPeninsula MSP；PowerToys R62；Rainmeter 分层 |
| **无障碍** | 键盘路径/全局热键、accessible name/role/state、焦点指示 ≥3:1、文本不栅格化、系统字号放大不截断、减少动效 | WCAG 2.2 R96–R101；Apple HIG Widgets |
| **键盘命令面板** | `Ctrl/Cmd+K` 唤出，键盘优先 | OpenCode Notch；Droppy「media keys beat hovering」 |
| **孤立代理对 → U+FFFD**（issue #2 #9） | `mini_json` 字符串解码配对失败时替换 | 调研前已识别 |
| **三处 data race** | `lyrics.hpp` `simplify_` 加原子；editor/browser watcher 的 `bool` 改 atomic | 审查报告低危观察 |
| **TOPMOST 三形态 / 主题预设** | `Notch / Capsule / Indicator` + 主题包 | 行业事实标准；本项目已有三档材质，可复用 |

### P2 — 立即可做的低成本项（建议下一轮）

- `rules.json` 写 `schemaVersion`（几行）
- Release 附 `SHA256SUMS.txt`（改 `package.ps1` + `release.yml`）
- `SetWindowPos(HWND_TOPMOST, …)` 节流重断言（约 10 行）
- 主题预设（把现有三档材质参数存成可切换预设）

---

## 3. ⛔ 明确不做（范式边界，写下来比事后解释省事）

| 不做 | 理由 |
|---|---|
| **AI 额度看板 / token 统计** | 至少 4 家已做（Crest、OpenCode Notch、Developer-Island、BrimDeck），赛道拥挤；本项目差异在真实系统活动聚合 |
| **插件市场 / 应用启动器 / 剪贴板管理器 / 待办工具** | 会退化成「another notification area to manage」；按范式选择「极简内核 + 公开 IPC」，不做 Rainmeter 式全开放 |
| **读聊天正文** | 隐私与风控；同类项目（Fly Island）也主动掐住这条能力边界 |
| **账号 / 云同步 / 订阅分级** | 与「本地、无遥测、数据留本机」承诺冲突 |
| **内嵌浏览器引擎做常驻壳** | Electron/WebView2 常驻开销；本项目 C++20 原生定位天然占优 |
| **注入 `explorer.exe` / patch 系统 DLL** | Win11 无受支持的第三方任务栏扩展点；ExplorerPatcher 反例，随系统更新失效 |
| **照搬 Apple HIG 的 44pt / 84–160pt / ≤2s** | 那是 iOS 硬件挖孔的约束，不是桌面岛的规则；`core/tokens.py` 已明确改为「调研参照值」 |
| **自签名证书 / 让用户关闭安全软件** | 社区一致反对；自签名教用户装根证书比 SmartScreen 提示更危险 |

---

## 4. 覆盖度自评（诚实边界）

| 用户要求的类别 | 本轮处理 |
|---|---|
| 逻辑错误 / 潜在 bug | ✅ 上一轮修 issue #2 的 10 条；本轮无新增逻辑缺陷发现 |
| 边界条件 / 异常处理 | ✅ 契约加入 `quiet_alpha` 相位、`clip_transition_ms` 负值、`info_level` L0–L3 断言 |
| 空值 / 类型问题 | ✅ 上一轮 `lyrics_proxy` 转义、`hint[32]` 初始化、NaN/inf/hex 拒绝 |
| 资源泄漏 / 并发风险 | ✅ 上一轮 `FindClose` 泄漏、IPC 关停死锁、队列上限；⏳ 三处 data race 待补 |
| 安全隐患 | ✅ 命名管道 Flood 上限、配置原子写；⏳ 分发可信度待补 |
| 性能瓶颈 | ✅ epoch 节流 + 静止桌面跳过抓屏；⏳ 抓屏移出主线程待补（最大剩余风险） |
| 代码重复 / 风格不一致 | ✅ 范式常量收敛到单一契约源；⏳ 未做全仓风格重构（刻意，避免大范围改动） |
| 未使用代码 / 依赖 | 🟡 `core/tokens.py` 明确降级为「调研参照值」，未删除（契约仍需它） |
| 配置 / 环境路径错误 | ✅ 更名迁移 + 安装脚本清理 + 白名单审计 |

**最重要的一句**：本轮改动面已经很大（更名 541 处 + 新增范式/契约/交互/抓屏优化），
按项目一贯纪律，**应先让外部审查者复验，再继续堆改动**。
自动化通过 ≠ 实机视觉验收；P0 两项（多线程抓屏、多显示器）尤其需要真实硬件验证。
