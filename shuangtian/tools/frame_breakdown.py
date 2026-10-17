#!/usr/bin/env python3
"""帧分解对照：同一二进制在不同后端/尺寸下的 layout/paint/present 与帧耗时分位。

用途：Linux 上屏路径优化的**可复跑量尺**——改前后各跑一次，数字直接可比。
与 `tools/mem_sample.py` 同一姿态（一条命令复核基线），区别是这里量的是时间。

口径：
  - 每个组合新起一个进程（`--frames` 极大，靠 terminate 收尾），跑满 warmup 后取 `metrics`；
  - `metrics` 里的 `present_ms` / `paint_ms` / `layout_ms` 是**最后一帧**的分段
    （所以先 sleep 让帧稳态），`frame_p50/p95` 是环形缓冲的统计量；
  - 未显式给 `--scale` 时窗口按 Xft.dpi、无头按系统缩放——本脚本固定 `--scale 1.0`，
    排除 DPI 差异（口径一致才好比）。

用法：
    python3 tools/frame_breakdown.py                     # headless/x11 × 720p/1080p
    python3 tools/frame_breakdown.py --sizes 1920x1080 --backends x11
    python3 tools/frame_breakdown.py --renderer software --warmup 3
"""
from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from st_client_lib import call_with_token, load_control  # noqa: E402

KEYS = ("backend", "renderer", "device_scale", "physical_width", "physical_height", "frames",
        "layout_ms", "paint_ms", "present_ms", "last_frame_ms", "frame_p50_ms", "frame_p95_ms",
        "wall_p50_ms", "partial_frame")


def sample(exe: pathlib.Path, backend: str, width: int, height: int, renderer: str,
           warmup: float, out_dir: pathlib.Path) -> dict:
    ctl = out_dir / f"ctl-{backend}-{width}x{height}.json"
    if ctl.exists():
        ctl.unlink()
    cmd = [str(exe), "--backend", backend, "--width", str(width), "--height", str(height),
           "--scale", "1.0", "--renderer", renderer, "--control-port", "0",
           "--control-file", str(ctl), "--frames", "1000000"]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        info = {}
        for _ in range(200):
            info = load_control(str(ctl))
            if info.get("port"):
                break
            if proc.poll() is not None:
                return {"error": f"进程提前退出（code={proc.returncode}）"}
            time.sleep(0.05)
        if not info.get("port"):
            return {"error": "控制通道未就绪"}
        time.sleep(warmup)
        reply = call_with_token(info["port"], "metrics", {}, info.get("token", ""))
        result = reply.get("result", reply)
        return {key: result.get(key) for key in KEYS}
    except Exception as exc:  # noqa: BLE001
        return {"error": str(exc)}
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=str(ROOT / "build/dev/bin/gallery"))
    parser.add_argument("--backends", default="headless,x11")
    parser.add_argument("--sizes", default="1280x720,1920x1080")
    parser.add_argument("--renderer", default="software")
    parser.add_argument("--warmup", type=float, default=3.0)
    parser.add_argument("--json", default="")
    args = parser.parse_args()

    out_dir = ROOT / "build/frame-breakdown"
    out_dir.mkdir(parents=True, exist_ok=True)
    exe = pathlib.Path(args.exe)
    if not exe.exists():
        print(f"可执行文件不存在：{exe}", file=sys.stderr)
        return 2

    report: dict[str, dict] = {}
    for backend in args.backends.split(","):
        for size in args.sizes.split(","):
            width, height = (int(part) for part in size.split("x"))
            key = f"{backend}-{width}x{height}"
            report[key] = sample(exe, backend.strip(), width, height, args.renderer, args.warmup,
                                 out_dir)
            print(f"[{key}] {json.dumps(report[key], ensure_ascii=False)}", flush=True)

    print()
    header = f"{'组合':<22}{'layout':>9}{'paint':>9}{'present':>10}{'p50':>9}{'p95':>9}"
    print(header)
    print("-" * len(header))
    for key, row in report.items():
        if "error" in row:
            print(f"{key:<22}{row['error']}")
            continue
        print(f"{key:<22}{row['layout_ms']:>9.2f}{row['paint_ms']:>9.2f}{row['present_ms']:>10.2f}"
              f"{row['frame_p50_ms']:>9.2f}{row['frame_p95_ms']:>9.2f}")
    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(report, indent=2, ensure_ascii=False),
                                           encoding="utf-8")
        print(f"\n已写入 {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
