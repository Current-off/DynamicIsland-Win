# IPC 协议 v1

管道：`\\.\pipe\DynamicIslandWin-v1`（per-user ACL，无 TCP 端口、无 LAN 端点）。
格式：UTF-8 JSON lines，一条消息一行，键顺序固定、无多余空白。

## 消息字段

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `protocolVersion` | 正整数 | 是 | 当前为 `1` |
| `seq` | 非负整数 | 是 | 单调递增序号，用于检测丢帧 |
| `type` | 非空字符串 | 是 | 见下表 |
| `ts` | 非负整数 | 是 | 发送方毫秒时间戳 |
| `activity` | 对象 | 否 | 活动载荷，字段见下文 |

解码器（`bridge/protocol.py::decode`）严格执行：缺字段、类型错误、布尔冒充整数、
负序号/时间戳、非对象 activity、多行输入、非法 JSON 均抛 `ProtocolError`。
未知字段被忽略，用于前向兼容。

## 消息类型

| type | 方向 | 说明 |
|------|------|------|
| `activity.upsert` | bridge → overlay | 新建或更新活动（按 id 幂等） |
| `activity.end` | bridge → overlay | 结束活动，`activity.id` 必填 |
| `state.frame` | bridge → overlay | 一帧调度结果 |
| `heartbeat` | 双向 | 心跳；超时触发重连 |
| `hello` | overlay → bridge | 携带本地协议版本，发起协商 |
| `settings.update` | overlay → bridge | 设置变更（仅传变更键） |

## activity 对象

字段名与 `core/activity.py::to_dict()` 一致：

```json
{
  "id": "media:now-playing",
  "kind": "media",
  "title": "Song title",
  "relevanceScore": 80.0,
  "createdAtMs": 1000,
  "staleAtMs": 5000,
  "content": {"artist": "...", "playing": true},
  "state": "active"
}
```

`kind` 以 `bridge.` 前缀标识桥接类活动；桥接离线时这类活动被过滤，
内置活动（`media` / `system` 等）不受影响。

## 版本协商

`negotiate(peerVersion)` 返回三种结果：

| 结果 | 条件 | 处理 |
|------|------|------|
| `compatible` | 等于当前版本 | 正常工作 |
| `degraded` | 低于当前版本 | 读取已知字段、忽略未知字段，继续工作 |
| `incompatible` | 高于当前版本或非法值 | 渲染层降级到内置 Provider 并提示升级 |

该机制消除 dynamic-island-v2 中「mod 源码必须与 bridge 构建版本严格匹配、
升级需用户手动重新粘贴并编译」的交付缺陷。

## 连接与重连

- overlay 以 worker thread 非阻塞消费消息，桥接重启后自动重连。
- 桥接离线期间 overlay 保持最后一次有效帧与内置 Provider 状态，不显示错误弹窗。
- 心跳超时判定与重连退避参数属于 S5 实现范围，本阶段未实现。

## 安全边界

- 管道 ACL 限定当前 Windows 用户；不监听任何 TCP 端口。
- 凭据（日历 OAuth token 等）仅由桥接进程用 DPAPI 加密存储，永不进入协议消息或日志。
- 剪贴板内容不经过协议传输，除非用户显式启用对应 Provider（默认关闭）。
