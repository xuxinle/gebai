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
DEFAULT_EXE = ROOT / "build/debug/bin/gbcode.exe"
OUT_DIR = ROOT / "build/win-titlebar"
sys.path.insert(0, str(ROOT / "tools"))
from st_client_lib import call_with_token, load_control  # noqa: E402

USER32 = ctypes.WinDLL("user32", use_last_error=True)

# ⚠ **测量进程自己必须先声明 DPI 感知**，否则 `GetWindowRect`/`GetClientRect` 给的是
# 被 **虚拟化** 过的坐标（Windows 把它们按系统缩放折算成逻辑值，分数缩放再取整）。
# 实测教训（2026-11-01，1.5x 屏）：未声明时同一窗口报 2355x1281 窗口 / 2355x1280 客户区
# ——看起来是“客户区少了 1px”的真缺陷，实际是 3533x1921 物理像素被换算时的截断假象。
# 本脚本后面要拿像素做断言（阴影），坐标必须是**物理像素**，所以这一步不能省。
# 必须早于任何窗口枚举。
try:
    _set_ctx = USER32.SetProcessDpiAwarenessContext
    _set_ctx.argtypes = [ctypes.c_void_p]
    _set_ctx.restype = ctypes.c_bool
    _set_ctx(ctypes.c_void_p(-4))  # PER_MONITOR_AWARE_V2
except (AttributeError, OSError):
    pass  # 老系统：退到下面的 SetProcessDPIAware
try:
    USER32.SetProcessDPIAware()
except (AttributeError, OSError):
    pass


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


def window_handle(pid: int) -> int:
    """找该进程的主窗口句柄（与 `window_rects` 同一套枚举口径）。"""
    found: list[int] = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def callback(hwnd, _lparam):
        owner = ctypes.c_ulong()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and USER32.IsWindowVisible(hwnd):
            found.append(int(hwnd))
        return True

    USER32.EnumWindows(callback, 0)
    return found[0] if found else 0


GDI32 = ctypes.WinDLL("gdi32", use_last_error=True)


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", ctypes.c_uint32),
        ("biWidth", ctypes.c_int32),
        ("biHeight", ctypes.c_int32),
        ("biPlanes", ctypes.c_uint16),
        ("biBitCount", ctypes.c_uint16),
        ("biCompression", ctypes.c_uint32),
        ("biSizeImage", ctypes.c_uint32),
        ("biXPelsPerMeter", ctypes.c_int32),
        ("biYPelsPerMeter", ctypes.c_int32),
        ("biClrUsed", ctypes.c_uint32),
        ("biClrImportant", ctypes.c_uint32),
    ]


def grab_pixels(x: int, y: int, w: int, h: int) -> list[list[tuple[int, int, int]]] | None:
    """截屏一块区域，返回逐行 BGR 像素（bgh 顺序的 list[y][x]）。

    用 ctypes 直接调 GDI：本脚本要“看窗外那圈像素”，而控制通道只能拿应用自己的画布——
    阴影在**窗口之外**，只有屏幕截图里才有。
    """
    screen = USER32.GetDC(None)
    if not screen:
        return None
    mem = GDI32.CreateCompatibleDC(screen)
    header = BITMAPINFOHEADER()
    header.biSize = ctypes.sizeof(header)
    header.biWidth = w
    header.biHeight = -h  # 负数 = 自上而下
    header.biPlanes = 1
    header.biBitCount = 32
    header.biCompression = 0  # BI_RGB
    bits = ctypes.c_void_p()
    dib = GDI32.CreateDIBSection(
        screen, ctypes.byref(header), 0, ctypes.byref(bits), None, 0
    )
    rows = None
    if dib and bits:
        old = GDI32.SelectObject(mem, dib)
        if GDI32.BitBlt(mem, 0, 0, w, h, screen, x, y, 0x00CC0020 | 0x40000000):  # SRCCOPY|CAPTUREBLT
            raw = ctypes.string_at(bits, w * h * 4)
            rows = [
                [
                    (raw[(r * w + c) * 4], raw[(r * w + c) * 4 + 1], raw[(r * w + c) * 4 + 2])
                    for c in range(w)
                ]
                for r in range(h)
            ]
        GDI32.SelectObject(mem, old)
    if dib:
        GDI32.DeleteObject(dib)
    GDI32.DeleteDC(mem)
    USER32.ReleaseDC(None, screen)
    return rows


