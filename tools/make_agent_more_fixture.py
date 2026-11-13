"""生成 Pi 与 ZCode 的假会话，用于验证「新增 provider」的完整链路。

为什么单独一个脚本：
  Pi 是 JSONL，ZCode 是 SQLite —— 两种形态的 fixture 没法放进同一个
  make_agent_fixture.py（那个只管 Claude 格式）。这里把两个新数据源
  一起造出来，跑一次就能在界面上看到两个新品牌色。

用法：
  python tools/make_agent_more_fixture.py build/agent_more
  diw_overlay.exe --agent-home=<abs path to build/agent_more>
"""
import json
import os
import sqlite3
import sys
import uuid
from datetime import datetime, timezone, timedelta

root = sys.argv[1] if len(sys.argv) > 1 else "build/agent_more"
now = datetime.now(timezone.utc)


def iso(dt):
    return dt.strftime("%Y-%m-%dT%H:%M:%S.") + f"{dt.microsecond // 1000:03d}Z"


def ms(dt):
    return int(dt.timestamp() * 1000)


# ---------------------------------------------------------------- Pi（JSONL）
pi_dir = os.path.join(root, ".pi", "agent", "sessions", "--D--work--order-service--")
os.makedirs(pi_dir, exist_ok=True)
sid = str(uuid.uuid4())
start = now - timedelta(minutes=9)
rows = [
    {"type": "session", "version": 3, "id": sid, "timestamp": iso(start),
     "cwd": "D:\\work\\order-service"},
    {"type": "model_change", "id": "m1", "timestamp": iso(start + timedelta(seconds=1)),
     "provider": "anthropic", "modelId": "claude-sonnet-4-5"},
    {"type": "message", "id": "u1", "timestamp": iso(start + timedelta(seconds=5)),
     "message": {"role": "user",
                 "content": [{"type": "text", "text": "把订单导出的 CSV 改成流式写，别一次性进内存"}],
                 "timestamp": ms(start + timedelta(seconds=5))}},
    {"type": "message", "id": "a1", "timestamp": iso(now - timedelta(seconds=18)),
     "message": {"role": "assistant", "modelId": "claude-sonnet-4-5",
                 "content": [
                     {"type": "thinking", "thinking": "先看导出实现"},
                     {"type": "text", "text": "导出目前是先把整批订单拼成一个大字符串，我改成按行写。"},
                     {"type": "toolCall", "id": "c1", "name": "read",
                      "arguments": {"path": "src/export/order_export.py"}},
                     {"type": "toolCall", "id": "c2", "name": "edit",
                      "arguments": {"path": "src/export/order_export.py"}},
                 ],
                 "timestamp": ms(now - timedelta(seconds=18))}},
    {"type": "message", "id": "r1", "timestamp": iso(now - timedelta(seconds=16)),
     "message": {"role": "toolResult", "toolName": "edit",
                 "content": [{"type": "text", "text": "patched"}],
                 "timestamp": ms(now - timedelta(seconds=16))}},
    {"type": "message", "id": "a2", "timestamp": iso(now - timedelta(seconds=3)),
     "message": {"role": "assistant", "content": [
         {"type": "toolCall", "id": "c3", "name": "bash",
          "arguments": {"command": "pytest tests/test_export.py -q"}}],
         "timestamp": ms(now - timedelta(seconds=3))}},
]
pi_path = os.path.join(pi_dir, f"{start.strftime('%Y-%m-%dT%H-%M-%S-%f')[:-3]}Z_{sid}.jsonl")
with open(pi_path, "w", encoding="utf-8", newline="\n") as f:
    for row in rows:
        f.write(json.dumps(row, ensure_ascii=False) + "\n")
print(pi_path)

# ---------------------------------------------------------------- ZCode（SQLite）
db_dir = os.path.join(root, ".zcode", "cli", "db")
os.makedirs(db_dir, exist_ok=True)
db_path = os.path.join(db_dir, "db.sqlite")
if os.path.exists(db_path):
    os.remove(db_path)
con = sqlite3.connect(db_path)
con.executescript("""
CREATE TABLE session (
  id text primary key, project_id text not null, workspace_id text, parent_id text,
  slug text not null, directory text not null, path text, title text not null,
  version text not null, share_url text, time_created integer not null, time_updated integer not null);
CREATE TABLE message (
  id text primary key, session_id text not null, time_created integer not null,
  time_updated integer not null, data text not null, sequence integer);
CREATE TABLE part (
  id text primary key, message_id text not null, session_id text not null,
  time_created integer not null, time_updated integer not null, data text not null, sequence integer);
CREATE TABLE todo (
  session_id text not null, content text not null, status text not null,
  priority text not null, position integer not null,
  time_created integer not null, time_updated integer not null);
""")

zsid = "sess_" + str(uuid.uuid4())
zstart = now - timedelta(minutes=14)
con.execute("insert into session values (?,?,?,?,?,?,?,?,?,?,?,?)",
            (zsid, "proj_1", None, None, "order-service", "D:\\work\\order-service", None,
             "补全订单导出接口", "0.1.0", None, ms(zstart), ms(now - timedelta(seconds=4))))
con.execute("insert into message values (?,?,?,?,?,?)",
            ("msg_1", zsid, ms(now - timedelta(seconds=20)), ms(now - timedelta(seconds=12)),
             json.dumps({"role": "assistant", "modelId": "glm-4.6", "providerId": "zai",
                         "time": {"created": ms(now - timedelta(seconds=20)),
                                  "completed": ms(now - timedelta(seconds=12))}},
                        ensure_ascii=False), 1))

parts = [
    ("part_1", {"type": "reasoning", "text": "先看现有路由"}),
    ("part_2", {"type": "tool", "tool": "Read", "state": {"status": "completed",
                                                          "input": {"file_path": "src/routes/order.py"}}}),
    ("part_3", {"type": "tool", "tool": "Edit", "state": {"status": "completed",
                                                          "input": {"file_path": "src/routes/order.py"}}}),
    ("part_4", {"type": "text", "text": "导出接口补上了分页参数，默认 500 条一页。"}),
    ("part_5", {"type": "tool", "tool": "Bash", "state": {"status": "running",
                                                          "input": {"command": "npm test -- order"}}}),
]
for i, (pid, data) in enumerate(parts):
    con.execute("insert into part values (?,?,?,?,?,?,?)",
                (pid, "msg_1", zsid, ms(now - timedelta(minutes=10)) + i * 1000,
                 ms(now - timedelta(minutes=10)) + i * 1000, json.dumps(data, ensure_ascii=False), i))

todos = [
    ("读现有导出实现", "completed", 0),
    ("改成分页 + 流式写出", "completed", 1),
    ("补分页参数的单元测试", "in_progress", 2),
    ("更新 API 文档", "pending", 3),
]
for content, status, pos in todos:
    con.execute("insert into todo values (?,?,?,?,?,?,?)",
                (zsid, content, status, "medium", pos, ms(zstart), ms(zstart)))
con.commit()
con.close()
print(db_path)
