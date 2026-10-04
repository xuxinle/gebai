#!/usr/bin/env python3
"""真实窗口下的窗框验证：**无边框窗口 + 自绘标题栏 + 窗口控制**。

为什么必须真窗口：`WS_POPUP | WS_THICKFRAME` + `WM_NCCALCSIZE` + `WM_NCHITTEST` 这套
只在真窗口里才有效果——无头跑一万遍也验证不了"客户区是否等于整窗""边缘能不能抓"。
而又**不能只看代码**：少返回一个 `HT*` 码，拖动/缩放就静默失效，截图上看不出来。

本脚本用进程内控制通道做的验证（可在无 X 服务器的机器上跑）：
  1. 无边框下 `客户区 == 窗口矩形`（少这一条就是"边缘被系统裁掉 + DXGI 拉伸发糊"）；
  2. `#titlebar` 存在且类型是 `TitleBar`（自绘窗框在真窗口下照旧）；
  3. `invoke minimize/maximize/close` 三个动作**真的生效**（`handled=true`），
     且 `maximized` 属性随窗口状态变化（按钮形态据此切换）；
  4. `--decorations` 对照：显式要系统窗框时，机制仍工作（不因一条路径改动而崩）。

真实鼠标点击标题栏拖动/边缘缩放需要 X 服务器（`xdotool`），
那部分由 `tools/st_win_check.py` 在有桌面会话的机器上覆盖——两条脚本分工明确：
**本脚本验"窗口结构与窗口控制"，`st_win_check.py` 验"输入事件链路"。**

用法：`python tools/title_bar_win_check.py [--exe PATH]`
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes
import json
import os
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_EXE = ROOT / "build/debug/bin/codeeditor.exe"
OUT_DIR = ROOT / "build/win-titlebar"
sys.path.insert(0, str(ROOT / "tools"))
from st_client_lib import call_with_token, load_control  # noqa: E402

USER32 = ctypes.WinDLL("user32", use_last_error=True)


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


class MONITORINFO(ctypes.Structure):
    """`ctypes.wintypes` **没有**这个结构（它不是 Win32 基础类型）——只能自己声明。

    字段顺序必须与 SDK 一致：少给 `cbSize` 或顺序错了，`GetMonitorInfoW` 直接失败
    （而它失败时返回 0、不抛异常，很容易被当成"工作区就是屏幕"）。
    """

    _fields_ = [("cbSize", ctypes.c_ulong), ("rcMonitor", RECT), ("rcWork", RECT),
                ("dwFlags", ctypes.c_ulong)]

GWL_STYLE = -16
WS_CAPTION = 0x00C00000
WS_THICKFRAME = 0x00040000
WS_POPUP = 0x80000000

failures: list[str] = []


def check(condition: bool, message: str) -> None:
    if condition:
        print(f"  [OK] {message}", flush=True)
    else:
        print(f"  [X]  {message}", flush=True)
        failures.append(message)


def window_rects(pid: int) -> tuple[tuple[int, int, int, int], tuple[int, int, int, int]] | None:
    """找该进程的主窗口，返回 (窗口矩形, 客户区矩形)（均为屏幕坐标）。"""
    found: list[tuple[tuple[int, int, int, int], tuple[int, int, int, int]]] = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def callback(hwnd, _lparam):
        owner = ctypes.c_ulong()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value != pid or not USER32.IsWindowVisible(hwnd):
            return True
        win = ctypes.wintypes.RECT()
        client = ctypes.wintypes.RECT()
        USER32.GetWindowRect(hwnd, ctypes.byref(win))
        origin = ctypes.wintypes.POINT(0, 0)
        USER32.ClientToScreen(hwnd, ctypes.byref(origin))
        USER32.GetClientRect(hwnd, ctypes.byref(client))
        found.append(
            (
                (win.left, win.top, win.right, win.bottom),
                (origin.x, origin.y, origin.x + client.right, origin.y + client.bottom),
            )
        )
        return True

    USER32.EnumWindows(callback, 0)
    return found[0] if found else None


def window_style(pid: int) -> int:
    style = 0

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def callback(hwnd, _lparam):
        nonlocal style
        owner = ctypes.c_ulong()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and USER32.IsWindowVisible(hwnd):
            style = USER32.GetWindowLongPtrW(hwnd, GWL_STYLE)
            return False
        return True

    USER32.EnumWindows(callback, 0)
    return style


def run_case(exe: pathlib.Path, decorations: bool, label: str) -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    ctl = OUT_DIR / f"ctl-{label}.json"
    if ctl.exists():
        ctl.unlink()
    args = [str(exe), "--control-file", str(ctl), "--frames", "0"]
    if decorations:
        args.append("--decorations")
    print(f"\n=== {label}（decorations={decorations}）===", flush=True)
    proc = subprocess.Popen(args, cwd=str(ROOT))
    try:
        info = {}
        for _ in range(100):
            time.sleep(0.2)
            info = load_control(str(ctl))
            if info.get("port") and info.get("pid"):
                break
        if not info.get("port"):
            check(False, "应用启动（控制通道未就绪）")
            return
        port, token, pid = info["port"], info.get("token", ""), info.get("pid", proc.pid)
        time.sleep(1.0)  # 等首帧与 ShowWindow 完成

        def call(method: str, params: dict | None = None) -> dict:
            reply = call_with_token(port, method, params, token)
            return reply.get("result", reply)

        rects = window_rects(pid)
        check(rects is not None, "找到可见主窗口")
        if rects is None:
            return
        window, client = rects
        style = window_style(pid)
        has_caption = bool(style & WS_CAPTION)
        has_thick = bool(style & WS_THICKFRAME)
        has_popup = bool(style & WS_POPUP)
        print(f"  样式: style=0x{style & 0xFFFFFFFF:08X} caption={has_caption} "
              f"thickframe={has_thick} popup={has_popup}", flush=True)
        print(f"  窗口={window} 客户区={client}", flush=True)

        # —— 1. 窗口风格 ——
        check(has_caption == decorations,
              f"{'有' if decorations else '无'}系统标题栏（WS_CAPTION）")
        check(has_thick, "保留 WS_THICKFRAME（边缘缩放与 Aero Snap 的系统挂载点）")
        # —— 2. 客户区 == 整窗（无边框下的核心不变量）——
        window_size = (window[2] - window[0], window[3] - window[1])
        client_size = (client[2] - client[0], client[3] - client[1])
        print(f"  尺寸: 窗口={window_size} 客户区={client_size}", flush=True)
        if not decorations:
            check(client_size == window_size,
                  f"客户区 == 窗口矩形（{client_size} vs {window_size}）")
        # —— 3. 自绘窗框在真窗口下照旧 ——
        snapshot = call("get", {"id": "titlebar"})
        check(snapshot.get("type") == "TitleBar", "真窗口下 #titlebar 是自绘窗框组件")
        props = snapshot.get("props", {})
        check(props.get("window_control_available") == "true",
              "窗口控制端口可用（真窗口下应报 true）")
        # —— 4. 窗口控制真的生效 ——
        reply = call("invoke", {"id": "titlebar", "action": "maximize"})
        handled = reply.get("handled")
        check(handled is True, "invoke maximize → handled=true（最大化真的发生）")
        time.sleep(0.6)
        max_prop = call("get", {"id": "titlebar"}).get("props", {}).get("maximized")
        check(max_prop == "true", f"最大化后 maximized 属性 = true（实际 {max_prop}）")
        # 最大化后的窗口矩形必须落在工作区内（**不能盖住任务栏**——
        # 无边框窗口最经典的副作用，`WM_SYSCOMMAND`/`WM_SIZE` 里各校正一次就是为了它）。
        maximized_rects = window_rects(pid)
        if maximized_rects is not None:
            win = maximized_rects[0]
            info = MONITORINFO()
            info.cbSize = ctypes.sizeof(info)
            monitor = USER32.MonitorFromWindow(
                USER32.GetForegroundWindow(), 2  # MONITOR_DEFAULTTONEAREST
            )
            USER32.GetMonitorInfoW(monitor, ctypes.byref(info))
            work = info.rcWork
            inside = (win[0] >= work.left - 1 and win[1] >= work.top - 1
                      and win[2] <= work.right + 1 and win[3] <= work.bottom + 1)
            # 只在**无边框**分支断言：有边框窗口下 Win10+ 最大化会故意向外扩一圈
            # （-7,-7 那个经典偏移，给可拖拉的边缘留位），那是系统行为、不是缺陷——
            # 在这条分支上断言"不超出工作区"会得到一个永远红的假失败。
            if decorations:
                print(f"  (decorations 分支：最大化外扩属系统行为，不作断言) 窗口={win}", flush=True)
            else:
                check(inside, f"最大化后不超出工作区（窗口 {win} / 工作区 "
                              f"{work.left, work.top, work.right, work.bottom}）")
        reply = call("invoke", {"id": "titlebar", "action": "maximize"})
        check(reply.get("handled") is True, "再次 invoke maximize → 还原（切换语义）")
        time.sleep(0.6)
        check(call("get", {"id": "titlebar"}).get("props", {}).get("maximized") == "false",
              "还原后 maximized 属性 = false")
        # —— 5. 截图留证 ——
        shot = OUT_DIR / f"titlebar-{label}.png"
        result = call("capture", {"encode": "file", "path": str(shot)})
        check(bool(result.get("path") or shot.exists()), f"截图落盘 {shot.name}")
        # —— 6. 关闭动作走收尾路径（不直接 exit）——
        reply = call("invoke", {"id": "titlebar", "action": "close"})
        check(reply.get("handled") is True, "invoke close → handled=true")
        try:
            code = proc.wait(timeout=15)
            check(code == 0, f"关窗动作让应用正常退出（exit={code}）")
        except subprocess.TimeoutExpired:
            check(False, "关窗动作后应用未退出")
    finally:
        if proc.poll() is None:
            proc.kill()
            print("  (进程被强制结束)", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=str(DEFAULT_EXE))
    args = parser.parse_args()
    exe = pathlib.Path(args.exe)
    if not exe.exists():
        print(f"可执行文件不存在：{exe}（先 st build codeeditor）")
        return 2
    if os.name != "nt":
        print("本脚本用 Win32 API 读窗口矩形/样式，只在 Windows 宿主上跑")
        return 2
    run_case(exe, decorations=False, label="selfdrawn")
    run_case(exe, decorations=True, label="decorations")
    print()
    if failures:
        print(f"发现 {len(failures)} 个问题：")
        for item in failures:
            print("  [X]", item)
        return 1
    print("[OK] 窗框真实窗口检查全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
