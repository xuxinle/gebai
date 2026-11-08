#!/usr/bin/env python3
"""启动 UX 量尺：**双击 → 窗口可见 → 内容出现**（墙钟，含进程启动）。

## 为什么需要它（与仓里已有三个量尺的分工）

| 量尺 | 量的是 | 漏掉的 |
|---|---|---|
| `perf::startup_mark`（`ST_PERF_STARTUP=1`） | `app.start()` 内部各阶段 | **进程启动段**（CRT/静态构造/日志落盘），且**只有 `Application::run()` 会打印**——自己写主循环的应用（gallery/相期/裁云/gbcode）需自己调 `log_startup_summary`，现状只有 gallery 调了 |
| `tools/startup_responsiveness_probe.py` | 主线程最长不回消息（会不会转圈） | 用户"看见东西"的时刻 |
| `tools/startup_latency_probe.py` | 控制通道就绪 | 同上 |

本探针在**父进程**侧打墙钟，量三个用户可见事件：

    t_win      EnumWindows 找到 ShuangtianWindow 对象（CreateWindowExW 已返回）
    t_visible  IsWindowVisible 首次为真（= `show_early()` 填底色那一刻）
    t_content  客户区中心采样到"有内容"（颜色数 ≥3 且主导色占比 < 90%）

于是 `t_content - t_visible` 就是**用户盯着一块纯底色的时长**——启动动画要填的正是这一段。

## 口径（读数字前必看）

- 三个时刻都是**墙钟**，含进程启动（`CreateProcess` → main 之间那段不归应用）。
- 内容判据是**采样**：每轮只 BitBlt 客户区中心 `--sample` 见方的一块（全窗 BitBlt 27MB 会自己拖慢
  被测进程），所以 `t_content` 的分辨率是采样周期（默认 8ms）。
- 底色是"一种颜色、占比 100%"，界面一出现立刻变成"多种颜色、主导色 <90%"——两者区分度极大，
  不需要更细的判据。
- 也记录一次"建窗后主线程回不回消息"（`SendMessageTimeout(WM_NULL)`），0 = 当场回。

用法：
    py -3 tools/startup_ux_probe.py                                  # gallery，5 轮
    py -3 tools/startup_ux_probe.py --exe build/dev/bin/xiangqi.exe --runs 3
    py -3 tools/startup_ux_probe.py --theme dark                     # 顺带验证底色是否跟随主题
"""

from __future__ import annotations

import argparse
import collections
import ctypes
import ctypes.wintypes as wt
import pathlib
import statistics
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
USER32 = ctypes.WinDLL("user32", use_last_error=True)
GDI32 = ctypes.WinDLL("gdi32", use_last_error=True)
USER32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))

WM_NULL = 0x0000
SMTO_BLOCK = 0x0001
SMTO_ABORTIFHUNG = 0x0002
SRCCOPY = 0x00CC0020


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", ctypes.c_long), ("biHeight", ctypes.c_long),
                ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wt.DWORD),
                ("biClrImportant", wt.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wt.DWORD * 3)]


def find_main(pid: int):
    """该进程的主窗（类名 ShuangtianWindow；跳过 WS_EX_LAYERED 的阴影层）。"""
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    def cb(hwnd, _lp):
        owner = wt.DWORD()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value != pid:
            return True
        name = ctypes.create_unicode_buffer(64)
        USER32.GetClassNameW(hwnd, name, 64)
        if name.value != "ShuangtianWindow":
            return True
        if USER32.GetWindowLongW(hwnd, -20) & 0x80000:  # WS_EX_LAYERED
            return True
        found.append(hwnd)
        return False

    USER32.EnumWindows(cb, 0)
    return found[0] if found else None


