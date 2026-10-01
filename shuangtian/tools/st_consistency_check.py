#!/usr/bin/env python3
"""内置通道与桌面一致性检查（Windows 真机）。

回答的问题：「内置通道（无头）截图」与「桌面窗口实际显示」是否一致？
分两层验证（都要求安装 Windows + 该机有显示器）：

① **窗口呈现无损**：应用帧缓冲 vs 窗口客户区实际像素（PrintWindow 抓取）。
   若呈现路径引入了缩放/DXGI 拉伸（历史上两次真实缺陷），这里不是 0 就会报出来。
② **两通道同源**：同参数下（相同 scale / 文本形态）无头截图 vs 窗口帧缓冲。
   默认参数下应当只剩很小的边缘差异（GPU 合成 vs 软件光栅的容差），
   而不是「一种灰度一种彩边」这种整体形态差异。

用法：
    python tools/st_consistency_check.py [--exe PATH] [--app gallery] [--scale 1.5]
退出码 0 = 全部通过。

依赖：Windows + Pillow（`pip install pillow`）。非 Windows 直接跳过（退出 0）。
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if os.name != "nt":
    print("跳过：本脚本只在 Windows 上运行（需要在真机抓窗口像素）")
    sys.exit(0)

try:
    from PIL import Image  # noqa: E402
except ImportError:
    print("跳过：缺少 Pillow（pip install pillow）")
    sys.exit(0)

try:
    ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    ctypes.windll.user32.SetProcessDPIAware()

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
PW_CLIENTONLY = 0x1
PW_RENDERFULLCONTENT = 0x2


class Client:
    """单连接会话：控制协议要求每连接先 hello 握手，后续请求复用同一连接。"""

    def __init__(self, port: int, token: str) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=60.0)
        self.sock.settimeout(60.0)
        self.counter = 0
        hello = self.call("hello", {"token": token})
        if not hello.get("ok"):
            raise RuntimeError(f"hello 失败: {hello.get('error')}")

    def call(self, method: str, params: dict | None = None) -> dict:
        self.counter += 1
        body = json.dumps({"id": self.counter, "method": method, "params": params or {}}).encode()
        self.sock.sendall(struct.pack(">I", len(body)) + body)
        length = struct.unpack(">I", self._read_exact(4))[0]
        return json.loads(self._read_exact(length).decode())

    def ok(self, method: str, params: dict | None = None) -> dict:
        reply = self.call(method, params)
        if not reply.get("ok"):
            raise RuntimeError(f"{method} 失败: {reply.get('error')}")
        return reply.get("result") or {}

    def _read_exact(self, size: int) -> bytes:
        buffer = b""
        while len(buffer) < size:
            chunk = self.sock.recv(size - len(buffer))
            if not chunk:
                raise RuntimeError("连接被关闭")
            buffer += chunk
        return buffer

    def close(self) -> None:
        self.sock.close()


def launch(exe: Path, extra: list[str], ctl: Path, log: Path) -> tuple[subprocess.Popen, Client]:
    ctl.unlink(missing_ok=True)
    handle = open(log, "wb")
    process = subprocess.Popen([str(exe), "--control-port", "0", "--control-file", str(ctl)] + extra,
                               stdout=handle, stderr=handle, stdin=subprocess.DEVNULL)
    for _ in range(160):
        if ctl.exists():
            try:
                info = json.loads(ctl.read_text(encoding="utf-8"))
                if info.get("port"):
                    return process, Client(int(info["port"]), info.get("token", ""))
            except (OSError, RuntimeError, json.JSONDecodeError):
                pass
        if process.poll() is not None:
            tail = log.read_text(encoding="utf-8", errors="replace")[-600:]
            raise RuntimeError(f"进程提前退出（code={process.returncode}）：\n{tail}")
        time.sleep(0.25)
    raise RuntimeError("控制文件未生成（应用未就绪）")


def stop(process: subprocess.Popen, client: Client) -> None:
    try:
        client.ok("shutdown")
        client.close()
    except Exception:
        pass
    for _ in range(24):
        if process.poll() is not None:
            return
        time.sleep(0.25)
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()


def find_window(title: str) -> int:
    hwnd = user32.FindWindowW(None, title)
    if hwnd:
        return hwnd
    matches: list[int] = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def visit(hwnd_any, _):
        hwnd_int = int(hwnd_any)
        length = user32.GetWindowTextLengthW(hwnd_int)
        if length:
            buf = ctypes.create_unicode_buffer(length + 1)
            user32.GetWindowTextW(hwnd_int, buf, length + 1)
            if title in buf.value:
                matches.append(hwnd_int)
        return True

    user32.EnumWindows(visit, None)
    return matches[0] if matches else 0


def print_window(hwnd: int) -> Image.Image:
    """抓窗口客户区内容（不依赖窗口是否被别的窗口遮挡）。"""
    cr = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(cr))
    w, h = cr.right, cr.bottom
    hwnd_dc = user32.GetDC(hwnd)
    mem_dc = gdi32.CreateCompatibleDC(hwnd_dc)
    bmp = gdi32.CreateCompatibleBitmap(hwnd_dc, w, h)
    gdi32.SelectObject(mem_dc, bmp)
    user32.PrintWindow(hwnd, mem_dc, PW_CLIENTONLY | PW_RENDERFULLCONTENT)

    class Header(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                    ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                    ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                    ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                    ("biClrImportant", wt.DWORD)]

    header = Header()
    header.biSize = ctypes.sizeof(Header)
    header.biWidth = w
    header.biHeight = -h
    header.biPlanes = 1
    header.biBitCount = 32
    buffer = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mem_dc, bmp, 0, h, buffer, ctypes.byref(header), 0)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem_dc)
    user32.ReleaseDC(hwnd, hwnd_dc)
    return Image.frombuffer("RGBA", (w, h), buffer, "raw", "BGRA", 0, 1).convert("RGB")


def visible_diff(a: Image.Image, b: Image.Image, threshold: int = 12) -> float | None:
    """可见差异像素占比（任一通道差 > threshold）；尺寸不同返回 None。"""
    if a.size != b.size:
        return None
    pa, pb = a.convert("RGB").load(), b.convert("RGB").load()
    w, h = a.size
    strong = total = 0
    for y in range(h):
        for x in range(w):
            total += 1
            ra, ga, ba = pa[x, y]
            rb, gb, bb = pb[x, y]
            if abs(ra - rb) > threshold or abs(ga - gb) > threshold or abs(ba - bb) > threshold:
                strong += 1
    return strong / total * 100.0


def main() -> int:
    parser = argparse.ArgumentParser(description="内置通道与桌面一致性检查（Windows）")
    parser.add_argument("--exe", default=str(ROOT / "build/dev/bin/gallery.exe"))
    parser.add_argument("--app", default="gallery")
    parser.add_argument("--title", default="霜天")
    parser.add_argument("--scale", type=float, default=0.0,
                        help="显式 scale（默认 0 = 跟随系统，验证默认口径）")
    parser.add_argument("--shots", default=str(ROOT / "build/consistency"))
    args = parser.parse_args()

    exe = Path(args.exe)
    shots = Path(args.shots)
    shots.mkdir(parents=True, exist_ok=True)
    if not exe.exists():
        print(f"exe 不存在：{exe}（先 st build {args.app}）")
        return 1

    failures: list[str] = []
    checks = 0

    def check(condition: bool, message: str) -> None:
        nonlocal checks
        checks += 1
        print(("  [ok]   " if condition else "  [FAIL] ") + message, flush=True)
        if not condition:
            failures.append(message)

    scale_args = ["--scale", str(args.scale)] if args.scale > 0 else []
    common = scale_args + ["--renderer", "software"]

    # ① 窗口：帧缓冲 vs 客户区实际像素（默认文本形态）
    print("[①] 窗口呈现无损（帧缓冲 vs 客户区实际像素）")
    process, client = launch(exe, common, shots / "win-ctl.json", shots / "win.log")
    try:
        time.sleep(1.5)
        metrics = client.ok("metrics")
        print(f"    scale={metrics['device_scale']} 物理={metrics['physical_width']}x"
              f"{metrics['physical_height']} text={metrics.get('text_renderer')} "
              f"fit={metrics.get('text_fit')}")
        fb_path = shots / "win-fb.png"
        client.ok("capture", {"encode": "file", "path": str(fb_path)})
        hwnd = find_window(args.title)
        if not hwnd:
            check(False, f"找到窗口（标题含「{args.title}」）")
        else:
            user32.SetForegroundWindow(hwnd)
            time.sleep(1.0)
            window_img = print_window(hwnd)
            window_img.save(shots / "win-client.png")
            fb = Image.open(fb_path)
            ratio = visible_diff(fb, window_img)
            if ratio is None:
                check(False, f"帧缓冲与客户区尺寸一致（{fb.size} vs {window_img.size}）")
            else:
                check(ratio == 0.0, f"窗口呈现逐像素无损（可见差异 {ratio:.3f}%）")
    finally:
        stop(process, client)

    # ② 两通道同源：无头 vs 窗口（同参数）
    print("[②] 两通道同源（同参数：无头 vs 窗口）")
    process_h, client_h = launch(exe, common + ["--headless"], shots / "headless-ctl.json",
                                 shots / "headless.log")
    try:
        time.sleep(1.0)
        headless_metrics = client_h.ok("metrics")
        print(f"    headless scale={headless_metrics['device_scale']} "
              f"text={headless_metrics.get('text_renderer')} fit={headless_metrics.get('text_fit')}")
        client_h.ok("capture", {"encode": "file", "path": str(shots / "headless.png")})
    finally:
        stop(process_h, client_h)

    process_w, client_w = launch(exe, common, shots / "window-ctl2.json", shots / "window2.log")
    try:
        time.sleep(1.5)
        window_metrics = client_w.ok("metrics")
        print(f"    window   scale={window_metrics['device_scale']} "
              f"text={window_metrics.get('text_renderer')} fit={window_metrics.get('text_fit')}")
        client_w.ok("capture", {"encode": "file", "path": str(shots / "window.png")})
    finally:
        stop(process_w, client_w)

    check(headless_metrics["device_scale"] == window_metrics["device_scale"],
          f"两通道默认 scale 一致（无头 {headless_metrics['device_scale']} vs "
          f"窗口 {window_metrics['device_scale']}）")
    check(headless_metrics.get("text_renderer") == window_metrics.get("text_renderer"),
          f"两通道文本形态一致（{headless_metrics.get('text_renderer')} vs "
          f"{window_metrics.get('text_renderer')}）")
    check(headless_metrics.get("text_fit") == window_metrics.get("text_fit"),
          f"两通道网格拟合一致（{headless_metrics.get('text_fit')} vs "
          f"{window_metrics.get('text_fit')}）")

    headless_img = Image.open(shots / "headless.png")
    window_img = Image.open(shots / "window.png")
    ratio = visible_diff(headless_img, window_img)
    if ratio is None:
        check(headless_img.size == window_img.size,
              f"两通道截图尺寸一致（{headless_img.size} vs {window_img.size}）")
    else:
        # 同参数下仍可能有少量差异：GPU 合成 vs 软件光栅的边缘容差（§8.3.2），
        # 但不应超过 2%（超了说明默认口径又分家了）。
        check(ratio < 2.0, f"两通道截图接近（可见差异 {ratio:.3f}% < 2%）")

    print(f"\n检查 {checks} 项 · 失败 {len(failures)} 项")
    for item in failures:
        print(f"  - {item}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
