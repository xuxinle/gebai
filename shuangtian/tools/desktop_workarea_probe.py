"""量：隐藏/恢复系统任务栏时，工作区（rcWork）与任务栏矩形会怎么变。

## 为什么必须量这一条（决定 Dock 该多高）

需求是「任务栏与系统任务栏等高，避免最大化窗口遮蔽和间隙」。它成立的前提是
**两件矩形重合**：

    Dock 顶缘  ==  rcWork.bottom   （最大化窗口的底边）
    Dock 底缘  ==  rcMonitor.bottom（屏幕底）

而隐藏任务栏会改 `rcWork`——如果隐藏时 `rcWork.bottom` 跳到屏幕底，那么最大化
窗口的底边就落到屏幕底，Dock 必然**遮蔽**它；反过来若 `rcWork` 不变，Dock 高度
就等于系统任务栏厚度。

所以这里量三个时刻：可见 / 隐藏 / 恢复。
"""

import ctypes
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


def probe(tag):
    hwnd = user32.GetDesktopWindow()
    monitor = user32.MonitorFromWindow(hwnd, 2)   # NEAREST
    info = MONITORINFO()
    info.cbSize = ctypes.sizeof(MONITORINFO)
    user32.GetMonitorInfoW(monitor, ctypes.byref(info))
    mon, work = info.rcMonitor, info.rcWork
    strip = mon.bottom - work.bottom
    print(f"{tag}:")
    print(f"  rcMonitor.bottom = {mon.bottom}")
    print(f"  rcWork.bottom    = {work.bottom}   （任务栏厚度 = {strip}）")
    handle = tray()
    if handle:
        rect = RECT()
        user32.GetWindowRect(handle, ctypes.byref(rect))
        print(f"  tray rect: top={rect.top} bottom={rect.bottom} "
              f"h={rect.bottom - rect.top} visible={bool(user32.IsWindowVisible(handle))}")
    return mon.bottom, work.bottom, strip


def main():
    handle = tray()
    if handle is None:
        print("找不到系统任务栏")
        return 1
    try:
        visible = probe("任务栏可见")
        user32.ShowWindow(handle, 0)   # SW_HIDE
        time.sleep(0.8)
        hidden = probe("任务栏已隐藏")
        print()
        print("→ 隐藏前后 rcWork.bottom:", visible[1], "→", hidden[1])
        verdict = ("**不变**——Dock 高度 = 系统任务栏厚度（48）才能与最大化窗口对齐"
                   if visible[1] == hidden[1] else
                   "**变了**——隐藏任务栏后最大化窗口会铺到屏幕底，Dock 必然遮蔽它")
        print("→ 结论:", verdict)
        return 0
    finally:
        user32.ShowWindow(handle, 5)   # SW_SHOW
        time.sleep(0.5)
        print("（已恢复任务栏）", "visible =", bool(user32.IsWindowVisible(tray())))


if __name__ == "__main__":
    raise SystemExit(main())
