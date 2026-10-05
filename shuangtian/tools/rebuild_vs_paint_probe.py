"""量尺：把「重建画布/交换链」与「整帧重绘」拆成两个正交量。

拖动缩放每步要做两件事，它们的成本相差一个量级，混在一起就分不清优化该打哪里：
  A. **重建**：换画布（D3D11 纹理/渲染目标）、换 swapchain、换 DIB —— 只在尺寸变化时发生；
  B. **整帧重绘**：布局 + 绘制整树 + 送显 —— 与尺寸有关但与重建无关。

做法：同一尺寸下反复触发整帧（切主题），取 B；改一次尺寸后立刻取一帧，得到 A+B。
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from st_client_lib import call_with_token, load_control  # noqa: E402

u = ctypes.WinDLL("user32", use_last_error=True)
try:
    ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)
except Exception:
    u.SetProcessDPIAware()
u.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int,
                           ctypes.c_int, ctypes.c_int, ctypes.c_uint]
u.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u.EnumWindows.argtypes = [ctypes.c_void_p, wt.LPARAM]


def find(pid: int):
    found: list[int] = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    def cb(hwnd, _lp):
        o = wt.DWORD()
        u.GetWindowThreadProcessId(hwnd, ctypes.byref(o))
        if o.value == pid and u.IsWindowVisible(hwnd):
            r = wt.RECT()
            u.GetWindowRect(hwnd, ctypes.byref(r))
            if (r.right - r.left) * (r.bottom - r.top) > 0:
                found.append(hwnd)
        return True

    u.EnumWindows(cb, 0)
    return found[0] if found else None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=str(ROOT / "build/dev/bin/gallery.exe"))
    parser.add_argument("--renderer", default="")
    parser.add_argument("--sizes", default="900x600,1200x800,1600x1000")
    args = parser.parse_args()

    control = ROOT / "build/live-resize/ctl-rebuild.json"
    control.parent.mkdir(parents=True, exist_ok=True)
    if control.exists():
        control.unlink()
    command = [args.exe, "--control-file", str(control), "--theme", "light"]
    if args.renderer:
        command += ["--renderer", args.renderer]
    proc = subprocess.Popen(command, cwd=str(ROOT))
    try:
        info = {}
        for _ in range(120):
            time.sleep(0.2)
            info = load_control(str(control))
            if info.get("port") and info.get("pid"):
                break
        port, token, pid = info["port"], info.get("token", ""), info.get("pid")

        def call(method: str, params: dict | None = None) -> dict:
            reply = call_with_token(port, method, params, token, timeout=30)
            return reply.get("result", reply)

        time.sleep(1.2)
        hwnd = find(pid)
        metrics = call("metrics")
        print(f"renderer={metrics['renderer']} scale={metrics['device_scale']}")
        print(f"{'窗口(px)':>12} {'重建后一帧':>11} {'重建开销':>22} {'稳态整帧':>10} "
              f"{'layout':>8} {'paint':>8} {'present':>8} {'元素':>6}")

        rows = []
        for spec in args.sizes.split(","):
            width, height = (int(v) for v in spec.lower().split("x"))
            u.SetWindowPos(hwnd, wt.HWND(-1), 120, 120, width, height, 0x0040)
            # 抓"重建后第一帧"：SetWindowPos 后主循环下一轮就重建 + 重绘。
            # 判据用**物理尺寸 == SetWindowPos 给的尺寸**（无边框：客户区 == 整窗），
            # 而不是拿逻辑尺寸乘 scale（乘出来会因四舍五入对不上，永远抓不到）。
            # 采样必须稳健：单次取样会把"控制通道轮询把主循环节拍挤开"造成的
            # 偶发长帧当成重建代价（实测同一改动的单样本在 25~73ms 间跳）。
            # 取"尺寸已到位"的若干帧里的 **median**，并把重建开销单独拆出来。
            samples: list[dict] = []
            deadline = time.time() + 1.5
            while time.time() < deadline:
                m = call("metrics")
                if m["physical_width"] == width and m["physical_height"] == height:
                    samples.append(m)
                    if len(samples) >= 6:
                        break
                time.sleep(0.05)
            samples.sort(key=lambda item: item["last_frame_ms"])
            first = samples[len(samples) // 2] if samples else None
            time.sleep(0.4)
            steady = None
            for _ in range(4):
                mode = "dark" if call("metrics") else "dark"
                call("theme", {"mode": mode})
                time.sleep(0.25)
                m = call("metrics")
                if steady is None or m["paint_ms"] < steady["paint_ms"]:
                    steady = m
                call("theme", {"mode": "light"})
                time.sleep(0.25)
            row = {"size": spec,
                   "rebuild_frame_ms": round(first["last_frame_ms"], 2) if first else None,
                   "rebuild_overhead_ms": round(first["last_frame_ms"] - first["layout_ms"]
                                                - first["paint_ms"] - first["present_ms"], 2)
                   if first else None,
                   "steady_frame_ms": round(steady["last_frame_ms"], 2),
                   "steady_overhead_ms": round(steady["last_frame_ms"] - steady["layout_ms"]
                                               - steady["paint_ms"] - steady["present_ms"], 2),
                   "steady_layout_ms": round(steady["layout_ms"], 2),
                   "steady_paint_ms": round(steady["paint_ms"], 2),
                   "steady_present_ms": round(steady["present_ms"], 2),
                   "painted": steady.get("painted_elements")}
            rows.append(row)
            print(f"{spec:>12} {str(row['rebuild_frame_ms']):>11} "
                  f"({'重建开销 ' + str(row['rebuild_overhead_ms']) + ' ms' if first else 'n/a':>18}) "
                  f"{row['steady_frame_ms']:>10} {row['steady_layout_ms']:>8} "
                  f"{row['steady_paint_ms']:>8} {row['steady_present_ms']:>8} "
                  f"{str(row['painted']):>6}")

        (ROOT / "build/live-resize/rebuild-vs-paint.json").write_text(
            json.dumps({"renderer": metrics["renderer"], "rows": rows},
                       ensure_ascii=False, indent=2), encoding="utf-8")
        return 0
    finally:
        if proc.poll() is None:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
