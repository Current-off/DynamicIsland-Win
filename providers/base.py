"""Provider 接口契约。

约束：
- 每个 Provider 必须可独立启用/关闭；敏感项（剪贴板、日历、AI 集成）默认关闭。
- poll() 只返回数据，不直接操作渲染层；单个 Provider 异常由 EventBus 隔离。

基类用 dataclass 声明 enabled / sensitive，使 dataclass 子类能正确继承字段初始化，
避免子类自动生成的 __init__ 覆盖基类初始化（首轮验证暴露的缺陷）。
"""
from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass

from core.activity import Activity


@dataclass
class ProviderUpdate:
    activity: Activity | None = None
    end_activity_id: str | None = None


@dataclass
class Provider(ABC):
    enabled: bool = True
    sensitive: bool = False

    def __post_init__(self) -> None:
        if self.sensitive and self.enabled:
            raise ValueError("sensitive providers must default to disabled")

    @property
    @abstractmethod
    def name(self) -> str: ...

    @abstractmethod
    def poll(self, now_ms: int) -> tuple[ProviderUpdate, ...]:
        """返回本轮增量；无变化时返回空元组（避免无效重绘）。"""
        ...

    def set_enabled(self, enabled: bool) -> None:
        self.enabled = enabled
