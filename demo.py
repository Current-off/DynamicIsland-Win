"""可运行行为演示：核心域从活动创建到降级、避让的完整链路。

用法：python demo.py
本演示只依赖标准库，不访问任何 Windows API、网络或用户数据。
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from core.activity import Activity
from core.runtime import ActivityRuntime
from core.state_machine import CapsuleState


def show(rt: ActivityRuntime, label: str, now_ms: int) -> None:
    result = rt.tick(now_ms)
    print(f"\n[{label}] state={rt.state_machine.state.value} "
          f"visible={len(result.visible)} suppressed={result.suppressed_count} "
          f"expired={list(result.expired_ids)}")
    if not result.visible:
        print("  (胶囊隐藏)")
    for item in result.visible:
        print(f"  - {item.activity.id:<20} {item.presentation.value:<9} "
              f"region={item.region:<16} score={item.activity.relevance_score}")


def main() -> int:
    print("=" * 62)
    print("balance_island / 核心域行为演示")
    print("=" * 62)

    rt = ActivityRuntime()
    rt.bus.subscribe(
        "diagnostics",
        lambda topic, payload: print(f"  event -> {topic}: {payload}"),
    )

    show(rt, "启动：无活动", 0)

    rt.start_activity(Activity(
        id="media:now-playing", kind="media", title=" playing ",
        relevance_score=80.0, created_at_ms=10,
        content={"artist": "demo", "playing": True},
    ))
    show(rt, "媒体开始播放", 100)

    rt.start_activity(Activity(
        id="bridge.calendar:next", kind="bridge.calendar", title="下一个会议",
        relevance_score=90.0, created_at_ms=20,
    ))
    show(rt, "日历活动加入（高优先级）", 200)

    rt.start_activity(Activity(
        id="system:battery", kind="system", title="电量 20%",
        relevance_score=60.0, created_at_ms=30,
    ))
    show(rt, "第三个活动（超出最多 2 个可见）", 300)

    rt.set_bridge_online(False)
    show(rt, "桥接进程离线：内置 Provider 仍可显示", 400)

    rt.set_bridge_online(True)
    rt.set_suspended(True)
    show(rt, "全屏 / 反作弊场景：隐藏并暂停采样", 500)

    rt.set_suspended(False)
    rt.start_activity(Activity(
        id="media:download", kind="media", title="下载中",
        relevance_score=85.0, created_at_ms=510, stale_at_ms=800,
    ))
    show(rt, "下载活动（800ms 后过期）", 600)
    show(rt, "到达 staleDate：自动结束并重调度", 800)

    print("\n" + "=" * 62)
    print(f"最终胶囊状态: {rt.state_machine.state.value}")
    print(f"状态转移记录: {len(rt.state_machine.history)} 条")
    print("说明: 本演示为平台无关核心域；C++ 渲染层未编译、未实机验收。")
    print("=" * 62)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
