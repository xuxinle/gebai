#!/usr/bin/env python3
"""真窗口「拖动缩放」取证：拖动中画面是否 1:1、拖动中到底渲染了几帧。

为什么必须是**真鼠标 + 真窗口**：缩放走的是 Win32 模态循环
（`WM_NCLBUTTONDOWN` + `HT*` → `DefWindowProc` 的 `SC_SIZE`）。那个循环**阻塞应用主循环**，
窗口过程成为唯一的渲染触发点。合成一个 `WM_ENTERSIZEMOVE`/`SetWindowPos` 序列是**另一种
负载**（主循环照跑），得到的帧数结论会与用户看到的不一样。因此本脚本用 `SendInput`
真按下窗口边缘、真移动、真松开。

三个判据（全部是**用户视角**的口径）：
  1. **拖动中的画面 == 松手后的画面**（同一尺寸下逐像素比对）——拉伸畸变会让两者差一大截；
  2. **拖动中渲染了帧**：控制通道 `metrics.frames` 的增量（模态循环里主循环不动，
     只有新的渲染触发点存在时这个数才会涨）；
  3. 尺寸/渲染器如实记录，供归因。

抓的是**屏幕像素**（`GetDC(NULL)`），不是应用自己的截图接口——用户看的就是屏幕。
⚠ 本进程必须声明 DPI 感知，否则 `GetWindowRect` 给的是虚拟化后的逻辑坐标，与屏幕 DC 对不上。

用法：
  python tools/live_resize_probe.py --exe build/dev/bin/gallery.exe --label baseline
  python tools/live_resize_probe.py --exe build/dev/bin/gallery.exe --renderer software
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
DEFAULT_EXE = ROOT / "build/dev/bin/gallery.exe"
OUT_DIR = ROOT / "build/live-resize"
sys.path.insert(0, str(ROOT / "tools"))
from st_client_lib import call_with_token, load_control  # noqa: E402

USER32 = ctypes.WinDLL("user32", use_last_error=True)
GDI32 = ctypes.WinDLL("gdi32", use_last_error=True)

# ⚠ 必须显式声明 argtypes：默认 c_int 会把 64 位 HWND 截断（实测 `SetWindowPos` 直接
# 返回 1400「无效窗口句柄」），而失败是静默的——脚本会以为"窗口没动"是应用的问题。
USER32.EnumWindows.argtypes = [ctypes.c_void_p, wt.LPARAM]
USER32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
USER32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
USER32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, ctypes.c_int, ctypes.c_uint]
USER32.SetWindowPos.restype = wt.BOOL
USER32.SetForegroundWindow.argtypes = [wt.HWND]
USER32.SetForegroundWindow.restype = wt.BOOL
USER32.GetForegroundWindow.restype = wt.HWND
USER32.SendInput.argtypes = [wt.UINT, ctypes.c_void_p, ctypes.c_int]
USER32.SendInput.restype = wt.UINT

# —— SendInput 结构 ——
INPUT_MOUSE = 0
MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_ABSOLUTE = 0x8000
MOUSEEVENTF_VIRTUALDESK = 0x4000


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD),
                ("dwFlags", wt.DWORD), ("time", wt.DWORD),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("mi", MOUSEINPUT)]

    _anonymous_ = ("u",)
    _fields_ = [("type", wt.DWORD), ("u", _U)]


def send_mouse(flags: int, x: int = 0, y: int = 0) -> None:
    """绝对坐标移动/按键（屏幕物理像素）。"""
    screen_w = USER32.GetSystemMetrics(0)
    screen_h = USER32.GetSystemMetrics(1)
    # 绝对模式：0..65535 映射整个虚拟桌面
    ax = int(round(x * 65535 / max(1, screen_w - 1)))
    ay = int(round(y * 65535 / max(1, screen_h - 1)))
    item = INPUT(type=INPUT_MOUSE)
    item.mi = MOUSEINPUT(dx=ax, dy=ay, mouseData=0,
                         dwFlags=flags | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK,
                         time=0, dwExtraInfo=None)
    sent = USER32.SendInput(1, ctypes.byref(item), ctypes.sizeof(INPUT))
    if sent != 1:
        raise OSError(f"SendInput 失败：{ctypes.get_last_error()}")


HWND_TOPMOST = -1
HWND_NOTOPMOST = -2
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_NOACTIVATE = 0x0010
SWP_SHOWWINDOW = 0x0040


def find_window(pid: int):
    """找该进程的可见顶层窗口（取面积最大的那个）。"""
    found: list[int] = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    def callback(hwnd, _lparam):
        owner = wt.DWORD()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and USER32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    USER32.EnumWindows(callback, 0)
    if not found:
        return None
    return max(found, key=lambda h: area(window_rect(h)))


def grab_stable(hwnd, settle: float = 0.35, retries: int = 4):
    """抓一次**稳定**的窗口画面：停住鼠标 → 读矩形 → 抓 → 复核矩形，变了就重试。

    为什么必须复核：拖动是异步的，"先读矩形再抓屏"中间窗口还会长——于是抓到的是
    "某个更早尺寸的裁剪"，与参考图比出来的是**尺寸不匹配**而不是渲染差异
    （实测拿这个脏数据得出了 mean|Δ|=22.1 的假阳性）。停住鼠标能让消息队列耗尽，
    复核则保证"抓的这一帧就是该尺寸的那一帧"。
    """
    for _ in range(retries):
        time.sleep(settle)
        before = window_rect(hwnd)
        pixels = grab_screen(before)
        after = window_rect(hwnd)
        if before == after:
            return before, pixels
    return window_rect(hwnd), grab_screen(window_rect(hwnd))


def area(rect: tuple[int, int, int, int]) -> int:
    return max(0, rect[2] - rect[0]) * max(0, rect[3] - rect[1])


def place_and_raise(hwnd, x: int, y: int, width: int, height: int) -> None:
    """把窗口搬到指定位置并**置顶**。

    为什么必须置顶：真鼠标事件的落点是**屏幕命中测试**，窗口在别的窗口底下时
    点的是别的窗口（实测：不置顶时 `metrics.frames` 纹丝不动、窗口矩形也没变——
    事件根本就没进应用）。置顶是让"真鼠标"这条链路可行的前提。
    """
    USER32.SetWindowPos(hwnd, wt.HWND(HWND_TOPMOST), x, y, width, height,
                        SWP_SHOWWINDOW)
    time.sleep(0.4)
    # 后台进程抢前台会被系统拒绝：先合成一次 ALT 按键（系统把"用户刚有输入"的进程
    # 视为可信来源）。不这么做时前台仍是别的窗口，真鼠标事件会落到那上面。
    USER32.keybd_event(0x12, 0, 0, 0)
    USER32.keybd_event(0x12, 0, 2, 0)
    USER32.SetForegroundWindow(hwnd)
    time.sleep(0.3)


def window_rect(hwnd) -> tuple[int, int, int, int]:
    rect = wt.RECT()
    USER32.GetWindowRect(hwnd, ctypes.byref(rect))
    return (rect.left, rect.top, rect.right, rect.bottom)


def grab_screen(rect: tuple[int, int, int, int]) -> bytes:
    """抓屏幕的一块（物理像素，BGRA 自顶向下）。"""
    left, top, right, bottom = rect
    width, height = right - left, bottom - top

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                    ("biPlanes", wt.WORD), ("biBitCount", wt.WORD),
                    ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                    ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG),
                    ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]

    class BITMAPINFO(ctypes.Structure):
        _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wt.DWORD * 3)]

    screen_dc = USER32.GetDC(None)
    memory_dc = GDI32.CreateCompatibleDC(screen_dc)
    info = BITMAPINFO()
    info.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    info.bmiHeader.biWidth = width
    info.bmiHeader.biHeight = -height  # 自顶向下
    info.bmiHeader.biPlanes = 1
    info.bmiHeader.biBitCount = 32
    info.bmiHeader.biCompression = 0
    bits = ctypes.c_void_p()
    bitmap = GDI32.CreateDIBSection(screen_dc, ctypes.byref(info), 0, ctypes.byref(bits), None, 0)
    GDI32.SelectObject(memory_dc, bitmap)
    GDI32.BitBlt(memory_dc, 0, 0, width, height, screen_dc, left, top, 0x00CC0020)  # SRCCOPY
    buffer = ctypes.string_at(bits, width * height * 4)
    GDI32.DeleteObject(bitmap)
    GDI32.DeleteDC(memory_dc)
    USER32.ReleaseDC(None, screen_dc)
    return buffer


def save_png(path: pathlib.Path, rect: tuple[int, int, int, int], bgra: bytes) -> bool:
    try:
        from PIL import Image
    except ImportError:
        path.with_suffix(".raw").write_bytes(bgra)
        return False
    width, height = rect[2] - rect[0], rect[3] - rect[1]
    image = Image.frombuffer("RGBA", (width, height), bgra, "raw", "BGRA", 0, 1)
    image.convert("RGB").save(path)
    return True


def diff_stats(a: bytes, b: bytes) -> dict:
    """逐像素差异（BGRA）。返回平均绝对差与"明显不同"的像素占比。"""
    if len(a) != len(b):
        return {"shape_mismatch": True, "len_a": len(a), "len_b": len(b)}
    total = 0
    big = 0
    worst = 0
    pixels = len(a) // 3
    for index in range(0, len(a), 3):
        delta = max(abs(a[index] - b[index]),
                    abs(a[index + 1] - b[index + 1]),
                    abs(a[index + 2] - b[index + 2]))
        total += delta
        if delta > 8:
            big += 1
        if delta > worst:
            worst = delta
    return {"mean_abs": round(total / max(1, pixels), 3),
            "big_ratio": round(big / max(1, pixels), 4),
            "worst": worst}


diff_images = diff_stats


def run(args) -> int:
    exe = pathlib.Path(args.exe)
    if not exe.exists():
        print(f"可执行文件不存在：{exe}")
        return 2
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    control = OUT_DIR / f"ctl-{args.label}.json"
    if control.exists():
        control.unlink()
    command = [str(exe), "--control-file", str(control), "--theme", args.theme]
    if args.renderer:
        command += ["--renderer", args.renderer]
    if args.scale:
        command += ["--scale", str(args.scale)]
    print(f"启动：{' '.join(command)}", flush=True)
    proc = subprocess.Popen(command, cwd=str(ROOT))

    def finish(code: int) -> int:
        if proc.poll() is None:
            proc.kill()
        return code

    try:
        info = {}
        for _ in range(120):
            time.sleep(0.2)
            info = load_control(str(control))
            if info.get("port") and info.get("pid"):
                break
        if not info.get("port"):
            print("应用未就绪（控制通道没起来）")
            return finish(2)
        port, token, pid = info["port"], info.get("token", ""), info.get("pid", proc.pid)
        time.sleep(1.2)

        def call(method: str, params: dict | None = None) -> dict:
            reply = call_with_token(port, method, params, token, timeout=20)
            return reply.get("result", reply)

        hwnd = find_window(pid)
        if hwnd is None:
            print("找不到可见主窗口")
            return finish(2)
        place_and_raise(hwnd, args.at_x, args.at_y, args.win_w, args.win_h)
        foreground = USER32.GetForegroundWindow()
        print(f"窗口 hwnd={hwnd:#x} 前台={foreground:#x} "
              f"{'（已置顶）' if foreground == hwnd else '⚠ 未取得前台，鼠标事件可能落到别的窗口'}")

        metrics = call("metrics")
        print(f"backend={metrics.get('backend')} renderer={metrics.get('renderer')} "
              f"scale={metrics.get('device_scale')} phys={metrics.get('physical_width')}"
              f"x{metrics.get('physical_height')}")
        print(f"renderer_note={metrics.get('renderer_note')}")
        frames_before = metrics.get("frames", 0)

        time.sleep(0.6)
        rect0 = window_rect(hwnd)
        print(f"窗口 {rect0} 尺寸 {rect0[2] - rect0[0]}x{rect0[3] - rect0[1]}")
        origin_rect = rect0
        shot_before = OUT_DIR / f"resize-{args.label}-0-before.png"
        save_png(shot_before, rect0, grab_screen(rect0))

        # —— 真鼠标拖动右边框 ——
        left, top, right, bottom = rect0
        y = (top + bottom) // 2
        grab_x = right - 3  # 缩放命中带（逻辑 6px）内
        send_mouse(MOUSEEVENTF_MOVE, grab_x, y)
        time.sleep(0.15)
        send_mouse(MOUSEEVENTF_LEFTDOWN, grab_x, y)
        time.sleep(0.15)
        steps = max(1, args.steps)
        mid_shot = None
        mid_rect = None
        step_times: list[float] = []
        try:
            for index in range(1, steps + 1):
                x = grab_x + int(args.delta * index / steps)
                start = time.perf_counter()
                send_mouse(MOUSEEVENTF_MOVE, x, y)
                time.sleep(0.012)  # 贴近真实拖动的消息节奏（~60 次/秒）
                step_times.append((time.perf_counter() - start) * 1000.0)
                if index == int(steps * args.mid_at):
                    # 停住鼠标再抓（见 `grab_stable`）：**鼠标不动**时不会有新的 WM_SIZE，
                    # 抓到的那一帧就是当前尺寸下的最终画面。
                    time.sleep(args.hold)
                    mid_rect, mid_pixels = grab_stable(hwnd, settle=0.05)
                    mid_shot = OUT_DIR / f"resize-{args.label}-mid.png"
                    save_png(mid_shot, mid_rect, mid_pixels)
        finally:
            send_mouse(MOUSEEVENTF_LEFTUP, grab_x + int(args.delta), y)

        time.sleep(args.settle)
        rect1, final_pixels = grab_stable(hwnd)

        # —— 判据 1：拖动中的画面 vs **同一尺寸下的正确渲染** ——
        #
        # 比对必须控制"尺寸相同"这一个变量：拖动中抓到的那张，尺寸是 S_mid；
        # 松手后应用会把窗口停在 S_final（略大一截），两张图直接比就是比两种尺寸。
        # 做法：把窗口（不用鼠标）改成 S_mid，等它重绘完，得到的才是"S_mid 下的正确画面"。
        if mid_shot is None or mid_rect is None:
            print("未取到拖动中的截图，跳过画面比对")
            return finish(2)
        USER32.SetWindowPos(hwnd, wt.HWND(HWND_TOPMOST), mid_rect[0], mid_rect[1],
                            mid_rect[2] - mid_rect[0], mid_rect[3] - mid_rect[1],
                            SWP_SHOWWINDOW | SWP_NOMOVE)
        time.sleep(1.0)  # 等重建 + 重绘落地
        rect_mid_after, mid_pixels_now = grab_stable(hwnd)
        mid_ref = OUT_DIR / f"resize-{args.label}-mid-reference.png"
        save_png(mid_ref, rect_mid_after, mid_pixels_now)
        # 拖动中那一张从文件重读（与参考图同源口径，避免两次抓屏的时序差异混进来）
        from PIL import Image  # noqa: PLC0415
        during = Image.open(mid_shot).convert("RGB")
        reference = Image.open(mid_ref).convert("RGB")
        stats = diff_images(during.tobytes(), reference.tobytes())
        print(f"拖动中画面 vs 同尺寸正确画面：{stats}")
        print(f"  （参考图比对的尺寸：{rect_mid_after[2] - rect_mid_after[0]}"
              f"x{rect_mid_after[3] - rect_mid_after[1]}，与拖动中 {mid_rect} 一致）")

        metrics2 = call("metrics")
        frames_after = metrics2.get("frames", 0)
        print(f"拖动后窗口 {rect1} 尺寸 {rect1[2] - rect1[0]}x{rect1[3] - rect1[1]}"
              f"（预期 +{args.delta}）")
        after = OUT_DIR / f"resize-{args.label}-final.png"
        save_png(after, rect1, final_pixels)
        print(f"拖动中渲染帧数（metrics.frames 增量）= {frames_after - frames_before}"
              f" / 拖动步数 {steps}")
        print(f"单步 SendInput 往返 p50={sorted(step_times)[len(step_times) // 2]:.2f} ms"
              f" max={max(step_times):.2f} ms")

        result: dict = {"label": args.label, "exe": str(exe), "renderer": metrics.get("renderer"),
                        "renderer_note": metrics.get("renderer_note"),
                        "scale": metrics.get("device_scale"), "steps": steps,
                        "delta": args.delta, "frames_during_drag": frames_after - frames_before,
                        "size_before": [rect0[2] - rect0[0], rect0[3] - rect0[1]],
                        "size_after": [rect1[2] - rect1[0], rect1[3] - rect1[1]],
                        "size_at_probe": [mid_rect[2] - mid_rect[0], mid_rect[3] - mid_rect[1]],
                        "drag_vs_correct_same_size": stats,
                        "step_ms_p50": round(sorted(step_times)[len(step_times) // 2], 2),
                        "step_ms_max": round(max(step_times), 2)}
        print(f"拖动中尺寸 {mid_rect} vs 松手后 {rect1}（两张直接比就是比两种尺寸，不比）")
        # 收尾：把窗口放回原处、取消置顶（不留痕在用户桌面上）
        USER32.SetWindowPos(hwnd, wt.HWND(HWND_NOTOPMOST), origin_rect[0], origin_rect[1],
                            origin_rect[2] - origin_rect[0], origin_rect[3] - origin_rect[1],
                            SWP_SHOWWINDOW)
        (OUT_DIR / f"resize-{args.label}.json").write_text(
            json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"结果写入 {(OUT_DIR / f'resize-{args.label}.json').name}")
        return finish(0)
    finally:
        if proc.poll() is None:
            proc.kill()


def main() -> int:
    # Windows 控制台默认是 GBK：脚本里的箭头/星号等字符会让 print 直接抛 UnicodeEncodeError。
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass
    # ⚠ 必须声明 DPI 感知：否则 GetWindowRect/屏幕 DC 走虚拟化坐标，抓图会错位。
    try:
        ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)  # PROCESS_PER_MONITOR_DPI_AWARE
    except Exception:  # noqa: BLE001
        USER32.SetProcessDPIAware()

    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=str(DEFAULT_EXE))
    parser.add_argument("--renderer", default="")
    parser.add_argument("--scale", default="")
    parser.add_argument("--theme", default="light")
    parser.add_argument("--steps", type=int, default=30)
    parser.add_argument("--delta", type=int, default=240)
    parser.add_argument("--mid-at", type=float, default=0.6)
    parser.add_argument("--hold", type=float, default=0.35,
                        help="抓拖动中画面前的停顿（让消息队列耗尽、画面稳定）")
    parser.add_argument("--settle", type=float, default=0.6)
    parser.add_argument("--label", default="run")
    parser.add_argument("--at-x", type=int, default=200)
    parser.add_argument("--at-y", type=int, default=200)
    parser.add_argument("--win-w", type=int, default=900)
    parser.add_argument("--win-h", type=int, default=600)
    args = parser.parse_args()
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
