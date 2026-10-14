#!/usr/bin/env python3
"""同一二进制布局下的光栅改动**像素级等价**判据。

用途（2026-10-14）：`blend_coverage_runs` 的快路径重写要先证明"用户看到的画面不变"。
做法：对同一 gallery 场景取无头截图，与 `--reference` 给的基准 PNG 逐像素比对。

    python3 tools/raster_pixel_diff.py --exe build/release/bin/gallery --reference /tmp/a.png --out /tmp/b.png
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import st_client_lib as lib  # noqa: E402


def capture(exe: str, png: str) -> bool:
    ctl = f"/tmp/raster-diff-ctl-{os.getpid()}.json"
    if os.path.exists(ctl):
        os.remove(ctl)
    os.makedirs(os.path.dirname(png) or ".", exist_ok=True)
    if os.path.exists(png):
        os.remove(png)
    proc = subprocess.Popen([exe, "--headless", "--control-port", "0",
                             "--control-file", ctl, "--renderer", "software"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        info = {}
        for _ in range(200):
            time.sleep(0.1)
            info = lib.load_control(ctl)
            if info.get("port"):
                break
        if not info.get("port"):
            return False
        port, token = info["port"], info.get("token", "")
        time.sleep(1.5)
        reply = lib.call_with_token(port, "capture",
                                    {"encode": "file", "path": png}, token, timeout=60)
        ok = bool(reply.get("ok"))
        lib.call_with_token(port, "shutdown", {}, token, timeout=10)
        return ok and os.path.exists(png)
    finally:
        try:
            proc.wait(timeout=10)
        except Exception:
            proc.kill()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--reference", required=True)
    parser.add_argument("--out", required=True)
    parsed = parser.parse_args()

    if not capture(parsed.exe, parsed.out):
        print("截图失败", file=sys.stderr)
        return 2

    try:
        from PIL import Image
        import numpy as np
    except ImportError:
        # 无 PIL/numpy 时退回逐字节比较（PNG 压缩流一致即视为相同）
        same = open(parsed.reference, "rb").read() == open(parsed.out, "rb").read()
        print(f"逐字节比较：{'相同' if same else '不同'}（缺 PIL/numpy，只做流比较）")
        return 0 if same else 1

    a = np.asarray(Image.open(parsed.reference).convert("RGBA"), dtype=np.int16)
    b = np.asarray(Image.open(parsed.out).convert("RGBA"), dtype=np.int16)
    if a.shape != b.shape:
        print(f"尺寸不同：{a.shape} vs {b.shape}")
        return 1
    diff = np.abs(a - b)
    different = int((diff.max(axis=2) > 0).sum())
    total = a.shape[0] * a.shape[1]
    print(f"像素总数 {total} · 不同 {different}（{different / total * 100:.4f}%）· 最大通道差 {int(diff.max())}")
    return 0 if different == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