def luminance(bgr: tuple[int, int, int]) -> float:
    return 0.114 * bgr[0] + 0.587 * bgr[1] + 0.299 * bgr[2]


def check_window_shadow(rects, label: str) -> None:
    """断言**窗外有阴影**（自绘窗框最容易静默丢掉的那件事）。

    为什么要单独一条：无装饰窗口（`WS_POPUP` + 客户区=整窗）不会得到 DWM 阴影——
    它不给“框架窗口”以外的东西投影。而丢掉阴影**不报错、不影响任何已有断言**，
    只表现为“窗口和桌面糊成一片”（用户报的“没层次”）。因此必须有像素级断言。

    判据（二者取一即可，窗口可能贴在某条屏幕边上）：右侧或下侧之外 1..12px
    的亮度**显著低于** 30px 处的背景，且随距离回升（阴影的指纹）。
    """
    window, _client = rects
    band = 30
    x = window[0] - 6
    y = window[1] - 6
    w = (window[2] - window[0]) + 2 * band
    h = (window[3] - window[1]) + 2 * band
    if x < 0 or y < 0 or w > 8000 or h > 8000:
        print(f"  (窗口贴屏幕边缘，跳过阴影像素断言：window={window})", flush=True)
        return
    pixels = grab_pixels(x, y, w, h)
    if pixels is None:
        check(False, "截屏取像素（阴影断言）")
        return
    right = window[2] - 1 - x
    bottom = window[3] - 1 - y

    def column_profile(offset: int) -> list[float]:
        vals = []
        for d in range(1, band):
            col = right + d
            if col >= w:
                vals.append(float("nan"))
                continue
            ys = range(10, h - 10, 2)
            sample = [luminance(pixels[r][col]) for r in ys]
            vals.append(sum(sample) / len(sample) if sample else float("nan"))
        return vals

    def row_profile(offset: int) -> list[float]:
        vals = []
        for d in range(1, band):
            row = bottom + d
            if row >= h:
                vals.append(float("nan"))
                continue
            sample = [luminance(pixels[row][c]) for c in range(10, w - 10, 2)]
            vals.append(sum(sample) / len(sample) if sample else float("nan"))
        return vals

    def shadow_strength(profile: list[float]) -> float:
        """近处（1..3px）比远处（24..28px）暗多少（正=有阴影）。"""
        near = [v for v in profile[0:3] if v == v]
        far = [v for v in profile[23:28] if v == v]
        if not near or not far:
            return 0.0
        return sum(far) / len(far) - sum(near) / len(near)

    strength_r = shadow_strength(column_profile(right))
    strength_b = shadow_strength(row_profile(bottom))
    best = max(strength_r, strength_b)
    print(f"  阴影强度: 右侧={strength_r:.1f} 下侧={strength_b:.1f}（>5 视为有阴影）", flush=True)
    check(best > 5.0, f"{label}: 窗外有系统阴影（自绘窗框不再与背景糊成一片）")


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
        # —— 3. 窗外有阴影（自绘窗框丢了系统边框，DWM 就默认不投影）——
        # 必须放在窗口控制动作**之前**：下面会最大化/还原，窗口位置和尺寸都变了。
        if not decorations:
            check_window_shadow(rects, label)
        # —— 4. 自绘窗框在真窗口下照旧 ——
        snapshot = call("get", {"id": "titlebar"})
        check(snapshot.get("type") == "TitleBar", "真窗口下 #titlebar 是自绘窗框组件")
        props = snapshot.get("props", {})
        check(props.get("window_control_available") == "true",
              "窗口控制端口可用（真窗口下应报 true）")
        # —— 5. 窗口控制真的生效 ——
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
        # —— 6. 截图留证 ——
        shot = OUT_DIR / f"titlebar-{label}.png"
        result = call("capture", {"encode": "file", "path": str(shot)})
        check(bool(result.get("path") or shot.exists()), f"截图落盘 {shot.name}")
        # —— 7. 关闭动作走收尾路径（不直接 exit）——
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
        print(f"可执行文件不存在：{exe}（先 st build gbcode）")
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
