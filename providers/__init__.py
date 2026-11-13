"""Provider 层：内置系统源与插件化接口。"""
from .base import Provider, ProviderUpdate
from .builtin import MediaProvider, SystemStatusProvider

__all__ = ["Provider", "ProviderUpdate", "MediaProvider", "SystemStatusProvider"]
