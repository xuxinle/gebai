#!/usr/bin/env python3
"""量尺自检：两路通道各自**连续两次**取值是否一致？

判据的先后顺序很重要：如果「画布」与「合成」两路本身就不稳定，
那么把它们互比得到的差异不能归因于窗框——那只是时序噪声。

用法：python tools/window_channel_stability_check.py [--exe PATH] [--repeat 3]
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_EXE = ROOT / "build/debug/bin/gallery.exe"
sys.path.insert(0, str(ROOT / "tools"))
from st_client_lib import call_with_token, load_control  # noqa: E402
from window_chrome_measure import find_window, print_window  # noqa: E402

USER32 = ctypes.WinDLL("user32", use_last_error=True)
USER32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=str(DEFAULT_EXE))
    parser.add_argument("--out", default=str(ROOT / "build/win-channel-stability"))
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--delay", type=float, default=3.0)
    args = parser.parse_args()
    exe = pathlib.Path(args.exe)
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    ctl = out / "control.json"
    if ctl.exists():
        ctl.unlink()
    proc = subprocess.Popen([str(exe), "--control-file", str(ctl), "--frames", "0"], cwd=str(ROOT))
    try:
        handle = load_control(ctl)
        deadline = time.time() + 30
        while not handle and time.time() < deadline:
            time.sleep(0.2)
            handle = load_control(ctl)
        if not handle:
            print("控制文件未出现")
            return 3
        time.sleep(args.delay)
        hwnd = find_window(proc.pid)
        if not hwnd:
            print("找不到窗口")
            return 3
        from PIL import Image, ImageChops  # noqa: PLC0415

        def canvas_capture(tag: str):
            path = out / f"canvas_{tag}.png"
            if path.exists():
                path.unlink()
            call_with_token(handle["port"], "capture", {"path": str(path)}, handle.get("token", ""))
            return Image.open(path).convert("RGB") if path.exists() else None

        def composed(tag: str):
            rect = RECT()
            USER32.GetWindowRect(hwnd, ctypes.byref(rect))
            w, h = rect.right - rect.left, rect.bottom - rect.top
            ok, img = print_window(hwnd, w, h)
            img.save(out / f"composed_{tag}.png")
            return img

        stamps = []
        for i in range(args.repeat):
            stamps.append((canvas_capture(f"c{i}"), composed(f"p{i}")))
            time.sleep(0.6)
        print(f"窗口 {stamps[0][1].size}")

        def diff_count(a, b) -> int:
            if a.size != b.size:
                return -1
            d = ImageChops.difference(a, b)
            dp = d.load()
            w, h = a.size
            return sum(1 for y in range(0, h, 2) for x in range(0, w, 2) if max(dp[x, y]) > 2)

        print("\n[画布通道自身稳定性]（相邻两次 capture 互比）")
        for i in range(1, len(stamps)):
            print(f"  c{i - 1} vs c{i}: {diff_count(stamps[i - 1][0], stamps[i][0])}")
        print("\n[合成通道自身稳定性]（相邻两次 PrintWindow 互比）")
        for i in range(1, len(stamps)):
            print(f"  p{i - 1} vs p{i}: {diff_count(stamps[i - 1][1], stamps[i][1])}")
        print("\n[两路互比]（同一轮内）")
        for i, (canvas, comp) in enumerate(stamps):
            print(f"  第 {i} 轮: {diff_count(canvas, comp)}")
        # 再看「最早一轮的合成」 vs 「最晚一轮的画布」——若差异随时间收敛，说明是瞬态
        print(f"\n  最早合成 vs 最晚画布: {diff_count(stamps[0][1], stamps[-1][0])}")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
