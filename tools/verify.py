"""一键验证入口：单元测试 + 跨语言契约校验。

用法：python tools/verify.py
退出码 0 表示全部通过；非 0 表示有失败，不可据此声称功能已验收。
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

from check_contracts import check  # noqa: E402


def main() -> int:
    print("=" * 62)
    print("DynamicIsland-Win / local verification")
    print(f"python: {sys.version.split()[0]}  root: {ROOT}")
    print("=" * 62)

    suite = unittest.TestLoader().discover(str(ROOT / "tests"), pattern="test_*.py")
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    tests_ok = result.wasSuccessful()
    print(f"\nunit tests: run={result.testsRun} "
          f"failures={len(result.failures)} errors={len(result.errors)}")

    print("\n-- cross-language contract check --")
    passed, failed = check()
    for item in passed:
        print(f"  PASS  {item}")
    for item in failed:
        print(f"  FAIL  {item}")
    print(f"contracts: {len(passed)} passed, {len(failed)} failed")

    print("\n" + "=" * 62)
    ok = tests_ok and not failed
    print(f"RESULT: {'PASS' if ok else 'FAIL'}")
    print("SCOPE: Python spec + cross-language contracts only.")
    print("For C++ build and unit tests run: bash tools/build_cpp_msvc.sh")
    print("=" * 62)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
