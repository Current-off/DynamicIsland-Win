"""跨语言契约一致性校验（确定性静态抽取 + 比对）。

沿用 ZenDesktop 的 check_*.py 方法论：从 C++ 头文件抽取常量，
与 Python 参考实现的常量逐一比对，防止两侧设计 token 漂移。
不编译 C++，只校验契约数值；结果不能替代编译或实机验收。
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from bridge.protocol import PROTOCOL_VERSION  # noqa: E402
from core.tokens import DEFAULT_TOKENS  # noqa: E402


CPP_CONSTANTS = {
    "kProtocolVersion": ("int", PROTOCOL_VERSION),
    "kMaxVisibleActivities": ("int", 4),
    "kAnimationMaxMs": ("int", DEFAULT_TOKENS.animation_max_ms),
    "kCornerRadius": ("double", DEFAULT_TOKENS.corner_radius),
    "kConcentricMargin": ("double", DEFAULT_TOKENS.concentric_margin),
    "kExpandedMinHeight": ("double", DEFAULT_TOKENS.expanded_min_height),
    "kExpandedMaxHeight": ("double", DEFAULT_TOKENS.expanded_max_height),
}


def extract_cpp_constants(source: str) -> dict[str, float]:
    pattern = re.compile(
        r"constexpr\s+(int|double)\s+(\w+)\s*=\s*([0-9]+(?:\.[0-9]+)?)\s*;"
    )
    return {
        name: float(value)
        for _type, name, value in pattern.findall(source)
    }


def check() -> tuple[list[str], list[str]]:
    """返回 (通过项, 失败项)。缺常量、值不符均记为失败，不静默跳过。"""
    passed: list[str] = []
    failed: list[str] = []
    header = ROOT / "contracts" / "Types.hpp"
    if not header.is_file():
        return passed, [f"missing header: {header}"]

    found = extract_cpp_constants(header.read_text(encoding="utf-8"))
    for name, (_type, expected) in CPP_CONSTANTS.items():
        if name not in found:
            failed.append(f"{name}: not declared in Types.hpp")
        elif found[name] != float(expected):
            failed.append(f"{name}: C++={found[name]} != contract={expected}")
        else:
            passed.append(f"{name}={expected}")
    return passed, failed


def main() -> int:
    passed, failed = check()
    for item in passed:
        print(f"  PASS  {item}")
    for item in failed:
        print(f"  FAIL  {item}")
    print(f"contracts: {len(passed)} passed, {len(failed)} failed")
    return 0 if not failed else 1


if __name__ == "__main__":
    raise SystemExit(main())
