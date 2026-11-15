"""真桌面探针：验证 st_desktop 的「铺满显示器 / 置底 / 遮蔽系统任务栏」。

## 为什么必须从**桌面 DC** 抓图，而不是用应用的 capture

`capture` 读的是应用自己的 framebuffer（恒定"正确"），它看不到**窗口在屏幕上
究竟占了多少**。而这一轮要验证的正好是"窗口真的盖住了任务栏那条带子"——
那件事只存在于合成结果里。所以这里用桌面 DC 的 `BitBlt` 抓整屏。

## 为什么必须在 finally 里还原任务栏

被验证的进程会 `ShowWindow(SW_HIDE)` 系统任务栏（`Shell_TrayWnd`）。如果探针
在中途崩掉或超时，那个任务栏会一直躲着——用户会面对一个没有任务栏的桌面。
所以无论成败都要还原，并且在还原后**再断言一次**它真的回来了。

用法：python tools/desktop_shell_probe.py --exe <st_desktop.exe> [--out DIR]
"""

import argparse
import ctypes
import json
import os
import struct
import subprocess
import sys
import time
import zlib
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

# —— Win32 常量 ——
SW_HIDE, SW_SHOW = 0, 5
GWL_STYLE = -16
WS_VISIBLE = 0x10000000
SWP_NOZORDER, SWP_NOSIZE, SWP_NOMOVE, SWP_NOACTIVATE = 0x0004, 0x0001, 0x0002, 0x0010
HWND_TOP, HWND_BOTTOM, HWND_NOTOPMOST = 0, 1, -2
SRCCOPY = 0x00CC0020
DIB_RGB_COLORS = 0
SM_CXSCREEN, SM_CYSCREEN = 0, 1


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG),
                ("biHeight", wintypes.LONG), ("biPlanes", wintypes.WORD),
                ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
                ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD),
                ("biClrImportant", wintypes.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wintypes.DWORD * 3)]


class RECT(ctypes.Structure):
    _fields_ = [("left", wintypes.LONG), ("top", wintypes.LONG),
                ("right", wintypes.LONG), ("bottom", wintypes.LONG)]


class MONITORINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", RECT),
                ("rcWork", RECT), ("dwFlags", wintypes.DWORD)]


def taskbar():
    return user32.FindWindowW("Shell_TrayWnd", None)


def taskbar_visible():
    tb = taskbar()
    if not tb:
        return None
    return bool(user32.IsWindowVisible(tb))


def show_taskbar(show):
    tb = taskbar()
    if tb:
        user32.ShowWindow(tb, SW_SHOW if show else SW_HIDE)
    return tb


def window_rect(hwnd):
    r = RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    return r.left, r.top, r.right - r.left, r.bottom - r.top


def window_class(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buf, 256)
    return buf.value


def window_title(hwnd):
    buf = ctypes.create_unicode_buffer(512)
    n = user32.GetWindowTextW(hwnd, buf, 512)
    return buf.value[:n]


def find_app_window(pid):
    """找属于 pid 的**可见顶层**窗口（跳过工具窗口/0 尺寸）。"""
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def callback(hwnd, _lparam):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value != pid:
            return True
        if not user32.IsWindowVisible(hwnd):
            return True
        style = user32.GetWindowLongW(hwnd, GWL_STYLE)
        if not (style & WS_VISIBLE):
            return True
        left, top, width, height = window_rect(hwnd)
        if width < 100 or height < 100:
            return True
        found.append(hwnd)
        return True

    user32.EnumWindows(callback, 0)
    return found


def z_order_index(hwnd):
    """在顶层窗口 Z 序中的位置（0 = 最前）。返回 None = 不在列表里。"""
    order = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def callback(h, _lparam):
        order.append(h)
        return True

    user32.EnumWindows(callback, 0)
    # EnumWindows 从**最前**往后枚举
    try:
        return order.index(hwnd)
    except ValueError:
        return None