def sample(hwnd, size: int):
    """只抓客户区中心 size×size：返回 (颜色数, 主导色 hex, 主导色占比)。"""
    rc = RECT()
    if USER32.GetClientRect(hwnd, ctypes.byref(rc)) == 0:
        return None
    width, height = rc.right, rc.bottom
    if width <= 0 or height <= 0:
        return None
    x = max(0, width // 2 - size // 2)
    y = max(0, height // 2 - size // 2)
    w = min(size, width - x)
    h = min(size, height - y)
    dc = USER32.GetDC(hwnd)
    if not dc:
        return None
    mem = GDI32.CreateCompatibleDC(dc)
    bmp = GDI32.CreateCompatibleBitmap(dc, w, h)
    GDI32.SelectObject(mem, bmp)
    GDI32.BitBlt(mem, 0, 0, w, h, dc, x, y, SRCCOPY)
    info = BITMAPINFO()
    info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    info.bmiHeader.biWidth = w
    info.bmiHeader.biHeight = -h
    info.bmiHeader.biPlanes = 1
    info.bmiHeader.biBitCount = 32
    info.bmiHeader.biCompression = 0
    buf = ctypes.create_string_buffer(w * h * 4)
    GDI32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(info), 0)
    data = buf.raw
    hist = collections.Counter()
    for i in range(0, len(data), 4):
        hist[data[i:i + 3]] += 1
    GDI32.DeleteObject(bmp)
    GDI32.DeleteDC(mem)
    USER32.ReleaseDC(hwnd, dc)
    top, count = hist.most_common(1)[0]
    total = sum(hist.values())
    return len(hist), bytes(top).hex().upper(), count / total if total else 0.0


def one_run(exe: pathlib.Path, args: list[str], sample_size: int, timeout: float) -> dict:
    t0 = time.time()
    proc = subprocess.Popen([str(exe), *args], cwd=str(ROOT),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t_win = t_visible = t_content = None
    matte_hex = None
    content_hex = None
    hung_ms = None
    timeline = []
    try:
        while time.time() - t0 < timeout and proc.poll() is None:
            now = (time.time() - t0) * 1000.0
            if t_win is None:
                hwnd = find_main(proc.pid)
                if hwnd is not None:
                    t_win = now
            if t_win is not None and USER32.IsWindowVisible(wt.HWND(hwnd)):
                if t_visible is None:
                    t_visible = now
                    mark = wt.DWORD()
                    started = time.time()
                    USER32.SendMessageTimeoutW(wt.HWND(hwnd), WM_NULL, 0, 0,
                                               SMTO_BLOCK | SMTO_ABORTIFHUNG, 1000,
                                               ctypes.byref(mark))
                    hung_ms = (time.time() - started) * 1000.0
                snap = sample(wt.HWND(hwnd), sample_size)
                if snap is not None:
                    distinct, dominant, share = snap
                    timeline.append((round(now), distinct, dominant, round(share, 2)))
                    if matte_hex is None:
                        matte_hex = dominant
                    if t_content is None and distinct >= 3 and share < 0.90:
                        t_content = now
                        content_hex = dominant
            if t_content is not None:
                break
            time.sleep(0.008)
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    return {"t_win_ms": t_win, "t_visible_ms": t_visible, "t_content_ms": t_content,
            "matte": matte_hex, "content_dominant": content_hex,
            "hung_probe_ms": hung_ms, "timeline": timeline}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=str(ROOT / "build/dev/bin/gallery.exe"))
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--sample", type=int, default=160, help="中心采样边长（物理像素）")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--theme", default="", help="透传给应用的 --theme（验证底色是否跟随主题）")
    parser.add_argument("--arg", action="append", default=[], help="其他透传参数（可重复）")
    parser.add_argument("--frames", default="60",
                        help="应用跑多少帧后退出（只用于让进程活着，不影响测量）")
    args = parser.parse_args()

    exe = pathlib.Path(args.exe)
    if not exe.exists():
        print(f"找不到 {exe}")
        return 2
    extra = ["--frames", args.frames, *args.arg]
    if args.theme:
        extra += ["--theme", args.theme]

    runs = []
    for index in range(args.runs):
        result = one_run(exe, extra, args.sample, args.timeout)
        runs.append(result)
        def show(value):
            return "—" if value is None else f"{value:.0f}"
        print(f"  第 {index + 1} 轮：建窗 {show(result['t_win_ms'])}ms · "
              f"可见 {show(result['t_visible_ms'])}ms · 内容 {show(result['t_content_ms'])}ms · "
              f"底色 {result['matte']} → 内容主导色 {result['content_dominant']} · "
              f"建窗时无响应 {show(result['hung_probe_ms'])}ms")
        if result["t_content_ms"] is None and result["timeline"]:
            tail = result["timeline"][-3:]
            print(f"    ⚠ 未采到内容（最后几帧采样：{tail}）")

    def median(key):
        values = [r[key] for r in runs if r[key] is not None]
        return statistics.median(values) if values else None

    t_visible = median("t_visible_ms")
    t_content = median("t_content_ms")
    print(f"\n{exe.name} 中位（{len(runs)} 轮）："
          f"建窗 {median('t_win_ms') or 0:.0f}ms · 可见 {t_visible or 0:.0f}ms · "
          f"内容 {t_content or 0:.0f}ms")
    if t_visible is not None and t_content is not None:
        print(f"  纯底色停顿 {t_content - t_visible:.0f}ms ← 启动动画要覆盖的就是这一段")
    mattes = {r["matte"] for r in runs if r["matte"]}
    if mattes:
        print(f"  启动底色 {' / '.join(sorted(mattes))}"
              f"（{args.theme or '默认主题'}）——暗色主题下若仍是 F7F7FA，即为硬编码底色的缺陷）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
