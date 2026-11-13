"""生成一份假的 Agent 会话文件，用于截图回归「有任务清单的展开态」。

为什么需要 fixture：
  真实会话里 TodoWrite 可能整晚都不出现一次，靠「等一个真会写清单的会话」
  没法做可重复的视觉验证。这份文件的时间戳按“现在”生成，保证采集层判定为
  running，从而能稳定截到「运行中 + 3/4 完成」的画面。
"""
import json
import os
import sys
import uuid
from datetime import datetime, timezone, timedelta

root = sys.argv[1] if len(sys.argv) > 1 else "build/agent_fixture"
project = root + "/.claude/projects/island-demo"
os.makedirs(project, exist_ok=True)

now = datetime.now(timezone.utc)
# --idle=N：把最后一条事件推到 N 分钟前，用来截「等待输入」态。
# 不传就是「刚刚还在动」= running。
for _arg in sys.argv:
    if _arg.startswith("--idle="):
        now = now - timedelta(minutes=float(_arg.split("=", 1)[1]))
start = now - timedelta(minutes=12)


def iso(dt):
    return dt.strftime("%Y-%m-%dT%H:%M:%S.") + f"{dt.microsecond // 1000:03d}Z"


sid = str(uuid.uuid4())
cwd = "C:\\Users\\ow1\\WorkBuddy\\2026-11-12-14-12-37\\DynamicIsland-Win"

rows = []

rows.append({
    "type": "summary",
    "summary": "给灵动岛加编码 Agent 面板",
    "sessionId": sid,
    "cwd": cwd,
    "timestamp": iso(start),
})
rows.append({
    "type": "user",
    "sessionId": sid,
    "cwd": cwd,
    "message": {"role": "user", "content": "点开大岛要能看到 agent 在干什么"},
    "timestamp": iso(start + timedelta(seconds=2)),
})
rows.append({
    "type": "assistant",
    "sessionId": sid,
    "cwd": cwd,
    "message": {
        "role": "assistant",
        "model": "claude-sonnet-4-5",
        "usage": {"input_tokens": 48210, "output_tokens": 640},
        "content": [{"type": "text", "text": "我先把采集层和渲染层拆开，会话文件解析放在 core。"}],
    },
    "timestamp": iso(now - timedelta(minutes=9)),
})
rows.append({
    "type": "assistant",
    "sessionId": sid,
    "cwd": cwd,
    "message": {
        "role": "assistant",
        "model": "claude-sonnet-4-5",
        "content": [{
            "type": "tool_use",
            "name": "TodoWrite",
            "input": {"todos": [
                {"content": "解析 Claude Code 会话行", "status": "completed", "activeForm": "解析会话行"},
                {"content": "接入映射层与采集快照", "status": "completed", "activeForm": "接入映射层"},
                {"content": "画紧凑态与展开态面板", "status": "in_progress", "activeForm": "画面板"},
                {"content": "补四家格式的回归测试", "status": "pending", "activeForm": "补回归测试"},
            ]},
        }],
    },
    "timestamp": iso(now - timedelta(minutes=7)),
})
rows.append({
    "type": "assistant",
    "sessionId": sid,
    "cwd": cwd,
    "message": {
        "role": "assistant",
        "model": "claude-sonnet-4-5",
        "content": [{
            "type": "tool_use",
            "name": "Edit",
            "input": {"file_path": cwd + "\\src\\win32\\renderer.hpp"},
        }],
    },
    "timestamp": iso(now - timedelta(seconds=40)),
})
rows.append({
    "type": "assistant",
    "sessionId": sid,
    "cwd": cwd,
    "message": {
        "role": "assistant",
        "model": "claude-sonnet-4-5",
        "content": [{
            "type": "tool_use",
            "name": "Bash",
            "input": {"command": "cd \"C:/Users/ow1/WorkBuddy/DynamicIsland-Win\" && "
                                 "tools/build_cpp_msvc.sh", "description": "构建并跑测试"},
        }],
    },
    "timestamp": iso(now),
})

path = os.path.join(project, sid + ".jsonl")
with open(path, "w", encoding="utf-8", newline="\n") as f:
    for row in rows:
        f.write(json.dumps(row, ensure_ascii=False) + "\n")

print(path)
print("lines:", len(rows))

# ---------------------------------------------------------------- 第二个 fixture
# 同时开两个 Agent 是常态（一个改前端、一个跑测试）。这里再放一份 Codex 会话，
# 用来验证「多岛 / 一览 / 分离小圆」在 agent 场景下也成立。
if "--two" in sys.argv:
    cwd2 = "D://work//api-server"
    project2 = root + "/.codex/sessions/2026/11/12"
    os.makedirs(project2, exist_ok=True)
    sid2 = "rollout-" + str(uuid.uuid4())
    rows2 = [
        {"timestamp": iso(now - timedelta(minutes=6)), "type": "session_meta",
         "payload": {"id": sid2, "cwd": cwd2}},
        {"timestamp": iso(now - timedelta(minutes=6) + timedelta(seconds=1)),
         "type": "turn_context", "payload": {"cwd": cwd2, "model": "gpt-5-codex"}},
        {"timestamp": iso(now - timedelta(minutes=5)), "type": "response_item",
         "payload": {"type": "message", "role": "user",
                     "content": [{"type": "input_text", "text": "把健康检查接口补上"}]}},
        {"timestamp": iso(now - timedelta(seconds=20)), "type": "response_item",
         "payload": {"type": "function_call", "name": "update_plan",
                     "arguments": json.dumps({"plan": [
                         {"step": "读路由定义", "status": "completed"},
                         {"step": "加 /healthz", "status": "in_progress"},
                         {"step": "补集成测试", "status": "pending"},
                     ]}, ensure_ascii=False)}},
        {"timestamp": iso(now - timedelta(seconds=2)), "type": "response_item",
         "payload": {"type": "function_call", "name": "shell",
                     "arguments": json.dumps({"command": ["bash", "-lc", "npm test"]})}},
    ]
    path2 = os.path.join(project2, sid2 + ".jsonl")
    with open(path2, "w", encoding="utf-8", newline="\n") as f:
        for row in rows2:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")
    print(path2)