def grab_screen(path):
    """从桌面 DC 抓整屏并写 PNG（纯 ctypes + zlib，无第三方依赖）。"""
    width = user32.GetSystemMetrics(SM_CXSCREEN)
    height = user32.GetSystemMetrics(SM_CYSCREEN)
    screen_dc = user32.GetDC(None)
    mem_dc = gdi32.CreateCompatibleDC(screen_dc)
    bitmap = gdi32.CreateCompatibleBitmap(screen_dc, width, height)
    gdi32.SelectObject(mem_dc, bitmap)
    gdi32.BitBlt(mem_dc, 0, 0, width, height, screen_dc, 0, 0, SRCCOPY)

    # 抓成 32 位自上而下 DIB
    info = BITMAPINFO()
    info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    info.bmiHeader.biWidth = width
    info.bmiHeader.biHeight = -height        # 负 = 自上而下
    info.bmiHeader.biPlanes = 1
    info.bmiHeader.biBitCount = 32
    info.bmiHeader.biCompression = 0         # BI_RGB
    buffer = ctypes.create_string_buffer(width * height * 4)
    gdi32.GetDIBits(mem_dc, bitmap, 0, height, buffer, ctypes.byref(info), DIB_RGB_COLORS)

    gdi32.DeleteObject(bitmap)
    gdi32.DeleteDC(mem_dc)
    user32.ReleaseDC(None, screen_dc)

    raw = buffer.raw
    # BGRA → RGBA
    rows = bytearray(width * height * 4)
    for y in range(height):
        base = y * width * 4
        for x in range(width):
            o = base + x * 4
            rows[o + 0] = raw[o + 2]
            rows[o + 1] = raw[o + 1]
            rows[o + 2] = raw[o + 0]
            rows[o + 3] = 255

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    stride = width * 4
    lines = b"".join(b"\x00" + bytes(rows[y * stride:(y + 1) * stride]) for y in range(height))
    png += chunk(b"IDAT", zlib.compress(lines, 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as handle:
        handle.write(png)
    return width, height


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--out", default="tmp")
    parser.add_argument("--settle", type=float, default=2.5)
    parser.add_argument("--args", default="", help="传给应用的参数（空格分隔）")
    parser.add_argument("--expect", choices=["shell", "windowed"], default="shell",
                        help="shell=铺满+置底+遮蔽任务栏；windowed=普通窗口（应都不成立）")
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)
    report = {"steps": [], "ok": True, "expect": args.expect}

    def note(step, **fields):
        entry = {"step": step, **fields}
        report["steps"].append(entry)
        print(f"[probe] {step}: {json.dumps(fields, ensure_ascii=False)}")

    expect_shell = args.expect == "shell"
    before_visible = taskbar_visible()
    note("taskbar_before", visible=before_visible)

    proc = None
    try:
        command = [args.exe] + (args.args.split() if args.args else [])
        proc = subprocess.Popen(command, cwd=os.path.dirname(args.exe) or None)
        time.sleep(args.settle)

        windows = find_app_window(proc.pid)
        if not windows:
            note("window_found", ok=False, reason="没有找到应用的可见顶层窗口")
            report["ok"] = False
            return 1
        hwnd = windows[0]
        left, top, width, height = window_rect(hwnd)
        note("window_rect", hwnd=hwnd, x=left, y=top, w=width, h=height,
             title=window_title(hwnd), cls=window_class(hwnd))

        # 显示器矩形（含任务栏条带）
        monitor = user32.MonitorFromWindow(hwnd, 2)  # MONITOR_DEFAULTTONEAREST
        info = MONITORINFO()
        info.cbSize = ctypes.sizeof(MONITORINFO)
        user32.GetMonitorInfoW(monitor, ctypes.byref(info))
        mon = info.rcMonitor
        work = info.rcWork
        note("monitor", x=mon.left, y=mon.top, w=mon.right - mon.left, h=mon.bottom - mon.top,
             work_w=work.right - work.left, work_h=work.bottom - work.top,
             taskbar_strip_px=(work.bottom - work.top) - (mon.bottom - mon.top))

        covers_monitor = (left == mon.left and top == mon.top and
                          width == mon.right - mon.left and height == mon.bottom - mon.top)
        # 断言方向由 `--expect` 决定：shell 模式要求**成立**，windowed 要求**不成立**。
        note("covers_whole_monitor", ok=covers_monitor, expected=expect_shell)
        if covers_monitor != expect_shell:
            report["ok"] = False

        # 它必须**盖住任务栏条带**：窗口底缘 = 显示器底缘 > 工作区底缘
        covers_taskbar_strip = (top + height) > work.bottom
        note("covers_system_taskbar_strip", ok=covers_taskbar_strip,
             window_bottom=top + height, work_bottom=work.bottom, expected=expect_shell)
        if covers_taskbar_strip != expect_shell:
            report["ok"] = False

        z = z_order_index(hwnd)
        note("z_order", index=z, total_note="0=最前")
        # 置底：Z 序必须靠后（不是最前几名）。用"存在其它可见顶层窗口在它之上"判断
        at_bottom = z is not None and z > 0
        note("at_bottom", ok=at_bottom, index=z, expected=expect_shell)
        if expect_shell and not at_bottom:
            report["ok"] = False

        # 系统任务栏必须被隐藏（windowed 模式下**不应**被隐藏）
        taskbar_hidden = taskbar_visible() is False
        note("system_taskbar_hidden", ok=taskbar_hidden, visible=taskbar_visible(),
             expected=expect_shell)
        if taskbar_hidden != expect_shell:
            report["ok"] = False

        screen_w, screen_h = grab_screen(os.path.join(args.out, "real-desktop-fullscreen.png"))
        note("screen_grab", file="real-desktop-fullscreen.png", w=screen_w, h=screen_h)
        return 0 if report["ok"] else 1
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()
        time.sleep(1.0)
        # **无论如何都还原任务栏**，并在还原后复核一次
        show_taskbar(True)
        time.sleep(0.6)
        after = taskbar_visible()
        note("taskbar_after_restore", visible=after)
        if after is not True:
            print("[probe] !! 系统任务栏未还原 —— 请手动执行 explorer 重启")
            report["ok"] = False
        with open(os.path.join(args.out, "real-desktop-probe.json"), "w", encoding="utf-8") as handle:
            json.dump(report, handle, ensure_ascii=False, indent=2)
        print("[probe] report:", os.path.join(args.out, "real-desktop-probe.json"))


if __name__ == "__main__":
    sys.exit(main())
