#!/usr/bin/env python3
"""失焦灰框回归量测：让真窗口**失焦**，再看窗口内侧边上还是不是应用自己的像素。

用户报的就是这一条：「霜天桌面窗口…会有未激活的灰色边框问题」。所以判据必须是
**失焦态**，而不是激活态——两者在修复前差别巨大（激活 0px / 失焦 10px）。

三条判据（都对着真窗口、真 DWM）：
  ① 失焦后 `PrintWindow(PW_RENDERFULLCONTENT)` 得到的合成像素，与**应用画布**逐像素一致
     （差异像素 0）——系统没有往客户区里塞任何东西；
  ② 失焦态下窗口**内侧 0..15px** 与画布同位置像素一致（灰带不会再出现）；
  ③ 取景落图（`focused.png` / `unfocused.png` / 边缘放大图），供人眼核对。

为什么要用 PrintWindow 而不是桌面截屏：桌面截屏抓的是屏幕上当前的可见像素，别的窗口压在上面
时读到的是别人的颜色（本次第一版就因此把浏览器的侧栏读成"霜天的灰框"）。PrintWindow 让窗口
自己把合成后的内容画出来，无视遮挡，同时仍包含 DWM 施加在客户区上的效果。

用法：python tools/window_unfocused_frame_check.py [--exe PATH] [--out DIR]
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

failures: list[str] = []


def check(ok: bool, message: str) -> None:
    print(f"  [{'OK' if ok else 'X'}] {message}", flush=True)
    if not ok:
        failures.append(message)


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


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


def _image(w: int, h: int, bits):
    from PIL import Image  # noqa: PLC0415

    buf = (ctypes.c_ubyte * (w * h * 4)).from_address(bits.value)
    return Image.frombuffer("RGB", (w, h), bytes(buf), "raw", "BGRX", 0, 1).copy()


def print_window(hwnd, w: int, h: int):
    hdc_screen = USER32.GetDC(None)
    hdc_mem = GDI32.CreateCompatibleDC(hdc_screen)
    hbmp, bits = _dib(hdc_screen, w, h)
    old = GDI32.SelectObject(hdc_mem, hbmp)
    ok = USER32.PrintWindow(hwnd, hdc_mem, PW_RENDERFULLCONTENT)
    img = _image(w, h, bits)
    GDI32.SelectObject(hdc_mem, old)
    GDI32.DeleteObject(hbmp)
    GDI32.DeleteDC(hdc_mem)
    USER32.ReleaseDC(None, hdc_screen)
    return bool(ok), img


def find_window(pid: int):
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


def make_focus_thief():
    """造一个抢焦点用的**远处小窗**：它必须离被测窗口足够远，不能盖住它。

    为什么不能用「把焦点交给另一个已存在的窗口」：那个窗口的位置不可控。
    本次第一版就吃过亏——抢焦点的背板窗被抬到被测窗之上，读出来全是背板色。
    """
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetModuleHandleW.restype = ctypes.c_void_p
    kernel32.GetModuleHandleW.argtypes = [ctypes.wintypes.LPCWSTR]
    instance = kernel32.GetModuleHandleW(None)
    USER32.CreateWindowExW.restype = ctypes.wintypes.HWND
    USER32.CreateWindowExW.argtypes = [
        ctypes.wintypes.DWORD, ctypes.wintypes.LPCWSTR, ctypes.wintypes.LPCWSTR,
        ctypes.wintypes.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ctypes.wintypes.HWND, ctypes.wintypes.HMENU, ctypes.c_void_p, ctypes.c_void_p]
    hwnd = USER32.CreateWindowExW(
        0x00000008 | 0x00000080,  # WS_EX_TOPMOST | WS_EX_TOOLWINDOW
        "STATIC", "focus-thief",
        0x10000000 | 0x02000000,  # WS_POPUP | WS_VISIBLE
        5, 5, 120, 40, None, None, instance, None)
    return hwnd


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=str(DEFAULT_EXE))
    parser.add_argument("--out", default=str(ROOT / "build/win-unfocused-frame"))
    parser.add_argument("--delay", type=float, default=3.0)
    parser.add_argument("--hit-test", action="store_true",
                        help="额外验证 WM_NCHITTEST：客户区内部不应报 HTLEFT 之类")
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
    thief = None
    try:
        handle = load_control(ctl)
        deadline = time.time() + 30
        while not handle and time.time() < deadline:
            time.sleep(0.2)
            handle = load_control(ctl)
        if not handle:
            print("控制文件未出现（应用起不来？）")
            return 3
        time.sleep(args.delay)
        hwnd = find_window(proc.pid)
        if not hwnd:
            print("找不到窗口")
            return 3
        rect = RECT()
        USER32.GetWindowRect(hwnd, ctypes.byref(rect))
        w, h = rect.right - rect.left, rect.bottom - rect.top
        client = RECT()
        USER32.GetClientRect(hwnd, ctypes.byref(client))
        print(f"窗口 {w}x{h} @ ({rect.left},{rect.top}) 客户区 {client.right}x{client.bottom} "
              f"zoomed={USER32.IsZoomed(hwnd)}")
        check(client.right == w and client.bottom == h,
              f"客户区 == 整窗（{client.right}x{client.bottom} vs {w}x{h}）——自绘窗框的前提")

        from PIL import Image  # noqa: PLC0415

        canvas_path = out / "canvas.png"
        if canvas_path.exists():
            canvas_path.unlink()
        call_with_token(handle["port"], "capture", {"path": str(canvas_path)},
                        handle.get("token", ""))
        if not canvas_path.exists():
            print("capture 未产出图片")
            return 4
        canvas = Image.open(canvas_path).convert("RGB")

        # —— 失焦：用一个远处的小窗把焦点抢走 ——
        thief = make_focus_thief()
        USER32.ShowWindow(thief, 5)   # SW_SHOW
        USER32.SetForegroundWindow(thief)
        time.sleep(1.0)
        focused_now = USER32.GetForegroundWindow()
        check(focused_now != hwnd, f"窗口已失焦（前台={focused_now})")

        ok, composed = print_window(hwnd, w, h)
        composed.save(out / "unfocused.png")
        check(ok, "PrintWindow(PW_RENDERFULLCONTENT) 成功")
        if canvas.size != composed.size:
            check(False, f"尺寸不一致：画布 {canvas.size} vs 合成 {composed.size}")
            return 0

        # ① **不做**整窗逐像素对比。
        #
        # 实测（2026-11-05）：GPU 画布（swapchain）窗口上 `PrintWindow` 拿到的内容**部分过期**
        # ——即使窗口处于激活态，也能看到整行渐变条颜色差 18、某列整块是上帧内容。
        # 它靠得住的只有“合成层面”的东西（圆角遮罩、系统描边），像素内容不可信。
        # 所以本脚本只用它验**窗缘 16px**（那里是平坦背景，实测 0 处不一致）。
        # 真要做整窗逐像素断言：用 `--renderer software` 起应用（软件画布 → window DC 即画布）。
        #
        # 圆角改用系统回答（`GetWindowRgn` 是非题）：3 = SIMPLEREGION = 圆角在。
        gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
        region = gdi32.CreateRectRgn(0, 0, 1, 1)
        kind = USER32.GetWindowRgn(ctypes.wintypes.HWND(hwnd), region)
        gdi32.DeleteObject(region)
        check(kind == 3, f"圆角裁剪区在（GetWindowRgn={kind}，3=SIMPLEREGION）")

        # ② 内侧边带：左/上/右/下各取 15px 的采样
        cp, sp = canvas.load(), composed.load()
        bands = {
            "左": [(x, h // 2) for x in range(0, 16)],
            "上": [(w // 2, y) for y in range(0, 16)],
            "右": [(w - 1 - x, h // 2) for x in range(0, 16)],
            "下": [(w // 2, h - 1 - y) for y in range(0, 16)],
        }
        for name, points in bands.items():
            bad = [(x, y, cp[x, y], sp[x, y]) for (x, y) in points
                   if max(abs(cp[x, y][i] - sp[x, y][i]) for i in range(3)) > 2]
            check(not bad, f"失焦态：{name}侧 16px 内无系统像素（不一致 {len(bad)} 处）"
                           + (f"，首个 {bad[0]}" if bad else ""))

        # ③ 落图：直接看有没有那圈灰边
        for name, box in (("edge_left", (0, h // 2 - 6, 32, h // 2 + 6)),
                          ("edge_top", (w // 2 - 6, 0, w // 2 + 6, 32))):
            crop = composed.crop(box)
            crop.resize((crop.width * 8, crop.height * 8), Image.NEAREST).save(out / f"{name}.png")
        print(f"  取景落图：{out}")

        if args.hit_test:
            # WM_NCHITTEST：客户区内部（远离边缘与标题栏）必须回 HTCLIENT，
            # 否则整个客户区都成了拖拽/缩放带（实测过：标题栏以外的空白处拖不动应用、
            # 但拖进去就整窗乱动）。
            WM_NCHITTEST = 0x0084
            HTCLIENT, HTCAPTION, HTLEFT, HTRIGHT, HTTOP, HTBOTTOM = 1, 2, 10, 11, 12, 15
            names = {HTCLIENT: "HTCLIENT", HTCAPTION: "HTCAPTION", HTLEFT: "HTLEFT",
                     HTRIGHT: "HTRIGHT", HTTOP: "HTTOP", HTBOTTOM: "HTBOTTOM"}
            probes = {
                "正中心": (rect.left + w // 2, rect.top + h // 2),
                "左中(2px)": (rect.left + 2, rect.top + h // 2),
                "右上空白": (rect.left + w - 40, rect.top + 60),
                "标题栏": (rect.left + w // 2, rect.top + 12),
            }
            for label, (sx, sy) in probes.items():
                lparam = ((sy & 0xFFFF) << 16) | (sx & 0xFFFF)
                result = USER32.SendMessageW(hwnd, WM_NCHITTEST, 0, lparam)
                print(f"    {label} @ ({sx},{sy}) → {names.get(result, hex(result))}")
            centre = USER32.SendMessageW(hwnd, WM_NCHITTEST, 0,
                                         (((rect.top + h // 2) & 0xFFFF) << 16) |
                                         ((rect.left + w // 2) & 0xFFFF))
            check(centre == HTCLIENT, "客户区正中心命中 HTCLIENT（不是拖拽/缩放带）")
    finally:
        if thief:
            USER32.DestroyWindow(thief)
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
