#!/usr/bin/env python3
"""X11 双缓冲上屏的真窗口验证：**交替的局部上屏之后，画面必须仍然等于帧缓冲**。

为什么需要它：双缓冲的错法**不崩、不花屏、也不报错**——它只表现为"某块槽里留着旧像素"。
触发条件很窄：必须**先**把两块槽都用起来（各自吃到不同的局部区域），**再**发生一次整屏
重绘，那块"欠着旧内容"的槽才会被整屏复用。所以本脚本按这个顺序做：

  ① 启动（gallery 首帧整屏上屏 → 0 号槽拿到完整帧）；
  ② 一串**局部**改动（鼠标划过一个按钮组）：hover 只标脏那一小块，且交替落在两块槽上；
  ③ 一次**整屏**改动（切主题）—— 这时两块槽都被整屏写一遍；
  ④ 从**服务器侧**取回窗口像素，与进程内帧缓冲逐像素比对。

判决口径（与 `tools/x11_win_check.py` 同源，避免两把尺子量出两种结论）：
两次取像素不在同一瞬间，且过渡动画在推进——所以判据是"**帧缓冲前后没变的像素**，
必须与服务器像素一致"。画面静下来之后这条判据等价于逐像素相等；而不静的时候它
依然能抓住"槽里是旧帧"（那会让大片未变像素都对不上）。

断言：
  - `present_note` 报 `2/2 块`（双缓冲真的启用，而不是静默降级）；
  - 完成事件计数 > 0（服务端真的在回报它——否则双缓冲就是无保护覆写）；
  - 局部上屏真的生效过（`上帧送显` 远小于画布）；
  - 服务器像素与帧缓冲一致（没有旧槽内容漏到屏幕上）。

用法：
    python3 tools/x11_double_buffer_check.py [--display :0] [--exe PATH]

退出码 0 = 全部通过。非 Linux 直接跳过（退出 0）。
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_EXE = ROOT / "build/dev/bin/gallery"
OUT_DIR = ROOT / "build/x11-check"
sys.path.insert(0, str(ROOT / "tools"))
from st_client_lib import call_with_token, load_control  # noqa: E402

if not sys.platform.startswith("linux"):
    print("跳过：本脚本只在 Linux 上运行（需要真实 X 服务器）")
    sys.exit(0)


# ————————————————————————————————————————————————————————————————————————————
# 最小 Xlib 绑定（与 x11_win_check 同手法：ctypes 直调，不需要 python-xlib）
# ————————————————————————————————————————————————————————————————————————————

class XImage(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_int), ("height", ctypes.c_int), ("xoffset", ctypes.c_int),
        ("format", ctypes.c_int), ("data", ctypes.c_void_p), ("byte_order", ctypes.c_int),
        ("bitmap_unit", ctypes.c_int), ("bitmap_bit_order", ctypes.c_int),
        ("bitmap_pad", ctypes.c_int), ("depth", ctypes.c_int),
        ("bytes_per_line", ctypes.c_int), ("bits_per_pixel", ctypes.c_int),
        ("red_mask", ctypes.c_ulong), ("green_mask", ctypes.c_ulong), ("blue_mask", ctypes.c_ulong),
        ("obdata", ctypes.c_void_p),
    ]


def load_x11():
    name = ctypes.util.find_library("X11")
    return ctypes.CDLL(name) if name else None


def grab_pixels(x11, display, wid, w, h):
    """从服务器侧读窗口左上 w×h 的像素（ZPixmap 原始字节）。"""
    x11.XGetImage.restype = ctypes.POINTER(XImage)
    x11.XGetImage.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                              ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.c_int]
    x11.XDestroyImage.argtypes = [ctypes.POINTER(XImage)]
    image = x11.XGetImage(display, wid, 0, 0, w, h, 0xFFFFFFFF, 2)  # AllPlanes, ZPixmap
    if not image:
        raise RuntimeError("XGetImage 失败（窗口不可见？）")
    try:
        img = image.contents
        stride = img.bytes_per_line
        blob = ctypes.string_at(img.data, stride * h)
        return b"".join(blob[row * stride:row * stride + w * 4] for row in range(h))
    finally:
        x11.XDestroyImage(image)


def find_window(x11, display, pid):
    """按 `_NET_WM_PID` 找窗口（进程唯一、无编码问题）。"""
    x11.XDefaultScreen.restype = ctypes.c_int
    x11.XDefaultScreen.argtypes = [ctypes.c_void_p]
    x11.XRootWindow.restype = ctypes.c_ulong
    x11.XRootWindow.argtypes = [ctypes.c_void_p, ctypes.c_int]
    x11.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                               ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
                               ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
                               ctypes.POINTER(ctypes.c_uint)]
    x11.XInternAtom.restype = ctypes.c_ulong
    x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    x11.XGetWindowProperty.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_long, ctypes.c_long,
        ctypes.c_int, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte))]
    x11.XFree.argtypes = [ctypes.c_void_p]
    pid_atom = x11.XInternAtom(display, b"_NET_WM_PID", 0)
    root = x11.XRootWindow(display, x11.XDefaultScreen(display))

    def pid_of(wid):
        actual_type = ctypes.c_ulong()
        actual_format = ctypes.c_int()
        nitems = ctypes.c_ulong()
        after = ctypes.c_ulong()
        data = ctypes.POINTER(ctypes.c_ubyte)()
        if x11.XGetWindowProperty(display, wid, pid_atom, 0, 1, 0, 0, ctypes.byref(actual_type),
                                  ctypes.byref(actual_format), ctypes.byref(nitems),
                                  ctypes.byref(after), ctypes.byref(data)) != 0 or not data:
            return None
        try:
            if actual_format.value != 32 or nitems.value < 1:
                return None
            return ctypes.cast(data, ctypes.POINTER(ctypes.c_ulong))[0]
        finally:
            x11.XFree(data)

    def scan(node):
        r = ctypes.c_ulong()
        parent = ctypes.c_ulong()
        children = ctypes.POINTER(ctypes.c_ulong)()
        count = ctypes.c_uint()
        if not x11.XQueryTree(display, node, ctypes.byref(r), ctypes.byref(parent),
                              ctypes.byref(children), ctypes.byref(count)):
            return 0
        ids = [children[i] for i in range(count.value)] if count.value else []
        if children:
            x11.XFree(children)
        for wid in ids:
            if pid_of(wid) == pid:
                return wid
            hit = scan(wid)
            if hit:
                return hit
        return 0

    return scan(root)


def png_pixels(path):
    """解霜天自产 PNG（非隔行 8 位真彩）为紧凑 RGBA 行（手写解析，不依赖 Pillow）。"""
    blob = pathlib.Path(path).read_bytes()
    pos = 8
    idat = bytearray()
    width = height = 0
    while pos < len(blob):
        (length,) = struct.unpack(">I", blob[pos:pos + 4])
        kind = blob[pos + 4:pos + 8]
        body = blob[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        pos += 12 + length
    raw = zlib.decompress(bytes(idat))
    stride = width * 4
    out = bytearray()
    prev = bytearray(stride)
    cursor = 0
    for _row in range(height):
        filter_type = raw[cursor]
        cursor += 1
        line = bytearray(raw[cursor:cursor + stride])
        cursor += stride
        if filter_type == 1:
            for i in range(4, stride):
                line[i] = (line[i] + line[i - 4]) & 0xFF
        elif filter_type == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif filter_type == 3:
            for i in range(stride):
                left = line[i - 4] if i >= 4 else 0
                line[i] = (line[i] + (left + prev[i]) // 2) & 0xFF
        elif filter_type == 4:
            for i in range(stride):
                left = line[i - 4] if i >= 4 else 0
                up = prev[i]
                upleft = prev[i - 4] if i >= 4 else 0
                p = left + up - upleft
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - upleft)
                pred = left if (pa <= pb and pa <= pc) else (up if pb <= pc else upleft)
                line[i] = (line[i] + pred) & 0xFF
        out += line
        prev = line
    return bytes(out), width, height


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=str(DEFAULT_EXE))
    parser.add_argument("--display", default=os.environ.get("DISPLAY", ":0"))
    args = parser.parse_args()

    exe = pathlib.Path(args.exe)
    if not exe.exists():
        print(f"找不到示例可执行文件：{exe}（先 st build gallery）")
        return 2
    x11 = load_x11()
    if x11 is None:
        print("跳过：libX11 不可加载")
        return 0
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    failures = []
    checks = []
    env = dict(os.environ)
    env["DISPLAY"] = args.display
    control_file = OUT_DIR / "dbuf-control.json"
    if control_file.exists():
        control_file.unlink()
    log_path = OUT_DIR / "gallery-x11-dbuf.log"
    with open(log_path, "w", encoding="utf-8") as log:
        proc = subprocess.Popen(
            [str(exe), "--backend", "x11", "--scale", "1", "--width", "900", "--height", "640",
             "--control-file", str(control_file)],
            stdout=log, stderr=subprocess.STDOUT, env=env, cwd=str(ROOT))
    try:
        info = {}
        for _ in range(100):
            info = load_control(control_file)
            if info.get("port"):
                break
            if proc.poll() is not None:
                break
            time.sleep(0.1)
        if not info.get("port"):
            print("失败：控制通道未就绪")
            print(log_path.read_text(errors="replace")[-3000:])
            return 1
        token = info.get("token", "")

        def call(method, params=None, timeout=60):
            return call_with_token(info["port"], method, params or {}, token, timeout=timeout)

        display = x11.XOpenDisplay(args.display.encode())
        if not display:
            print(f"XOpenDisplay({args.display}) 失败")
            return 1
        try:
            wid = 0
            for _ in range(60):
                wid = find_window(x11, display, proc.pid)
                if wid:
                    break
                time.sleep(0.2)
            if not wid:
                print("失败：找不到窗口")
                return 1
            checks.append(f"窗口 0x{wid:x}")

            metrics = call("metrics", {}).get("result", {})
            note = metrics.get("present_note", "")
            checks.append(f"present_note = {note}")
            if "2/2 块" not in note:
                failures.append(f"双缓冲未启用（present_note={note!r}）—— 期望 '2/2 块'")
            m = re.search(r"完成事件 (\d+) 个", note)
            completions = int(m.group(1)) if m else 0
            if completions <= 0:
                failures.append(f"完成事件计数 {completions}——服务端没回报 XShmCompletionEvent，"
                                "双缓冲等于无保护覆写")
            else:
                checks.append(f"完成事件已回报（{completions} 个）")

            width = int(metrics.get("physical_width") or 0)
            height = int(metrics.get("physical_height") or 0)
            if width <= 0 or height <= 0:
                failures.append("metrics 未报物理尺寸")
                return 1

            # —— ② 一串**局部**改动：hover 划过真实控件的中心，让两块槽各自吃到
            #     不同的局部区域（坐标从 `find` 拿真 bounds——硬编坐标会落在空白处，
            #     而“没脏区就不重绘”会让送显面积仍是上一次的整屏，看起来像没生效）
            call("wait", {"for": "stable", "stable_ms": 250, "timeout_ms": 3000}, timeout=10)
            found = call("find", {"selector": "Button", "limit": 12}, timeout=20).get("result") or {}
            targets = [m["bounds"] for m in (found.get("matches") or []) if m.get("visible")]
            partials_seen = 0
            tried = 0
            for bounds in targets[:8]:
                cx = bounds["x"] + bounds["width"] / 2.0
                cy = bounds["y"] + bounds["height"] / 2.0
                call("input.mouse", {"kind": "move", "x": cx, "y": cy}, timeout=10)
                time.sleep(0.08)
                note_now = call("metrics", {}).get("result", {}).get("present_note", "")
                area = re.search(r"上帧送显 (\d+)x(\d+)", note_now)
                tried += 1
                if area:
                    w_partial, h_partial = int(area.group(1)), int(area.group(2))
                    if w_partial * h_partial < width * height // 2:
                        partials_seen += 1
            if tried == 0:
                failures.append("找不到可 hover 的按钮（夹具失效，不能据此下结论）")
            elif partials_seen < 2:
                failures.append(f"局部上屏没生效（{partials_seen}/{tried} 次 hover 的送显面积小于半屏）")
            else:
                checks.append(f"局部上屏生效（{partials_seen}/{tried} 次 hover 上报了局部面积）")

            # —— ③ 一次**整屏**改动：切主题（两块槽都会被整屏写） ——
            call("invoke", {"id": "theme-toggle", "action": "click"}, timeout=20)
            call("wait", {"for": "stable", "stable_ms": 300, "timeout_ms": 3000}, timeout=10)
            note_after = call("metrics", {}).get("result", {}).get("present_note", "")
            m2 = re.search(r"完成事件 (\d+) 个", note_after)
            if m2 and int(m2.group(1)) <= completions:
                failures.append("切主题后完成事件计数没有增长（上屏状态机可能停转了）")
            else:
                checks.append("整屏改动后完成事件继续增长（状态机在转）")

            # —— ④ 服务器像素 vs 帧缓冲：抓"旧槽内容是否漏到屏幕上" ——
            read_w = min(220, width)
            read_h = min(160, height)
            shot1 = OUT_DIR / "dbuf-frame-1.png"
            shot2 = OUT_DIR / "dbuf-frame-2.png"
            for stale in (shot1, shot2):
                if stale.exists():
                    stale.unlink()

            def capture(path):
                reply = call("capture", {"encode": "file", "path": str(path)}, timeout=30)
                written = (reply.get("result") or {}).get("path") if isinstance(reply, dict) else None
                return pathlib.Path(written) if written else path

            shot1 = capture(shot1)
            server = grab_pixels(x11, display, wid, read_w, read_h)
            shot2 = capture(shot2)
            first, full_w, _ = png_pixels(shot1)
            second, _, _ = png_pixels(shot2)
            bad = 0
            checked = 0
            for y in range(read_h):
                for x in range(read_w):
                    index = (y * full_w + x) * 4
                    frame_a = first[index:index + 4]
                    frame_b = second[index:index + 4]
                    if frame_a != frame_b:
                        continue  # 这段时间画面变了：比对无意义（动画在推进）
                    checked += 1
                    # 画布是 R,G,B,A；X11 ZPixmap 小端是 B,G,R,X（0xFFRRGGBB）
                    expected = bytes((frame_a[2], frame_a[1], frame_a[0]))
                    got = server[(y * read_w + x) * 4:(y * read_w + x) * 4 + 3]
                    if expected != got:
                        bad += 1
            checks.append(f"服务器像素 vs 帧缓冲：比对 {checked} 像素，不符 {bad}")
            if checked < 1000:
                failures.append(f"可比对像素太少（{checked}）—— 判据失效，不能据此下结论")
            elif bad > 0:
                failures.append(f"{bad}/{checked} 个未变像素与服务器侧不符"
                                "（典型成因：某块槽里留着旧内容）")
            else:
                checks.append("无旧槽内容漏到屏幕（交替局部上屏 + 整屏重绘后逐像素一致）")
        finally:
            x11.XCloseDisplay(display)
    finally:
        try:
            call_with_token(load_control(control_file).get("port", 0), "shutdown", {},
                            load_control(control_file).get("token", ""), timeout=5)
        except Exception:  # noqa: BLE001
            pass
        if proc.poll() is None:
            proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

    print("—— X11 双缓冲上屏验证 ——")
    for line in checks:
        print(f"  ✓ {line}")
    for line in failures:
        print(f"  ✗ {line}")
    print(f"\n  {len(checks)} 项通过，{len(failures)} 项失败")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
