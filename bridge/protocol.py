"""IPC 协议 v1（命名管道 JSON lines）。

设计来源：
- 规划文档 7.2 IPC 契约（protocolVersion / seq / type / activity / ts）
- dynamic-island-v2 的双进程协议，但增加显式版本协商，消除
  「sourcecode 必须与 bridge 构建版本严格匹配」这一交付缺陷。
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any

PROTOCOL_VERSION = 1
PIPE_NAME = r"\\.\pipe\DynamicIslandWin-v1"


class ProtocolError(ValueError):
    pass


@dataclass(frozen=True)
class Message:
    protocol_version: int
    seq: int
    type: str
    ts_ms: int
    activity: dict[str, Any] | None = None

    def to_dict(self) -> dict[str, Any]:
        data: dict[str, Any] = {
            "protocolVersion": self.protocol_version,
            "seq": self.seq,
            "type": self.type,
            "ts": self.ts_ms,
        }
        if self.activity is not None:
            data["activity"] = self.activity
        return data


def encode(message: Message) -> str:
    """确定性编码：键顺序固定、无多余空白、单行。"""
    return json.dumps(message.to_dict(), ensure_ascii=False, separators=(",", ":"))


def decode(line: str) -> Message:
    """严格解码：缺字段、类型错误、多余行、非法 JSON 均抛出 ProtocolError。"""
    if not isinstance(line, str):
        raise ProtocolError("line must be str")
    stripped = line.strip()
    if not stripped:
        raise ProtocolError("empty line")
    if "\n" in stripped or "\r" in stripped:
        raise ProtocolError("JSON lines must contain exactly one message")
    try:
        data = json.loads(stripped)
    except json.JSONDecodeError as exc:
        raise ProtocolError(f"invalid JSON: {exc.msg}") from exc
    if not isinstance(data, dict):
        raise ProtocolError("message must be a JSON object")

    for key in ("protocolVersion", "seq", "type", "ts"):
        if key not in data:
            raise ProtocolError(f"missing field: {key}")

    version, seq, msg_type, ts = (
        data["protocolVersion"],
        data["seq"],
        data["type"],
        data["ts"],
    )
    if not _is_int(version) or version < 1:
        raise ProtocolError("protocolVersion must be a positive integer")
    if not _is_int(seq) or seq < 0:
        raise ProtocolError("seq must be a non-negative integer")
    if not _is_int(ts) or ts < 0:
        raise ProtocolError("ts must be a non-negative integer")
    if not isinstance(msg_type, str) or not msg_type:
        raise ProtocolError("type must be a non-empty string")
    activity = data.get("activity")
    if activity is not None and not isinstance(activity, dict):
        raise ProtocolError("activity must be an object when present")

    return Message(
        protocol_version=int(version),
        seq=int(seq),
        type=msg_type,
        ts_ms=int(ts),
        activity=activity,
    )


def negotiate(peer_version: int) -> str:
    """版本协商结果：compatible / degraded / incompatible。

    - 同版本：compatible
    - 低一个主版本：degraded（读取已知字段，忽略未知字段，继续工作）
    - 其他：incompatible（渲染层降级到内置 Provider 并提示升级）
    """
    if not _is_int(peer_version) or peer_version < 1:
        return "incompatible"
    if peer_version == PROTOCOL_VERSION:
        return "compatible"
    if peer_version < PROTOCOL_VERSION:
        return "degraded"
    return "incompatible"


def _is_int(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)
