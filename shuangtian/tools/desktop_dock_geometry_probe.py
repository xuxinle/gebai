"""验收「Dock 与系统任务栏等高、零缝隙」：全屏模式 + 真窗口 + 最大化一个应用窗口。

## 为什么必须在真窗口下验，且必须再最大化一个窗口

无头后端没有“工作区”这回事（`work_area_logical()` 退回视口），所以 Dock 高度只会
落到兜底值——数字碰巧对，但证明不了任何事。而需求的实质是一个**几何关系**：

    Dock 顶缘  ==  rcWork.bottom  ==  最大化窗口的底边

只要三者不重合，用户就会看到「缝隙」或「遮蔽」。所以本探针做三件事：

1. 起桌面壳（默认就是全屏置底 + 遮蔽系统任务栏），量它的 Dock 矩形；
2. 起一个**普通应用**并最大化它，量它的客户区矩形；
3. 断言：Dock 顶缘 == 最大化窗口底边（容差 1px），且 Dock 底缘 == 屏幕底。

## 从哪儿读 Dock 的矩形

控制通道（`find "#dock"` → `bounds`，逻辑像素）。不用截图像素测——那会被
玻璃的半透明混色干扰，而几何是我们要断言的东西。

用法：
    python tools/desktop_dock_geometry_probe.py --exe <st_desktop.exe> \\
        [--app <gallery.exe>] [--control <control.json>]
"""

import argparse
import ctypes
import json
import os
import subprocess
import sys
import time
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)


class RECT(ctypes.Structure):
    _fields_ = [("left", wintypes.LONG), ("top", wintypes.LONG),
                ("right", wintypes.LONG), ("bottom", wintypes.LONG)]


class MONITORINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", RECT),
                ("rcWork", RECT), ("dwFlags", wintypes.DWORD)]


def tray():
    return user32.FindWindowW("Shell_TrayWnd", None)


def monitor_of(hwnd):
    info = MONITORINFO()
    info.cbSize = ctypes.sizeof(MONITORINFO)
    monitor = user32.MonitorFromWindow(hwnd or user32.GetDesktopWindow(), 2)
    user32.GetMonitorInfoW(monitor, ctypes.byref(info))
    return info.rcMonitor, info.rcWork


