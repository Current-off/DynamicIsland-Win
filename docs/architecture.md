# 架构说明（S3 阶段）

## 分层

```
┌──────────────── 渲染层 IOverlayRenderer（可替换） ────────────────┐
│ P0: Windhawk tool mod (C++)，运行在 windhawk.exe 自身进程           │
│ P2: 独立 WinUI3 / 原生 Win32 进程                                  │
│ 分层窗口 · SetWindowRgn · click-through · DWM 亚克力 · 弹簧动画       │
│ 三态布局契约: compactLeading | compactTrailing | minimal | expanded   │
└──────────────────────────────┬──────────────────────────────────┘
                               │ 命名管道 JSON lines（协议见 protocol.md）
┌──────────────────────────────┴──────────────────────────────────┐
│ 核心域 Activity Runtime（core/，本阶段已实现，平台无关）             │
│ ActivityStore  · Scheduler  · CapsuleStateMachine                  │
│ EventBus       · DesignTokens · ActivityRuntime（门面）             │
└──────────────────────────────┬──────────────────────────────────┘
                               │ Provider 接口
┌──────────────────────────────┴──────────────────────────────────┐
│ Provider 层                                                       │
│ 内置: 媒体会话 / 通知 / 电池 / 蓝牙 / Wi-Fi / 音量 / 时间            │
│ 桥接: 下载 · 剪贴板 · 日历(OAuth) · AI Agent                        │
│ 第三方: Provider SDK                                              │
└─────────────────────────────────────────────────────────────────┘
旁路: Settings(中文分组/幂等迁移) · Installer(HKCU Run/卸载) · 本地诊断日志
```

## 当前实际落地形态（第五轮）

规划里的「渲染层 + 命名管道 + 桥接进程」是 P2 目标形态；现在跑的是一个**单进程**实现，
但分层已经按最终形态切开，替换其中任何一层都不需要动其他层：

```
采集层 src/win32/live_sources.hpp        Win32 / WinRT 调用只出现在这里
        │  Snapshot（纯数据，无平台类型）
映射层 src/core/live_mapping.hpp          系统状态 → Activity（优先级 / 过期 / content_json）
        │  Activity 候选
规则层 src/core/rules.hpp                 用户规则：事件开关 / 免打扰 / 应用 / 扩展名
        │  通过规则的 Activity
核心域 src/core/activity.hpp              Store · Scheduler · StateMachine · Runtime
        │  Frame / ScheduleResult
渲染层 src/win32/renderer.hpp             只认 CapsuleView，不认系统状态
        │
上屏   src/win32/layered_surface.hpp      32bpp DIB + UpdateLayeredWindow（预乘 alpha）

配置   src/win32/rules_io.hpp             %APPDATA%\balance_island\rules.json（mtime 热加载）
界面   src/win32/settings_ui.hpp          原生 Win32 控件，只读写 RuleConfig
```

两条硬约束：

1. **只有采集层允许出现 Win32 / WinRT 类型**。映射层与规则层是纯 C++，
   所以 `rules_tests` 能在任何机器（含 CI 无桌面会话）上跑，这是它们敢被信任的前提。
2. **渲染层不允许认识「复制」「通知」这些业务词**。它只按 `kind` 取强调色与图标，
   文案全部来自 `content_json`。第一版把歌名硬编码在渲染层，换真实数据后屏幕仍是假内容，
   这个坑已经写进 CONTRIBUTING。

## 为什么渲染层可替换

规划文档 7.1 的核心决策：P0 借 Windhawk 生态快速落地，P2 做零依赖独立版。
两者共用同一套 Activity 内核、Provider 接口与 IPC 协议，P2 是「换渲染层」而非重写项目。
因此 `contracts/IOverlayRenderer.hpp` 不引入任何平台头文件。

## 核心域数据流

1. Provider `poll(now_ms)` 返回 `ProviderUpdate`（内容未变化返回空）。
2. `ActivityStore.upsert()` 幂等写入；`purge_expired(now)` 把到期活动标记为 `STALE`。
3. `Scheduler.select()` 按 `(−relevanceScore, createdAtMs, id)` 确定性排序，
   最多取 `max_visible` 个，分配 `compact / minimal / expanded` 呈现。
4. `ScheduleResult` 交给渲染层绘制；`CapsuleStateMachine` 同步胶囊状态。
5. `EventBus.publish()` 分发诊断事件，单个订阅者异常被隔离并记入 `failures`。

## 降级链（对应 EARS Unwanted / State-driven）

| 条件 | 行为 |
|------|------|
| 桥接离线 | 过滤 `bridge.*` 类活动，内置 `media` / `system` 继续显示 |
| 协议版本不兼容 | `negotiate()` 返回 `incompatible`，渲染层降级并提示升级 |
| 全屏 / 反作弊 | `set_suspended(True)` → 胶囊隐藏、活动状态保留、采样暂停 |
| 无透明效果 / 远程桌面 | `SetBackdropMode(OpaqueFallback)`，交互不减 |
| Reduce Motion | `DesignTokens.animation_duration_ms()` 返回 0，动画改用 fade |

## 已实现 / 未实现边界

- 已实现：核心域全部逻辑、协议编解码与协商、两个内置 Provider 样例、C++ 接口契约头文件。
- 未实现：Windhawk mod 源码、命名管道实体、桥接进程、设置面板、安装器、实机视觉验收。
- `contracts/*.hpp` 未经编译；本机无 C++ 编译器，详见 `verification.md`。
