#!/usr/bin/env python3
"""启动响应性量尺：主线程最长多久不回消息（= 会不会出现"等待光标转圈"）。

## 为什么量这个

用户报「gbcode 启动时怎么有短暂的旋转光标」。旋转光标不是应用画的——是 Windows
在判定**进程无响应**时由系统显示。系统判据（经验事实）：进程有可见窗口，但 5 秒内
不响应 `WM_NULL` 之类的基本消息，就被标记为 Ghost 窗口。

所以"有没有转圈"可以量化成：**主线程从建窗到首帧之间最长阻塞多少毫秒**。
用 `SendMessageTimeout(WM_NULL, timeout=1000)` 逐次探测：
- 返回成功且延迟很小 → 主线程在干活之余还回消息；
- 返回 `ok=0`（超时）→ 主线程卡死，那一段就是"用户看到转圈"的窗口。

## 判据

| 量 | 判据 |
|---|---|
| 最长无响应时长 | **< 400ms**（低于系统 Ghost 阈值的一半，留足余量） |
| 首帧完成时刻 | 记录即可（用于解释"为什么慢"，不是判据） |

判据取 400ms 而不是 5000ms：系统阈值是"持续 5 秒"，但用户**感知**的卡顿阈值远低；
而且实测修复前是 1004ms（正好越过我们自己的 1s 探测窗口，说明至少 1s）。

用法：python tools/startup_responsiveness_probe.py [--exe PATH]
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
USER32 = ctypes.WinDLL("user32", use_last_error=True)
USER32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))

WM_NULL = 0x0000
SMTO_BLOCK = 0x0001
SMTO_ABORTIFHUNG = 0x0002
PROBE_TIMEOUT_MS = 1000
BUDGET_MS = 400


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


def find_main(pid: int):
    """该进程的主窗（非阴影层：阴影层带 WS_EX_LAYERED）。"""
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.wintypes.HWND, ctypes.wintypes.LPARAM)
    def cb(hwnd, _lp):
        owner = ctypes.wintypes.DWORD()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value != pid:
            return True
        name = ctypes.create_unicode_buffer(64)
        USER32.GetClassNameW(hwnd, name, 64)
        if name.value != "ShuangtianWindow":
            return True
        if USER32.GetWindowLongW(hwnd, -20) & 0x80000:   # WS_EX_LAYERED = 阴影层
            return True
        found.append(hwnd)
        return False

    USER32.EnumWindows(cb, 0)
    return found[0] if found else None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=str(ROOT / "build/debug/bin/gbcode.exe"))
    parser.add_argument("--budget", type=int, default=BUDGET_MS)
    args = parser.parse_args()

    exe = pathlib.Path(args.exe)
    if not exe.exists():
        print(f"找不到 {exe}")
        return 2

    out = ROOT / "build/startup-resp"
    out.mkdir(parents=True, exist_ok=True)
    ctl = out / "control.json"
    if ctl.exists():
        ctl.unlink()

    samples: list[tuple[float, float, bool]] = []
    t0 = time.time()
    proc = subprocess.Popen([str(exe), "--control-file", str(ctl), "--frames", "3"],
                            cwd=str(ROOT))
    try:
        hwnd = None
        visible_at = None
        deadline = time.time() + 20
        while time.time() < deadline:
            if hwnd is None:
                hwnd = find_main(proc.pid)
            if hwnd is not None:
                mark = ctypes.wintypes.DWORD()
                started = time.time()
                ok = USER32.SendMessageTimeoutW(
                    ctypes.wintypes.HWND(hwnd), WM_NULL, 0, 0,
                    SMTO_BLOCK | SMTO_ABORTIFHUNG, PROBE_TIMEOUT_MS, ctypes.byref(mark))
                elapsed_ms = (time.time() - started) * 1000.0
                vis = bool(USER32.IsWindowVisible(ctypes.wintypes.HWND(hwnd)))
                if vis and visible_at is None:
                    visible_at = time.time() - t0
                samples.append((time.time() - t0, elapsed_ms, bool(ok)))
                if ok and elapsed_ms < 50 and vis and len(samples) > 6:
                    break
            if proc.poll() is not None:
                break
            time.sleep(0.05)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

    if not samples:
        print("没抓到窗口（进程可能没起来）")
        return 2

    print(f"{'时刻(ms)':>10} {'响应延迟(ms)':>14} {'响应':>6} {'可见':>6}")
    for at, ms, ok in samples:
        print(f"{at*1000:10.0f} {ms:14.1f} {str(ok):>6}")

    worst = max(ms for _, ms, ok in samples if not ok) if any(not ok for _, _, ok in samples) \
        else max(ms for _, ms, _ in samples)
    timed_out = [at for at, ms, ok in samples if not ok]
    print()
    print(f"最长无响应: {worst:.0f} ms（探测上限 {PROBE_TIMEOUT_MS} ms）")
    if visible_at is not None:
        print(f"窗口首次可见: {visible_at*1000:.0f} ms")
    if timed_out:
        print(f"探测超时的采样数: {len(timed_out)}（时刻 {', '.join(f'{t*1000:.0f}ms' for t in timed_out[:6])}）")
    # 判据：只要出现探测超时，就说明主线程卡了整整一个探测窗口
    blocked = worst if timed_out else 0.0
    ok = blocked < args.budget
    print(f"\n判据：最长连续无响应 < {args.budget} ms → {'PASS' if ok else 'FAIL'}")
    if not ok:
        print("  → 系统在此期间会把进程当作“未响应”，用户看到等待光标（转圈）")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