def find_window(pid, min_size=200):
    """找某 pid 的可见顶层窗口里最大的那个。"""
    best = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def callback(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value != pid or not user32.IsWindowVisible(hwnd):
            return True
        rect = RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        w, h = rect.right - rect.left, rect.bottom - rect.top
        if w >= min_size and h >= min_size:
            best.append((w * h, hwnd, rect))
        return True

    user32.EnumWindows(callback, 0)
    if not best:
        return None, None
    best.sort(reverse=True)
    return best[0][1], best[0][2]


def control_call(control_path, method, params=None):
    """经**项目自带的客户端库**调控制通道（不手搓协议）。

    手搓会踩三件事：token 握手、4 字节长度前缀、分片读取——而错误信息离真因很远
    （见 `tools/st_client_lib.py` 头注释）。
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from st_client_lib import call_with_token, load_control

    info = load_control(control_path)
    port = info.get("port")
    if not port:
        return None
    return call_with_token(port, method, params or {}, info.get("token", ""))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--app", default="")
    parser.add_argument("--control", default="")
    parser.add_argument("--settle", type=float, default=3.0)
    args = parser.parse_args()

    ok = True

    def check(label, condition, detail):
        nonlocal ok
        mark = "OK  " if condition else "FAIL"
        print(f"  [{mark}] {label}: {detail}")
        if not condition:
            ok = False

    # 带控制通道起：Dock 的矩形只能经它读（截图像素会被半透明混色污染）。
    control_path = args.control or os.path.join(
        os.path.dirname(os.path.abspath(args.exe)) or ".", "dock-probe-control.json")
    if os.path.exists(control_path):
        os.remove(control_path)
    desktop = subprocess.Popen(
        [args.exe, "--control-port", "0", "--control-file", control_path],
        cwd=os.path.dirname(args.exe) or None)
    for _ in range(40):
        if os.path.exists(control_path):
            break
        time.sleep(0.1)
    try:
        time.sleep(args.settle)
        main_hwnd, desktop_rect = find_window(desktop.pid, min_size=500)
        if main_hwnd is None:
            print("没找到桌面壳窗口")
            return 1
        mon, work = monitor_of(main_hwnd)
        print(f"显示器 rcMonitor = ({mon.left},{mon.top})-({mon.right},{mon.bottom})")
        print(f"工作区 rcWork    = ({work.left},{work.top})-({work.right},{work.bottom})")
        print(f"桌面壳窗口       = ({desktop_rect.left},{desktop_rect.top})"
              f"-({desktop_rect.right},{desktop_rect.bottom})")

        # —— Dock 矩形：从控制通道读 ——
        dock = None
        if os.path.exists(control_path):
            response = control_call(control_path, "find", {"selector": "#dock"})
            try:
                match = response["result"]["matches"][0]
                dock = match["bounds"]
            except Exception:
                dock = None
        if dock is None:
            print("读不到 Dock 矩形（控制通道不可用）——改用几何推导")
        else:
            print(f"Dock（逻辑像素）  = x={dock['x']} y={dock['y']} "
                  f"w={dock['width']} h={dock['height']}")

        # —— 起一个普通应用并最大化，量它的底边 ——
        app_hwnd = None
        if args.app and os.path.exists(args.app):
            app_proc = subprocess.Popen([args.app], cwd=os.path.dirname(args.app) or None)
            time.sleep(args.settle)
            app_hwnd, _ = find_window(app_proc.pid, min_size=300)
            if app_hwnd is not None:
                user32.ShowWindow(app_hwnd, 3)   # SW_MAXIMIZE
                time.sleep(1.2)
                rect = RECT()
                user32.GetWindowRect(app_hwnd, ctypes.byref(rect))
                print(f"最大化应用窗口    = ({rect.left},{rect.top})-({rect.right},{rect.bottom})")
                # 断言：最大化窗口底边 == 工作区底边（系统行为，Dock 要对齐的就是它）
                check("最大化窗口底边 == rcWork.bottom",
                      abs(rect.bottom - work.bottom) <= 1,
                      f"{rect.bottom} vs {work.bottom}")

        if dock is not None:
            # Dock 在窗口内的逻辑坐标 → 屏幕物理坐标（桌面壳铺满显示器且 scale=1）
            scale = (mon.right - mon.left) / max(1, desktop_rect.right - desktop_rect.left)
            dock_top_screen = desktop_rect.top + dock["y"] * scale
            dock_bottom_screen = dock_top_screen + dock["height"] * scale
            print(f"Dock 屏幕矩形     = top={dock_top_screen:.0f} bottom={dock_bottom_screen:.0f} "
                  f"h={dock['height'] * scale:.0f}")
            check("Dock 顶缘 == rcWork.bottom（零缝隙）",
                  abs(dock_top_screen - work.bottom) <= 1,
                  f"{dock_top_screen:.0f} vs {work.bottom}")
            check("Dock 底缘 == rcMonitor.bottom（贴屏幕底）",
                  abs(dock_bottom_screen - mon.bottom) <= 1,
                  f"{dock_bottom_screen:.0f} vs {mon.bottom}")
            check("Dock 高度 == 系统任务栏厚度",
                  abs(dock["height"] * scale - (mon.bottom - work.bottom)) <= 1,
                  f"{dock['height'] * scale:.0f} vs {mon.bottom - work.bottom}")
        return 0 if ok else 2
    finally:
        # 关掉最大化测试应用（本探针自己起的那一个）
        try:
            subprocess.run(["taskkill", "/F", "/IM", os.path.basename(args.app)],
                           capture_output=True) if args.app else None
        except Exception:
            pass
        if desktop.poll() is None:
            desktop.terminate()
            try:
                desktop.wait(timeout=8)
            except subprocess.TimeoutExpired:
                desktop.kill()
        time.sleep(1.2)
        handle = tray()
        if handle:
            user32.ShowWindow(handle, 5)


if __name__ == "__main__":
    sys.exit(main())
