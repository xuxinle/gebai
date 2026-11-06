#!/usr/bin/env python3
"""窗框量测（**不换焦点**）：判据全是对真窗口的数字。

为什么“不换焦点”：`PrintWindow(PW_RENDERFULLCONTENT)` 在窗口**失焦**时会返回一份
没有重绘的旧内容（实测：卡片数值全空、DPI 显示 1x），拿它与画布比会得出上万像素的
假差异。所以量测分两路，各取所长：

  · **窗缘像素**（有没有系统灰边 / 圆角是否生效）→ 用 `PrintWindow`（它自带 DWM 合成），
    但必须在**窗口仍激活**时取。失焦那一路由 `window_unfocused_frame_check.py` 负责
    （那条只做定性判断：窗缘仍是应用像素）。
  · **窗外阴影**（是否存在、剖面是否单调）→ 桌面截屏 + 把窗口挪到屏幕中间并抬到最前，
    且强制校验「中心点命中的窗口 == 目标窗口」，否则读数不可信。

判据全是数字：
  ① 窗缘四条边 0..15px 与画布一致（激活态）；
  ② 四角 0..(半径+2) 区域有差异（= 圆角真的让出去了）；
  ③ 窗外 1..margin 有单调衰减的非零阴影（剖面打印出来）；
  ④ 最大化后：影子层隐藏、无圆角裁剪区（`GetWindowRgn` 是非题）。

用法：python tools/window_chrome_measure.py [--exe PATH] [--out DIR]
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

USER32 = ctypes.WinDLL("user32", use_last_error=True)
GDI32 = ctypes.WinDLL("gdi32", use_last_error=True)
USER32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
PW_RENDERFULLCONTENT = 0x00000002
HWND_TOPMOST = ctypes.c_void_p(-1)
SWP_NOACTIVATE = 0x0010

failures: list[str] = []


def check(ok: bool, message: str) -> None:
    print(f"  [{'OK' if ok else 'X'}] {message}", flush=True)
    if not ok:
        failures.append(message)


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32),
                ("biHeight", ctypes.c_int32), ("biPlanes", ctypes.c_uint16),
                ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_int32),
                ("biYPelsPerMeter", ctypes.c_int32), ("biClrUsed", ctypes.c_uint32),
                ("biClrImportant", ctypes.c_uint32)]


def _dib(hdc_ref, w: int, h: int):
    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.biWidth = w
    bi.biHeight = -h
    bi.biPlanes = 1
    bi.biBitCount = 32
    bits = ctypes.c_void_p()
    hbmp = GDI32.CreateDIBSection(hdc_ref, ctypes.byref(bi), 0, ctypes.byref(bits), None, 0)
    return hbmp, bits


def _to_image(w: int, h: int, bits):
    from PIL import Image  # noqa: PLC0415

    buf = (ctypes.c_ubyte * (w * h * 4)).from_address(bits.value)
    return Image.frombuffer("RGB", (w, h), bytes(buf), "raw", "BGRX", 0, 1).copy()


def print_window(hwnd, w: int, h: int):
    """让窗口自己把**合成后**的内容画到 DC 上（无视遮挡，含 DWM 施加的效果）。

    ⚠ 失焦时会返回旧内容 → 只在窗口激活时用它做像素判据。
    """
    hdc_screen = USER32.GetDC(None)
    hdc_mem = GDI32.CreateCompatibleDC(hdc_screen)
    hbmp, bits = _dib(hdc_screen, w, h)
    old = GDI32.SelectObject(hdc_mem, hbmp)
    ok = USER32.PrintWindow(hwnd, hdc_mem, PW_RENDERFULLCONTENT)
    img = _to_image(w, h, bits)
    GDI32.SelectObject(hdc_mem, old)
    GDI32.DeleteObject(hbmp)
    GDI32.DeleteDC(hdc_mem)
    USER32.ReleaseDC(None, hdc_screen)
    return bool(ok), img


def grab_desktop(x: int, y: int, w: int, h: int):
    """抓屏幕上的一块（用于看**窗外**的阴影——PrintWindow 拿不到窗口外的像素）。"""
    hdc_screen = USER32.GetDC(None)
    hdc_mem = GDI32.CreateCompatibleDC(hdc_screen)
    hbmp, bits = _dib(hdc_screen, w, h)
    old = GDI32.SelectObject(hdc_mem, hbmp)
    GDI32.BitBlt(hdc_mem, 0, 0, w, h, hdc_screen, x, y, 0x00CC0020 | 0x40000000)
    img = _to_image(w, h, bits)
    GDI32.SelectObject(hdc_mem, old)
    GDI32.DeleteObject(hbmp)
    GDI32.DeleteDC(hdc_mem)
    USER32.ReleaseDC(None, hdc_screen)
    return img


def find_window(pid: int):
    """按**本进程**找霜天窗口（机器上可能还有别的霜天应用，按类名全局找会拿错窗）。"""
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.wintypes.HWND, ctypes.wintypes.LPARAM)
    def on_window(hwnd, _lp):
        owner = ctypes.wintypes.DWORD()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value != pid or not USER32.IsWindowVisible(hwnd):
            return True
        name = ctypes.create_unicode_buffer(64)
        USER32.GetClassNameW(hwnd, name, 64)
        if name.value != "ShuangtianWindow":
            return True
        found.append(hwnd)
        return False

    USER32.EnumWindows(on_window, 0)
    return found[0] if found else None


def visible_shuangtian_windows(pid: int) -> int:
    count = [0]

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.wintypes.HWND, ctypes.wintypes.LPARAM)
    def on_window(hwnd, _lp):
        owner = ctypes.wintypes.DWORD()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value != pid or not USER32.IsWindowVisible(hwnd):
            return True
        name = ctypes.create_unicode_buffer(64)
        USER32.GetClassNameW(hwnd, name, 64)
        if name.value == "ShuangtianWindow":
            count[0] += 1
        return True

    USER32.EnumWindows(on_window, 0)
    return count[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=str(DEFAULT_EXE))
    parser.add_argument("--out", default=str(ROOT / "build/win-chrome-measure"))
    parser.add_argument("--delay", type=float, default=3.5)
    args = parser.parse_args()
    exe = pathlib.Path(args.exe)
    if not exe.exists():
        print(f"可执行文件不存在：{exe}")
        return 2
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
        from PIL import Image  # noqa: PLC0415

        def canvas():
            path = out / "canvas.png"
            if path.exists():
                path.unlink()
            call_with_token(handle["port"], "capture", {"path": str(path)}, handle.get("token", ""))
            return Image.open(path).convert("RGB")

        # —— ① 窗缘与圆角（窗口仍激活；PrintWindow 此时内容可靠）——
        USER32.SetForegroundWindow(ctypes.wintypes.HWND(hwnd))
        time.sleep(0.8)
        rect = RECT()
        USER32.GetWindowRect(hwnd, ctypes.byref(rect))
        w, h = rect.right - rect.left, rect.bottom - rect.top
        ok, composed = print_window(hwnd, w, h)
        composed.save(out / "composed_active.png")
        img = canvas()
        metrics = call_with_token(handle["port"], "metrics", {}, handle.get("token", ""))
        scale = 1.0
        if isinstance(metrics, dict):
            data = metrics.get("result", metrics)
            if isinstance(data, dict) and "device_scale" in data:
                scale = float(data["device_scale"])
        print(f"窗口 {w}x{h} 画布 {img.size} device_scale={scale}")
        check(ok and img.size == composed.size, "PrintWindow 与画布尺寸一致")
        if img.size != composed.size:
            return 1
        cp, sp = img.load(), composed.load()

        def edge_bad(side: str) -> int:
            pts = {
                "左": [(x, h // 2) for x in range(0, 16)],
                "上": [(w // 2, y) for y in range(0, 16)],
                "右": [(w - 1 - x, h // 2) for x in range(0, 16)],
                "下": [(w // 2, h - 1 - y) for y in range(0, 16)],
            }[side]
            return sum(1 for (x, y) in pts if max(abs(cp[x, y][i] - sp[x, y][i]) for i in range(3)) > 2)

        for side in ("左", "上", "右", "下"):
            n = edge_bad(side)
            check(n == 0, f"{side}侧 16px 内与画布一致（不一致 {n} 处）")

        # —— ② 圆角真的让出去了？角上必须与画布不同 ——
        radius_px = max(10, int(round(10 * scale)))
        corner_diff = 0
        for y in range(0, radius_px + 2):
            for x in range(0, radius_px + 2):
                if max(abs(cp[x, y][i] - sp[x, y][i]) for i in range(3)) > 2:
                    corner_diff += 1
        check(corner_diff > 0, f"左上角 {radius_px + 2}px 内有 {corner_diff} 个像素被圆角让出")

        # —— ③ 窗外阴影（桌面截屏；先挪到屏幕中间并抬到最前）——
        # 窗口默认贴 (0,0) 且几乎铺满屏，左侧/上方根本没有“窗外”的像素可量（读到 000000）。
        USER32.SetWindowPos(ctypes.wintypes.HWND(hwnd), HWND_TOPMOST, 260, 200, 900, 640,
                            SWP_NOACTIVATE)
        time.sleep(1.2)
        USER32.BringWindowToTop(ctypes.wintypes.HWND(hwnd))
        USER32.SetForegroundWindow(ctypes.wintypes.HWND(hwnd))
        time.sleep(1.0)
        fr = RECT()
        USER32.GetWindowRect(hwnd, ctypes.byref(fr))
        probe = POINT(fr.left + (fr.right - fr.left) // 2, fr.top + (fr.bottom - fr.top) // 2)
        top = USER32.WindowFromPoint(probe)
        check(top == hwnd, f"窗口在最上面（中心点命中 {top:#x}）")
        pad = 48
        shot = grab_desktop(fr.left - pad, fr.top - pad, (fr.right - fr.left) + 2 * pad,
                            (fr.bottom - fr.top) + 2 * pad)
        shot.save(out / "desktop_ring.png")
        if top == hwnd:
            spx = shot.load()
            midy = pad + (fr.bottom - fr.top) // 2
            ring = [spx[pad - d, midy] for d in range(28, 0, -1)]
            print("  窗外左缘（由远及近，RGB）: " + " ".join(f"{c[0]:02x}{c[1]:02x}{c[2]:02x}" for c in ring))
            near = spx[pad - 3, midy]
            far = spx[pad - 26, midy]
            check(sum(near) // 3 < sum(far) // 3 - 6,
                  f"窗外近处比远处暗（阴影存在）：近 {near} 远 {far}")
            lumas = [sum(spx[pad - d, midy]) / 3 for d in range(3, 27)]
            non_decreasing = sum(1 for i in range(1, len(lumas)) if lumas[i] + 2 >= lumas[i - 1])
            check(non_decreasing >= len(lumas) - 2,
                  f"阴影由近及远单调变亮（{non_decreasing}/{len(lumas) - 1} 段满足）")

        # —— ④ 最大化：不画 ——
        call_with_token(handle["port"], "invoke", {"id": "titlebar", "action": "maximize"},
                        handle.get("token", ""))
        # 最大化触发了整窗重建（画布 + swapchain），等它真的画完再取。
        time.sleep(2.5)
        zoomed = USER32.IsZoomed(ctypes.wintypes.HWND(hwnd))
        check(bool(zoomed), "已最大化")
        if zoomed:
            # 最大化后不该有圆角：直接问系统「这个窗口还挂着裁剪区域吗」。
            # `GetWindowRgn` 是**是非题**：NULLREGION/ERROR = 没设过区域 = 直角。
            region = GDI32.CreateRectRgn(0, 0, 1, 1)
            kind = USER32.GetWindowRgn(ctypes.wintypes.HWND(hwnd), region)
            GDI32.DeleteObject(region)
            check(kind in (0, 1), f"最大化后无圆角裁剪区（GetWindowRgn={kind}，0/1=无区域）")
            check(visible_shuangtian_windows(proc.pid) == 1,
                  f"最大化后只显示主窗、影子层已隐藏（可见窗口 {visible_shuangtian_windows(proc.pid)} 个）")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    print()
    if failures:
        print(f"失败 {len(failures)} 项：")
        for item in failures:
            print(f"  · {item}")
        return 1
    print("全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
