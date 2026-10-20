"""真实界面的 paint_ms A/B 对比（交替采样，消除二进制间的系统性漂移）。

为何要交替：paint_ms 是"上一帧"的耗时，受 CPU 频率、缓存热态、限帧节奏影响，
单次对比容易被噪声或"哪个先跑"决定。做法是**同一个进程各测一组**不可能（代码已编进去），
所以退一步：两份二进制**交替重启**，每轮各采 N 个样本取中位，比较多轮的中位分布。

判据：报"接入组中位 / 基线组中位"的比值 + 两组各自的最小/最大，由人判是否显著。
"""

from __future__ import annotations

import json
import pathlib
import statistics
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "dev" / "bin" / "gbcode.exe"
WORK = ROOT / "build" / "paint-ab"
sys.path.insert(0, str(ROOT / "tools"))
import st_client_lib  # noqa: E402


def one_run(tag: str, samples: int = 10) -> list[float]:
    WORK.mkdir(parents=True, exist_ok=True)
    control = WORK / f"control-{tag}.json"
    control.unlink(missing_ok=True)
    proc = subprocess.Popen(
        [str(BIN), "--headless", "--control-port", "0", "--control-file", str(control)],
        cwd=str(BIN.parent),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        deadline = time.time() + 60
        info = {}
        while time.time() < deadline:
            if control.exists():
                try:
                    info = json.loads(control.read_text(encoding="utf-8"))
                    break
                except (json.JSONDecodeError, OSError):
                    pass
            time.sleep(0.3)
        if not info:
            return []
        time.sleep(2.0)  # 等首帧稳定
        port, token = int(info["port"]), info["token"]
        values: list[float] = []
        for index in range(samples):
            st_client_lib.call_with_token(
                port, "set", {"id": "editor-tabs", "props": {"active": index % 3}}, token
            )
            time.sleep(0.25)
            metrics = st_client_lib.call_with_token(port, "metrics", {}, token)["result"]
            values.append(float(metrics["paint_ms"]))
        return values
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


def main() -> int:
    if len(sys.argv) < 2:
        print("用法: py -3 tools/paint_ab_compare.py <baseline|covered> [轮数]")
        return 2
    tag = sys.argv[1]
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    all_medians: list[float] = []
    for round_index in range(rounds):
        values = one_run(f"{tag}-{round_index}")
        if not values:
            print(f"  第 {round_index + 1} 轮：控制通道未就绪")
            continue
        med = statistics.median(values)
        all_medians.append(med)
        print(f"  第 {round_index + 1} 轮：中位 {med:.2f} ms  "
              f"(min {min(values):.2f} / max {max(values):.2f})")
    if not all_medians:
        return 1
    print(f"\n[{tag}] 中位数的中位 = {statistics.median(all_medians):.2f} ms "
          f"（{len(all_medians)} 轮）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
