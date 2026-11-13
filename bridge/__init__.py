"""桥接进程与 IPC 协议（平台无关部分）。"""
from .protocol import (
    PIPE_NAME,
    PROTOCOL_VERSION,
    Message,
    ProtocolError,
    decode,
    encode,
    negotiate,
)

__all__ = [
    "PROTOCOL_VERSION",
    "PIPE_NAME",
    "Message",
    "ProtocolError",
    "encode",
    "decode",
    "negotiate",
]
