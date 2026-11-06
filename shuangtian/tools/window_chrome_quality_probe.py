#!/usr/bin/env python3
"""窗框三问的量尺：① 圆角边缘有多少级灰度过渡（越少越"台阶"）② 启动瞬间影子层在不在。

## 为什么需要它

用户报「圆角不光滑」「阴影要拖动一下才出现」。两者都是**观感/时序**问题，
不能靠读代码下结论：

- 圆角光滑度 = 边缘从"桌面色"到"窗口色"之间的**过渡像素级数**。
  `SetWindowRgn` 生成的是一**位**区域（在区域内/在区域外），所以过渡级数应该是
  **0 或 1** —— 这就是"台阶"的量化证据。
- 启动瞬间的影子 = 主窗可见的**那一刻**桌面像素里有没有变暗。拖动之后有、
  启动时没有，说明影子层的首次显示依赖后续事件。

## 判据

| 项 | 判据 |
|---|---|
| 圆角过渡级数 | AA 应有 ≥3 级中间灰；==0/1 级即"台阶"（复现用户所报） |
| 启动那一刻影子 | 主窗可见后立即采样，窗外 3..12px 应已变暗 |

用法：python tools/window_chrome_quality_probe.py
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from window_chrome_measure import grab_desktop, find_window, print_window  # noqa: E402
from st_client_lib import load_control, call_with_token  # noqa: E402

USER32 = ctypes.WinDLL("user32", use_last_error=True)
GDI32 = ctypes.WinDLL("gdi32", use_last_error=True)
USER32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
HWND_TOPMOST = ctypes.c_void_p(-1)
SWP_NOACTIVATE = 0x0010


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


def main() -> int:
    exe = ROOT / "build/debug/bin/gbcode.exe"
    out = ROOT / "build/chrome-quality"
    out.mkdir(parents=True, exist_ok=True)
    ctl = out / "control.json"
    if ctl.exists():
        ctl.unlink()

    proc = subprocess.Popen([str(exe), "--control-file", str(ctl), "--frames", "0"],
                            cwd=str(ROOT))
    try:
        # —— ② 启动那一刻：尽量早地抓桌面，看影子在不在 ——
        h = load_control(ctl)
        t0 = time.time()
        while not h and time.time() - t0 < 30:
            time.sleep(0.05)
            h = load_control(ctl)
        if not h:
            print("控制文件未出现")
            return 3

        hwnd = None
        deadline = time.time() + 30
        while hwnd is None and time.time() < deadline:
            hwnd = find_window(proc.pid)
            if hwnd is None:
                time.sleep(0.05)
        if hwnd is None:
            print("窗口没出现")
            return 3

        # 主窗一可见就立刻量（窗口此时可能还在 (0,0)，先把它挪到有桌面余量的地方
        # 那一刻影子层的最初可见性可能已经被这次挪动掩盖——所以两项分开测）。
        r = RECT()
        USER32.GetWindowRect(ctypes.wintypes.HWND(hwnd), ctypes.byref(r))
        print(f"窗口出现在 ({r.left},{r.top}) {r.right - r.left}x{r.bottom - r.top}")

        # —— ② 影子是否"初始就有" ——
        # 不挪窗口（挪动本身会触发 WM_MOVE，正好是用户说的"拖动一下才出现"的触发源）。
        # 若窗口贴 (0,0)，窗外像素在屏外 → 先把窗口挪到中间，但那会掩盖问题。
        # 折中：先查窗口可见后**立即**的影子读数，再查"一次 WM_MOVE 之后"的读数，
        # 两者若不同就说明首次显示缺了一次事件。
        def ring_probe(tag: str):
            rr = RECT()
            USER32.GetWindowRect(ctypes.wintypes.HWND(hwnd), ctypes.byref(rr))
            pad = 40
            if rr.left - pad < 0 or rr.top - pad < 0:
                print(f"  [{tag}] 窗外像素在屏外（左={rr.left} 上={rr.top} 尺寸 "
                      f"{rr.right-rr.left}x{rr.bottom-rr.top}）")
                return None
            shot = grab_desktop(rr.left - pad, rr.top - pad,
                                (rr.right - rr.left) + 2 * pad, (rr.bottom - rr.top) + 2 * pad)
            px = shot.load()
            midy = pad + (rr.bottom - rr.top) // 2
            vals = [px[pad - d, midy] for d in (3, 6, 9, 12)]
            print(f"  [{tag}] 窗外左缘 3/6/9/12px: " +
                  " ".join(f"{c[0]:02x}{c[1]:02x}{c[2]:02x}" for c in vals))
            return vals

        print("\n=== ② 影子层：启动时 vs 一次外部 WM_MOVE 之后 ===")
        # ⚠ 窗口默认尺寸可能大于屏幕工作区（实测 3587x1975 在 2560x1392 工作区上，
        # 启动就贴到 -27,-27）——那是“窗口超屏被夹”的正常行为，与本问题无关。
        # 量影子必须先把它缩到能看见屏幕余量。
        USER32.SetWindowPos(ctypes.wintypes.HWND(hwnd), HWND_TOPMOST, 300, 200, 900, 640,
                            SWP_NOACTIVATE)
        time.sleep(0.15)
        rr0 = RECT()
        USER32.GetWindowRect(ctypes.wintypes.HWND(hwnd), ctypes.byref(rr0))
        print(f"  挪后位置 ({rr0.left},{rr0.top}) {rr0.right-rr0.left}x{rr0.bottom-rr0.top}")        before = ring_probe("刚挪到中间 0.15s")
        time.sleep(0.35)
        before2 = ring_probe("再等 0.35s（无事件）")
        # 再挪一次（模拟“拖动一下”）
        USER32.SetWindowPos(ctypes.wintypes.HWND(hwnd), HWND_TOPMOST, 302, 202, 900, 640,
                            SWP_NOACTIVATE)
        time.sleep(0.30)
        after = ring_probe("再挪一次 0.3s")
        if before and after:
            same = before == after
            print(f"  => 挪动前 vs 挪动后读{'相同（影子不依赖拖动）' if same else '不同（首次缺事件）'}")
        if before and before2:
            print(f"  => 无事件等待前后{'相同（稳定）' if before == before2 else '不同（会延迟出现）'}")

        # —— ① 圆角光滑度 ——
        print("\n=== ① 圆角边缘过渡级数 ===")
        rr = RECT()
        USER32.GetWindowRect(ctypes.wintypes.HWND(hwnd), ctypes.byref(rr))
        scale = 1.5
        m = call_with_token(h["port"], "metrics", {}, h.get("token", ""))
        if isinstance(m, dict):
            data = m.get("result", m)
            if isinstance(data, dict) and "device_scale" in data:
                scale = float(data["device_scale"])
        radius_px = int(round(10 * scale))
        # 沿 45° 对角线，从窗口外侧走进内侧，统计灰度"跳变"的次数与中间级数
        shot = grab_desktop(rr.left - 40, rr.top - 40, 300, 300)
        shot.save(out / "corner.png")
        px = shot.load()
        sw, sh = shot.size
        ox, oy = 40, 40   # 窗口左上角在 shot 里的坐标
        cx, cy = ox + radius_px, oy + radius_px   # 圆心
        import math
        seq = []
        for step in range(-6, radius_px + 7):
            # 沿对角线（从圆外往圆内）
            x = int(round(cx - (radius_px + step) / math.sqrt(2)))
            y = int(round(cy - (radius_px + step) / math.sqrt(2)))
            if 0 <= x < sw and 0 <= y < sh:
                seq.append((step, px[x, y]))
        lumas = [sum(c) / 3 for _, c in seq]
        print(f"  半径 {radius_px}px，沿 45° 采样 {len(lumas)} 点：")
        print("   " + " ".join(f"{v:.0f}" for v in lumas))
        # 中间级数 = 既不接近最亮也不接近最暗的点数
        if lumas:
            lo, hi = min(lumas), max(lumas)
            span = hi - lo
            mid = sum(1 for v in lumas if lo + span * 0.15 < v < hi - span * 0.15) if span > 4 else 0
            print(f"  明暗跨度 {span:.0f}，**中间过渡级数 {mid}**"
                  f"（AA 应 ≥3；0~1 就是台阶）")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
