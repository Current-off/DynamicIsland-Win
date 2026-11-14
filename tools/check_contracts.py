"""跨语言契约一致性校验（确定性静态抽取 + 比对）。

沿用 ZenDesktop 的 check_*.py 方法论：从 C++ 头文件抽取常量，
与 Python 参考实现的常量逐一比对，防止两侧设计 token 漂移。
不编译 C++，只校验契约数值；结果不能替代编译或实机验收。

v0.2.0 起新增平衡岛范式三方校验：
    contracts/BalanceRules.hpp  ←→  core/balance.py  ←→  contracts/balance-paradigm.json
规则必须是可断言的数字，不能只写在 docs/PARADIGM.md 里。
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from bridge.protocol import PROTOCOL_VERSION  # noqa: E402
from core.tokens import DEFAULT_TOKENS  # noqa: E402
from core.balance import BALANCE, as_dict  # noqa: E402


CPP_CONSTANTS = {
    "kProtocolVersion": ("int", PROTOCOL_VERSION),
    "kMaxVisibleActivities": ("int", 4),
    "kAnimationMaxMs": ("int", DEFAULT_TOKENS.animation_max_ms),
    "kCornerRadius": ("double", DEFAULT_TOKENS.corner_radius),
    "kConcentricMargin": ("double", DEFAULT_TOKENS.concentric_margin),
    "kExpandedMinHeight": ("double", DEFAULT_TOKENS.expanded_min_height),
    "kExpandedMaxHeight": ("double", DEFAULT_TOKENS.expanded_max_height),
}


REQUIRED_GESTURES = (
    "single_click", "double_click", "detached_dot", "title",
    "transport", "close_x", "hold_ctrl", "context_menu", "fullscreen",
)


def extract_cpp_constants(source: str) -> dict[str, float]:
    pattern = re.compile(
        r"constexpr\s+(int|double)\s+(\w+)\s*=\s*([0-9]+(?:\.[0-9]+)?)\s*;"
    )
    return {
        name: float(value)
        for _type, name, value in pattern.findall(source)
    }


def _check_header(header: Path, mapping: dict[str, float],
                  passed: list[str], failed: list[str]) -> None:
    if not header.is_file():
        failed.append(f"missing header: {header}")
        return
    found = extract_cpp_constants(header.read_text(encoding="utf-8"))
    for name, expected in mapping.items():
        if name not in found:
            failed.append(f"{name}: not declared in {header.name}")
        elif found[name] != float(expected):
            failed.append(f"{name}: C++={found[name]} != contract={expected}")
        else:
            passed.append(f"{name}={expected}")


def _check_balance(passed: list[str], failed: list[str]) -> None:
    """范式三方校验：C++ 常量 / Python 常量 / JSON 资源必须一致。"""
    spec_path = ROOT / "contracts" / "balance-paradigm.json"
    if not spec_path.is_file():
        failed.append("missing balance-paradigm.json")
        return
    spec = json.loads(spec_path.read_text(encoding="utf-8"))
    py_rules = as_dict()

    # 1) JSON → Python 字段逐一对应，且值相等（防止文档漂移）
    json_rules = spec.get("rules", {})
    for key, value in json_rules.items():
        if key not in py_rules:
            failed.append(f"balance rule '{key}' in JSON but not in core/balance.py")
        elif float(py_rules[key]) != float(value):
            failed.append(f"balance rule '{key}': JSON={value} != Python={py_rules[key]}")
        else:
            passed.append(f"balance.json/{key}={value}")

    # 2) 每条规则都必须有 C++ 常量映射，且该常量存在、值相等
    cpp_map = spec.get("cpp_constant_map", {})
    header = ROOT / "contracts" / "BalanceRules.hpp"
    if not header.is_file():
        failed.append("missing BalanceRules.hpp")
        return
    cpp_found = extract_cpp_constants(header.read_text(encoding="utf-8"))
    for key, py_value in py_rules.items():
        const = cpp_map.get(key)
        if not const:
            failed.append(f"balance rule '{key}' has no C++ constant mapping")
        elif const not in cpp_found:
            failed.append(f"{const}: not declared in BalanceRules.hpp")
        elif cpp_found[const] != float(py_value):
            failed.append(f"{const}: C++={cpp_found[const]} != Python={py_value}")
        else:
            passed.append(f"{const}={py_value}")

    # 3) 交互语义表必须完整 —— 范式不是只有数字
    interaction = spec.get("interaction", {})
    for gesture in REQUIRED_GESTURES:
        if not interaction.get(gesture):
            failed.append(f"missing interaction semantics: {gesture}")
        else:
            passed.append(f"gesture/{gesture}")

    # 4) 纯函数行为断言：动效上限、让路、安静态曲线边界、分级回落
    from core.balance import clip_transition_ms, info_level, quiet_alpha, yield_opacity

    if clip_transition_ms(900) != BALANCE.transition_max_ms:
        failed.append("clip_transition_ms does not clamp to paradigm limit")
    else:
        passed.append("balance/clip_transition_ms")
    if clip_transition_ms(900, reduce_motion=True) != 0:
        failed.append("reduce_motion must degrade to fade (0)")
    else:
        passed.append("balance/reduce_motion")
    lo, hi = quiet_alpha(0.0), quiet_alpha(0.5)
    if not (BALANCE.quiet_alpha_min - 1e-9 <= lo <= hi <= BALANCE.quiet_alpha_max + 1e-9):
        failed.append(f"quiet_alpha out of range: {lo}..{hi}")
    else:
        passed.append("balance/quiet_alpha_range")
    if abs(yield_opacity(1.0) - BALANCE.yield_opacity) > 1e-9:
        failed.append("yield_opacity does not apply paradigm coefficient")
    else:
        passed.append("balance/yield_opacity")
    if info_level(True, True, True) != 3 or info_level(False, False, False) != 0:
        failed.append("info_level must support L0..L3 with L3 able to fall back")
    else:
        passed.append("balance/info_level")


def check() -> tuple[list[str], list[str]]:
    """返回 (通过项, 失败项)。缺常量、值不符均记为失败，不静默跳过。"""
    passed: list[str] = []
    failed: list[str] = []
    # CPP_CONSTANTS 的值是 (类型, 期望值)，_check_header 只需要期望值
    legacy = {name: expected for name, (_type, expected) in CPP_CONSTANTS.items()}
    _check_header(ROOT / "contracts" / "Types.hpp", legacy, passed, failed)
    _check_balance(passed, failed)
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
