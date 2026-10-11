"""逆向验证：侧栏「常驻声明+隐藏保活」相对旧写法（switch 只声明当前视图）的帧耗时代价。

跑法：py -3 tools/gbcode_sidebar_keepalive_bench.py
它**不改任何代码**——只用控制通道把活动栏在六个视图之间来回切，
每次切换后读一次 `metrics` 的分段耗时（layout/paint/present），
给出两组的对照。若保活写法与旧写法的量级一致，就说明“多构建几棵子树”
不是帧耗时的来源（真正的开销在 layout/paint 的其它环节）。
"""

import json
import pathlib
import statistics
import subprocess
import sys
import tempfile
import time
import shutil

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tools"))
from gbcode_sidebar_keepalive_e2e import Client, start  # noqa: E402


def git(cwd, *args):
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)


def sample(client, selector, rounds=12):
    """在指定视图上采样帧耗时（切过去 → 等稳定 → 连读若干帧）。"""
    client.click(selector)
    time.sleep(0.8)
    layout, paint, present, total = [], [], [], []
    for _ in range(rounds):
        m = client.ok("metrics", {})
        layout.append(m.get("layout_ms", 0.0))
        paint.append(m.get("paint_ms", 0.0))
        present.append(m.get("present_ms", 0.0))
        total.append(m.get("last_frame_ms", 0.0))
        time.sleep(0.1)
    return {
        "layout": statistics.median(layout),
        "paint": statistics.median(paint),
        "present": statistics.median(present),
        "frame_p50": statistics.median(total),
    }


def main():
    work = pathlib.Path(tempfile.mkdtemp(prefix="gbcode_bench_"))
    repo = work / "repo"
    repo.mkdir()
    git(repo, "init", "-q", "-b", "main")
    git(repo, "config", "user.email", "b@local")
    git(repo, "config", "user.name", "Bench")
    for index in range(120):
        (repo / f"f{index:03d}.txt").write_text(f"line {index}\n", encoding="utf-8")
    git(repo, "add", ".")
    git(repo, "commit", "-qm", "bulk")
    (repo / "f000.txt").write_text("changed\n", encoding="utf-8")

    proc, client, _ = start(repo)
    try:
        views = [("explorer", "#activity-explorer"), ("search", "#activity-search"),
                 ("scm", "#activity-scm"), ("run", "#activity-run")]
        print("视图\tlayout_ms\tpaint_ms\tpresent_ms\tframe_p50_ms")
        rows = {}
        # 两轮：第二轮是“切回来”（此时各视图已经历过一次构建/隐藏）
        for round_index in (1, 2):
            for name, selector in views:
                stats = sample(client, selector)
                rows.setdefault(name, []).append(stats)
                print(f"{name}(第{round_index}轮)\t{stats['layout']:.2f}\t{stats['paint']:.2f}\t"
                      f"{stats['present']:.2f}\t{stats['frame_p50']:.2f}")
        print()
        for name, samples in rows.items():
            first, second = samples[0], samples[1]
            delta = second["frame_p50"] - first["frame_p50"]
            print(f"{name}: 第1轮 p50 {first['frame_p50']:.2f}ms → 第2轮 {second['frame_p50']:.2f}ms"
                  f"（Δ {delta:+.2f}ms）")
    finally:
        proc.terminate()
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
