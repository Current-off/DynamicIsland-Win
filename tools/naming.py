#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""balance_island 更名工具：一次性替换 + 长期残留审计。

用法：
    python tools/naming.py --apply     # 执行全仓更名（排除 .git/build/缓存）
    python tools/naming.py --check     # 审计旧称残留，有残留则退出码 1
    python tools/naming.py --dry-run   # 只打印将要改动的文件与次数

设计要点：
  1. 有序映射，长串在前，避免子串互相覆盖（如 diw:: 与 diw_overlay）。
  2. GitHub 仓库地址是**外部真实 URL**，仓库本身未改名，必须原样保留；
     替换前先占位、替换后还原。
  3. docs/RENAME.md 是更名对照文档，按设计需要引用旧称，列入白名单。
  4. 本脚本自身不参与替换。
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXCLUDE_DIRS = {".git", "build", "vcache_backup", "__pycache__", "node_modules", "dist"}
EXCLUDE_FILES = {"tools/naming.py", "docs/RENAME.md"}
TEXT_SUFFIXES = {
    ".hpp", ".cpp", ".h", ".c", ".cc", ".cs", ".py", ".ps1", ".sh", ".cmd", ".bat",
    ".md", ".txt", ".json", ".yml", ".yaml", ".toml", ".ini", ".cfg", ".rsp",
    ".svg", ".html", ".css", ".js", ".ts", ".jsx", ".tsx", ".cmake", ".gitignore",
}
SELF_NAME = Path(__file__).name

# 产品可见名 / 标识 / 路径，长串在前
MAPPINGS: list[tuple[str, str]] = [
    ("DynamicIsland-Win-Overlay-SingleInstance", "BalanceIsland-Overlay-SingleInstance"),
    ("DynamicIslandWin-v1", "BalanceIsland-v1"),
    ("DynamicIslandWin", "BalanceIsland"),
    ("DynamicIsland-Win", "balance_island"),
    # 注意：不全局替换 "Dynamic Island" / "灵动岛" / "动态岛"。
    # 它们在文档里大多指 Apple 的功能或整个品类（例如「区别于对灵动岛的
    # 简单复刻」），属于**术语**而非本项目旧称。一刀切替换会写出
    #「区别于对平衡岛简单复刻」这种语义反转的句子。指代本项目的短语
    # 在 docs/RENAME.md 的对照表里逐条人工判定，见该文件。
    ("DIW_OverlayWindow", "BI_OverlayWindow"),
    ("DIW_SettingsWindow", "BI_SettingsWindow"),
    ("DIW_WhiteBackground", "BI_WhiteBackground"),
    ("DIW_CONTRACTS_", "BI_CONTRACTS_"),
    ("DIW_SRC_", "BI_SRC_"),
    ("diw-overlay.crash", "balance-island.crash"),
    ("diw-overlay", "balance-island"),
    ("diw_overlay", "balance_island"),
    ("diw::", "bi::"),
    ("namespace diw", "namespace bi"),
    ("namespace win32", "namespace win32"),  # 显式保留，避免误伤
]

# 旧称判定（审计用）：只针对本项目的品牌标识与内部标识符。
# "灵动岛"/"Dynamic Island" 是品类术语，允许出现在范式文档中。
LEGACY_PATTERNS = [
    "DynamicIsland-Win", "DynamicIslandWin",
    "diw_overlay", "diw-overlay", "DIW_", "namespace diw", "diw::",
]

REPO_URL_TOKEN = "Current-off/DynamicIsland-Win"
PLACEHOLDER = "__BI_REPO_URL__"

# 允许出现旧称的位置（外部真实地址 / 更名对照文档）
ALLOW_RULES = [
    lambda text: "github.com/" + REPO_URL_TOKEN in text,
    lambda text: "git@github.com:" + REPO_URL_TOKEN in text,
    # 迁移/清理代码里刻意保留的旧标识（v0.1.x 目录名、进程名、管道名），
    # 必须在同一行带 BI_LEGACY_ 标记，避免把审计白名单开成无底洞。
    lambda text: "BI_LEGACY_" in text,
    # 调研文档里引用**第三方仓库的真实名称**（如某项目叫 DynamicIsland-Windows），
    # 恰好与本项目旧名前缀重合。这不是残留，必须逐行显式标记。
    lambda text: "BI_CITATION_" in text,
]


def iter_files():
    for p in sorted(ROOT.rglob("*")):
        if not p.is_file():
            continue
        rel = p.relative_to(ROOT).as_posix()
        if any(part in EXCLUDE_DIRS for part in p.relative_to(ROOT).parts):
            continue
        if rel in EXCLUDE_FILES or p.name == SELF_NAME:
            continue
        if p.suffix.lower() not in TEXT_SUFFIXES and p.name not in {"CMakeLists.txt"}:
            continue
        if p.suffix.lower() == "" and p.name not in {"LICENSE", "CMakeLists.txt"}:
            continue
        yield p, rel


def read(p: Path) -> str | None:
    try:
        return p.read_text(encoding="utf-8-sig")
    except (UnicodeDecodeError, OSError):
        return None


def transform(text: str) -> tuple[str, int]:
    protected = text.replace(REPO_URL_TOKEN, PLACEHOLDER)
    hits = 0
    for old, new in MAPPINGS:
        if old == new:
            continue
        n = protected.count(old)
        if n:
            protected = protected.replace(old, new)
            hits += n
    return protected.replace(PLACEHOLDER, REPO_URL_TOKEN), hits


def main() -> int:
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--apply", action="store_true")
    g.add_argument("--check", action="store_true")
    g.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    total_files, total_hits, leftovers = 0, 0, []
    for p, rel in iter_files():
        text = read(p)
        if text is None:
            continue
        if args.check:
            for i, line in enumerate(text.splitlines(), 1):
                if any(pat in line for pat in LEGACY_PATTERNS):
                    if any(rule(line) for rule in ALLOW_RULES):
                        continue
                    leftovers.append((rel, i, line.strip()[:110]))
            continue
        new_text, hits = transform(text)
        if hits:
            total_files += 1
            total_hits += hits
            if args.apply:
                p.write_text(new_text, encoding="utf-8", newline="")
            else:
                print(f"  {rel}: {hits}")

    if args.check:
        if leftovers:
            print(f"旧称残留 {len(leftovers)} 处：")
            for rel, line, snippet in leftovers[:40]:
                print(f"  {rel}:{line}  {snippet}")
            return 1
        print("OK：无旧称残留（已按白名单排除 GitHub 仓库地址与更名对照文档）")
        return 0

    verb = "已替换" if args.apply else "将替换"
    print(f"{verb} {total_hits} 处，涉及 {total_files} 个文件")
    return 0


if __name__ == "__main__":
    sys.exit(main())
