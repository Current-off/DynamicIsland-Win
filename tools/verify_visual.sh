#!/usr/bin/env bash
# 视觉证据一键生成：编译 → 跑三态快照 → 抓桌面实证。
# 产物在 build/：snap_compact.png / snap_minimal.png / snap_expanded.png / desktop_proof.png
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

EXE="build/cpp/diw_overlay.exe"
if [ ! -f "$EXE" ]; then
  echo "=== 未找到 $EXE，先构建 ==="
  bash tools/build_cpp_msvc.sh
fi

mkdir -p build

echo "=== 紧凑态（媒体播放，t=1.7s）==="
./"$EXE" --duration=2600 --no-hud --snapshot=build/snap_compact.png --snapshot-at=1700 \
  --log=build/run_compact.log

echo "=== 极简态（日历抢占 + 分离小圆，t=3.4s）==="
./"$EXE" --duration=4200 --no-hud --snapshot=build/snap_minimal.png --snapshot-at=3400 \
  --log=build/run_minimal.log

echo "=== 展开态（t=4.6s）+ 桌面实证 ==="
./"$EXE" --duration=7000 --no-hud --snapshot=build/snap_expanded.png --snapshot-at=4600 \
  --grab=build/desktop_proof.png --log=build/run_expanded.log

echo ""
echo "=== 产物 ==="
ls -la build/snap_*.png build/desktop_proof.png
echo ""
echo "=== 状态转移（展开态那轮）==="
grep -E "state=|action=|snapshot|grab" build/run_expanded.log
