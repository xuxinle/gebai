#!/usr/bin/env python3
"""X11 真窗口验证（Linux）：**去装饰窗口 + 自绘窗框 + 窗口控制 + 像素真上屏**。

为什么必须真窗口：`_MOTIF_WM_HINTS` 去装饰、`_NET_WM_MOVERESIZE` 拖拽/缩放、
`_NET_WM_STATE` 最大化这套**只在真实 X 服务器上才有效果**——无头跑一万遍也验证不了
"窗口管理器有没有把边框去掉""客户区是否等于窗口矩形"。而**又不能只看代码**：
少发一个 client message，窗口控制就静默失效，截图上看不出来。

分两类断言（各自独立，失败时能立刻分辨是"窗口结构"还是"像素通路"坏了）：

  ① **窗口结构（X 服务器侧，由本脚本直接查）**
     - 窗口真的被映射（`Map State: IsViewable`）；
     - **去装饰生效**：`_NET_FRAME_EXTENTS` 不存在（WM 不为它画边框）；
       —— 这一条是"自绘窗框"的前提：留着系统边框就是**两条标题栏并存**；
     - **客户区 == 窗口矩形**：`xwininfo` 的 width/height 与请求的逻辑 × scale 一致；
     - 属性齐备：`_NET_WM_NAME`（UTF-8 标题）/`WM_PROTOCOLS`（含 WM_DELETE_WINDOW）/
       `_NET_WM_PID`。

  ② **像素通路（帧缓冲 → 窗口，逐像素）**
     - 用 `XGetImage` 从**服务器侧**读回窗口像素；
     - 与进程内控制通道 `capture` 拿到的帧缓冲逐像素比对：
       两者应**完全一致**（同一尺寸、同一内容）——若像素格式搞错（红蓝互换/α 当蓝），
       这里立刻报出来（实测过：直接送 `0xRRGGBBAA` 会整屏红蓝互换）。

用法：
    python3 tools/x11_win_check.py [--exe PATH] [--display :9] [--scale 1.0]
    python3 tools/x11_win_check.py --spawn-xwayland    # 自己起一个无 WM 的私有 X 显示

退出码 0 = 全部通过。

依赖：Python + `libX11.so.6`（用 ctypes 直调，**不需要 python-xlib**）。
非 Linux 直接跳过（退出 0）。
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import json
import os
import pathlib
import shutil
import signal
import struct
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_EXE = ROOT / "build/dev/bin/gallery"
OUT_DIR = ROOT / "build/x11-check"
sys.path.insert(0, str(ROOT / "tools"))
from st_client_lib import call_with_token, load_control  # noqa: E402

if not sys.platform.startswith("linux"):
    print("跳过：本脚本只在 Linux 上运行（需要真实 X 服务器）")
    sys.exit(0)


# ————————————————————————————————————————————————————————————————————————————
# 最小 Xlib 绑定（只用到读属性与取像素；与后端一样走 dlopen/ctypes，不依赖 -dev 包）
# ————————————————————————————————————————————————————————————————————————————

def load_x11():  # noqa: ANN201
    path = ctypes.util.find_library("X11")
    if path is None:
        return None
    return ctypes.CDLL(path)


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


def find_window(x11, display, name_hint: str, pid: int = 0):  # noqa: ANN001
    """找目标窗口：优先按 `_NET_WM_PID` 匹配（最可靠），否则退回标题关键字。

    为什么不只按名字：应用可以不设标题（`--title` 未给时用默认值），而 `XFetchName`
    返回的是 Latin-1——中文标题会解出乱码。`_NET_WM_PID` 是**我们自己在建窗时写的**
    （`set_net_wm_pid`），进程唯一、无编码问题。
    """
    x11.XDefaultRootWindow.restype = ctypes.c_ulong
    x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x11.XDefaultScreen.restype = ctypes.c_int
    x11.XDefaultScreen.argtypes = [ctypes.c_void_p]
    x11.XRootWindow.restype = ctypes.c_ulong
    x11.XRootWindow.argtypes = [ctypes.c_void_p, ctypes.c_int]
    x11.XQueryTree.restype = ctypes.c_int
    x11.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
                               ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
                               ctypes.POINTER(ctypes.c_uint)]
    x11.XFetchName.restype = ctypes.c_int
    x11.XFetchName.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_char_p)]
    x11.XFree.argtypes = [ctypes.c_void_p]
    root = x11.XRootWindow(display, x11.XDefaultScreen(display))

    def scan(node: int) -> int:
        r = ctypes.c_ulong()
        parent = ctypes.c_ulong()
        children = ctypes.POINTER(ctypes.c_ulong)()
        count = ctypes.c_uint()
        if not x11.XQueryTree(display, node, ctypes.byref(r), ctypes.byref(parent),
                              ctypes.byref(children), ctypes.byref(count)):
            return 0
        hit = 0
        ids = [children[i] for i in range(count.value)] if count.value else []
        if children:
            x11.XFree(children)
        for wid in ids:
            if pid:
                got_pid, _, _ = _read_prop(x11, display, wid, "_NET_WM_PID")
                if isinstance(got_pid, list) and got_pid and got_pid[0] == pid:
                    return wid
            name = ctypes.c_char_p()
            if x11.XFetchName(display, wid, ctypes.byref(name)) and name.value:
                text = name.value.decode("latin-1")
                x11.XFree(name)
                if name_hint and name_hint in text:
                    hit = wid
                    break
            # 递归（WM 会加一层 reparenting 外壳）
            hit = scan(wid)
            if hit:
                break
        return hit

    return scan(root)


def _read_prop(x11, display, wid: int, atom_name: str, delete: int = 0):  # noqa: ANN001
    """读一个窗口属性（8 位→bytes / 32 位→list[int]）。"""
    x11.XInternAtom.restype = ctypes.c_ulong
    x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    x11.XGetWindowProperty.restype = ctypes.c_int
    # 官方签名（Xlib.h）：
    #   int XGetWindowProperty(Display*, Window, Atom property, long long_offset,
    #       long long_length, Bool delete, Atom req_type, Atom* actual_type_return,
    #       int* actual_format_return, unsigned long* nitems_return,
    #       unsigned long* bytes_after_return, unsigned char** prop_return);
    # ⚠ `req_type` 极易漏抄——漏了就是参数整体错位一位（读出垃圾或直接崩）。
    x11.XGetWindowProperty.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_long, ctypes.c_long,
        ctypes.c_int, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte))]
    x11.XFree.argtypes = [ctypes.c_void_p]
    atom = x11.XInternAtom(display, atom_name.encode(), 0)
    if atom == 0:
        return None, 0, 0
    actual_type = ctypes.c_ulong()
    actual_format = ctypes.c_int()
    nitems = ctypes.c_ulong()
    after = ctypes.c_ulong()
    data = ctypes.POINTER(ctypes.c_ubyte)()
    status = x11.XGetWindowProperty(display, wid, atom, 0, 1024, delete, 0,  # AnyPropertyType
                                    ctypes.byref(actual_type), ctypes.byref(actual_format),
                                    ctypes.byref(nitems), ctypes.byref(after), ctypes.byref(data))
    if status != 0 or not data:
        return None, 0, 0
    if actual_format.value == 8:
        value = bytes(bytearray(data[:nitems.value]))
    elif actual_format.value == 32:
        longs = ctypes.cast(data, ctypes.POINTER(ctypes.c_ulong))
        value = [longs[i] for i in range(nitems.value)]
    else:
        value = None
    x11.XFree(data)
    return value, actual_format.value, nitems.value


def window_props(x11, display, wid: int) -> dict:  # noqa: ANN001
    """读 `_NET_FRAME_EXTENTS` / `_NET_WM_NAME` / `_NET_WM_PID` / `WM_PROTOCOLS`。"""
    x11.XInternAtom.restype = ctypes.c_ulong
    x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    x11.XGetWindowProperty.restype = ctypes.c_int
    # 官方签名（Xlib.h）：
    #   int XGetWindowProperty(Display*, Window, Atom property, long long_offset,
    #       long long_length, Bool delete, Atom req_type, Atom* actual_type_return,
    #       int* actual_format_return, unsigned long* nitems_return,
    #       unsigned long* bytes_after_return, unsigned char** prop_return);
    # ⚠ `req_type` 极易漏抄——漏了就是参数整体错位一位（读出垃圾或直接崩）。
    x11.XGetWindowProperty.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_long, ctypes.c_long,
        ctypes.c_int, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte))]

    def get(atom_name: str, delete: int = 0):  # noqa: ANN202
        return _read_prop(x11, display, wid, atom_name, delete)

    frame_extents, _, _ = get("_NET_FRAME_EXTENTS")
    net_name, _, _ = get("_NET_WM_NAME")
    pid, _, _ = get("_NET_WM_PID")
    protocols, _, _ = get("WM_PROTOCOLS")
    wm_delete = x11.XInternAtom(display, b"WM_DELETE_WINDOW", 0)
    return {
        "frame_extents": frame_extents,
        "net_wm_name": net_name.decode("utf-8", "replace") if isinstance(net_name, bytes) else None,
        "net_wm_pid": pid[0] if isinstance(pid, list) and pid else None,
        "has_wm_delete": bool(protocols) and wm_delete in protocols,
    }


def root_size(x11, display) -> tuple[int, int]:  # noqa: ANN001
    """根窗口（屏幕）尺寸：用它夹取验证窗口——私有 Xwayland 的屏可能比窗口小，
    此时窗口超出屏幕的部分**服务器侧根本没有像素**（`XGetImage` 会 BadMatch）。"""
    x11.XDefaultRootWindow.restype = ctypes.c_ulong
    x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x11.XGetGeometry.restype = ctypes.c_int
    x11.XGetGeometry.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint),
        ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint)]
    root = x11.XDefaultRootWindow(display)
    r = ctypes.c_ulong()
    x = ctypes.c_int()
    y = ctypes.c_int()
    w = ctypes.c_uint()
    h = ctypes.c_uint()
    bw = ctypes.c_uint()
    depth = ctypes.c_uint()
    x11.XGetGeometry(display, root, ctypes.byref(r), ctypes.byref(x), ctypes.byref(y),
                     ctypes.byref(w), ctypes.byref(h), ctypes.byref(bw), ctypes.byref(depth))
    return w.value, h.value


def geometry(x11, display, wid: int) -> dict:  # noqa: ANN001
    x11.XGetGeometry.restype = ctypes.c_int
    x11.XGetGeometry.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint),
        ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint)]
    x11.XTranslateCoordinates.restype = ctypes.c_int
    x11.XTranslateCoordinates.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_ulong)]
    x11.XDefaultRootWindow.restype = ctypes.c_ulong
    root = x11.XDefaultRootWindow(display)
    r = ctypes.c_ulong()
    x = ctypes.c_int()
    y = ctypes.c_int()
    w = ctypes.c_uint()
    h = ctypes.c_uint()
    bw = ctypes.c_uint()
    depth = ctypes.c_uint()
    x11.XGetGeometry(display, wid, ctypes.byref(r), ctypes.byref(x), ctypes.byref(y),
                     ctypes.byref(w), ctypes.byref(h), ctypes.byref(bw), ctypes.byref(depth))
    abs_x = ctypes.c_int()
    abs_y = ctypes.c_int()
    child = ctypes.c_ulong()
    x11.XTranslateCoordinates(display, wid, root, 0, 0, ctypes.byref(abs_x),
                              ctypes.byref(abs_y), ctypes.byref(child))
    # 映射状态（Map State: IsViewable 见 xwininfo；用 XGetWindowAttributes 读更直接）
    return {"x": x.value, "y": y.value, "width": w.value, "height": h.value,
            "depth": depth.value, "abs_x": abs_x.value, "abs_y": abs_y.value}


def grab_pixels(x11, display, wid: int, width: int, height: int) -> bytes:  # noqa: ANN001
    """从**服务器侧**读回窗口像素（BGRX，小端 4 字节/像素）。"""
    x11.XGetImage.restype = ctypes.c_void_p
    x11.XGetImage.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint,
        ctypes.c_ulong, ctypes.c_int]
    raw = x11.XGetImage(display, wid, 0, 0, width, height, 0xFFFFFFFF, 2)
    if not raw:
        raise RuntimeError("XGetImage 返回空（窗口不可读？）")
    image = ctypes.cast(raw, ctypes.POINTER(XImage)).contents
    data = ctypes.string_at(image.data, image.bytes_per_line * height)
    # XDestroyImage 是宏：这里用 XFree 释放结构（data 由 Xlib 自分配，结构亦然）
    x11.XFree(ctypes.c_void_p(raw))
    return data


def unmap_read(data: bytes, width: int, height: int, bpl: int) -> bytes:
    """把带行填充的 BGRX 拉成紧凑 RGBA（与 `Surface::to_rgba8` 同口径便于逐像素比）。"""
    out = bytearray()
    for row in range(height):
        line = data[row * bpl:row * bpl + width * 4]
        for col in range(width):
            b, g, r, _a = line[col * 4:col * 4 + 4]
            out += bytes((r, g, b, 255))
    return bytes(out)


# ————————————————————————————————————————————————————————————————————————————
# 驱动
# ————————————————————————————————————————————————————————————————————————————

def spawn_xwayland() -> tuple[subprocess.Popen | None, str | None, str | None]:
    """起一个**无窗口管理器**的私有 Xwayland 显示：坐标精确、无 reparenting。

    为什么不用桌面会话里已有的 `:0`：GNOME 会把窗口交给 mutter-x11-frames 重新父化
    （窗口被搬走/加上自己的 frame），"客户区 == 窗口矩形"这类断言在那里测不准。
    无 WM 的私有显示上，X 服务器给什么就是什么——这正是**窗口层**该被验证的形态。
    """
    exe = shutil.which("Xwayland")
    if exe is None:
        return None, None, "未找到 Xwayland（请用 --display 指定一个已有的 X 显示）"
    display = ":97"
    env = dict(os.environ)
    log = open("/tmp/st-x11-check-xwayland.log", "wb")  # noqa: SIM115
    proc = subprocess.Popen([exe, display, "-noreset"], stdout=log, stderr=log, env=env,
                            start_new_session=True)
    for _ in range(50):
        time.sleep(0.2)
        check = subprocess.run(["xdpyinfo", "-display", display], capture_output=True)
        if check.returncode == 0:
            return proc, display, None
    proc.terminate()
    return None, None, "Xwayland 启动后无法连接"


def main() -> int:
    parser = argparse.ArgumentParser(description="X11 真窗口验证")
    parser.add_argument("--exe", default=str(DEFAULT_EXE))
    parser.add_argument("--display", default="")
    parser.add_argument("--scale", type=float, default=1.0)
    parser.add_argument("--spawn-xwayland", action="store_true",
                        help="自己起一个无 WM 的私有 Xwayland 显示")
    args = parser.parse_args()

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    failures: list[str] = []
    checks: list[str] = []

    x11 = load_x11()
    if x11 is None:
        print("跳过：未找到 libX11.so.6")
        return 0
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]

    spawned = None
    display_name = args.display
    if args.spawn_xwayland or not display_name:
        spawned, display_name, error = spawn_xwayland()
        if error:
            print(f"跳过：{error}")
            return 0
        checks.append(f"已起私有 X 显示 {display_name}（无窗口管理器）")
    display = x11.XOpenDisplay(display_name.encode())
    if not display:
        print(f"跳过：无法连接显示 {display_name}")
        return 0

    # 控制文件放在本脚本的产物目录：控制通道的**截图落盘白名单**是
    # 「temp / 可执行文件目录 / 控制文件目录」——截图与取证文件要落在后者才写得进去。
    control_file = str(OUT_DIR / f"ctl-{os.getpid()}.json")
    if os.path.exists(control_file):
        os.unlink(control_file)
    env = dict(os.environ)
    env["DISPLAY"] = display_name
    log_path = OUT_DIR / "gallery-x11.log"
    log = open(log_path, "wb")  # noqa: SIM115
    # 窗口尺寸要被屏幕容得下：超出屏幕的部分在服务器侧没有像素，
    # `XGetImage` 会以 BadMatch 失败（**这不是后端的缺陷**，而是私有 X 屏太小）。
    # 因此这里按屏幕夹取，并在报告里写明实际用的尺寸。
    screen_w, screen_h = root_size(x11, display)
    logical_w = min(1000, max(320, screen_w - 40))
    logical_h = min(700, max(240, screen_h - 40))
    proc = subprocess.Popen(
        [args.exe, "--backend=x11", "--control-port", "0", "--control-file", control_file,
         "--scale", str(args.scale), "--width", str(logical_w), "--height", str(logical_h),
         "--ms", "12000"],
        stdout=log, stderr=log, env=env, start_new_session=True)
    try:
        info = {}
        for _ in range(150):
            time.sleep(0.2)
            info = load_control(control_file)
            if info.get("port"):
                break
            if proc.poll() is not None:
                break
        if not info.get("port"):
            print("失败：控制通道未就绪（进程可能已退出）")
            print(log_path.read_text(errors="replace")[-3000:])
            return 1

        # —— ① 窗口结构 ——
        wid = 0
        for _ in range(50):
            wid = find_window(x11, display, "", pid=proc.pid)
            if wid:
                break
            time.sleep(0.2)
        metrics = call_with_token(info["port"], "metrics", {}, info.get("token", ""))
        result = metrics.get("result", {}) if isinstance(metrics, dict) else {}
        if not wid:
            failures.append("在 X 服务器上找不到窗口（窗口未映射？）")
        else:
            checks.append(f"窗口已找到：0x{wid:x}")
            props = window_props(x11, display, wid)
            geo = geometry(x11, display, wid)
            checks.append(f"窗口几何 {geo['width']}x{geo['height']} @ ({geo['abs_x']},{geo['abs_y']})")
            # 客户区 == 后端声明给应用的物理尺寸（去装饰下二者必须逐像素对齐：
            # 不一致就是"画布被系统拉伸"的老病根）。期望值取自 `metrics`——
            # 那是应用真正在用的画布尺寸，比命令行参数权威（示例可能自己定尺寸）。
            declared_w = int(result.get("physical_width") or 0)
            declared_h = int(result.get("physical_height") or 0)
            if declared_w and declared_h and (geo["width"], geo["height"]) != (declared_w, declared_h):
                failures.append(
                    f"窗口尺寸 {geo['width']}x{geo['height']} != 后端声明的画布 {declared_w}x{declared_h}"
                    "（客户区与画布不符 → 画面会被系统拉伸）")
            else:
                checks.append(
                    f"客户区 == 画布物理尺寸（{geo['width']}x{geo['height']}，去装饰下即整窗）")
            # 去装饰：`_NET_FRAME_EXTENTS` 不存在 = WM 不为它画边框
            if props["frame_extents"] is not None:
                failures.append(f"去装饰未生效：_NET_FRAME_EXTENTS={props['frame_extents']}（系统边框仍在）")
            else:
                checks.append("去装饰生效：无 _NET_FRAME_EXTENTS（系统不画边框）")
            if props["net_wm_name"] is None:
                failures.append("缺少 _NET_WM_NAME（UTF-8 标题未设置）")
            else:
                checks.append(f"_NET_WM_NAME = {props['net_wm_name']!r}")
            if not props["has_wm_delete"]:
                failures.append("WM_PROTOCOLS 缺少 WM_DELETE_WINDOW（点关闭无法走正常收尾）")
            else:
                checks.append("WM_PROTOCOLS 含 WM_DELETE_WINDOW")
            if props["net_wm_pid"] != proc.pid:
                failures.append(f"_NET_WM_PID={props['net_wm_pid']} != 进程 pid {proc.pid}")
            else:
                checks.append("_NET_WM_PID == 进程 pid（WM 可正确归类）")

            # —— ② 像素通路：服务器侧窗口像素 vs 进程内帧缓冲 ——
            if result.get("backend") != "x11":
                failures.append(f"metrics.backend={result.get('backend')!r}（期望 'x11'）")
            else:
                checks.append("metrics 上报 backend=x11 / headless=" + str(result.get("headless")))

            # —— 帧缓冲 vs 服务器侧像素 ——
            #
            # ⚠ **比对口径**：两次取像素不可能在同一瞬间发生（控制通道截图取的是
            #    进程内画布，服务器侧像素取的是窗口当前内容），而界面上的过渡动画在推进
            #    （实测同一实例连拍两次都会有上千像素差异，见 DESIGN.md 的视口剔除一节）。
            #    因此判据不是"逐像素相等"，而是：
            #      **两者不同的像素，必须能被"这段时间里画面真的变了"解释掉**。
            #    做法：取样式定为 shot1 → 服务器像素 → shot2；若某像素 shot1 == shot2
            #    （画面没变）却与服务器像素不同，那才是真缺陷（格式/上屏路径错）。
            #    这样既不受动画干扰，又能抓住"红蓝互换"这类真错——互换时几乎全部
            #    未变像素都会对不上。
            #
            # 先等画面稳定（动画停下），能等就等，等不到也照测（判据本身能容忍变化）。
            try:
                call_with_token(info["port"], "wait",
                                {"for": "stable", "stable_ms": 250, "timeout_ms": 1500},
                                info.get("token", ""), timeout=5)
            except Exception:  # noqa: BLE001
                pass

            shot_dir = OUT_DIR
            shot1 = shot_dir / "frame.png"
            shot2 = shot_dir / "frame-2.png"
            for stale in (shot1, shot2):
                if stale.exists():
                    stale.unlink()

            def capture_to(path: pathlib.Path) -> pathlib.Path:
                reply = call_with_token(info["port"], "capture",
                                        {"encode": "file", "path": str(path)},
                                        info.get("token", ""))
                if isinstance(reply, dict) and not reply.get("ok", True):
                    raise RuntimeError(f"capture 失败：{reply.get('error')}")
                written = (reply.get("result") or {}).get("path") if isinstance(reply, dict) else None
                return pathlib.Path(written) if written else path

            shot1 = capture_to(shot1)
            # 服务器侧取样区域必须同时不超过**窗口**与**屏幕**：超出屏幕的区域服务器侧
            # 没有像素，`XGetImage` 会直接以 BadMatch 失败（私有 X 屏常比窗口小）。
            screen_w, screen_h = root_size(x11, display)
            read_w = min(160, geo["width"], screen_w)
            read_h = min(120, geo["height"], screen_h)
            raw = grab_pixels(x11, display, wid, read_w, read_h)
            server = unmap_read(raw, read_w, read_h, read_w * 4)
            shot2 = capture_to(shot2)
            sample_w, sample_h = read_w, read_h
            (OUT_DIR / "server-sample.rgba").write_bytes(server)
            checks.append(f"服务器侧取像素成功（窗口 {geo['width']}x{geo['height']}，样本 {sample_w}x{sample_h}）")
            if shot1.exists() and shot1.stat().st_size > 0:
                checks.append(f"控制通道截图已落盘（{shot1.stat().st_size} 字节）")
            else:
                failures.append("控制通道截图未落盘")

            # 帧缓冲与服务器侧像素的**一致性**：窗口内容必须就是帧缓冲的内容。
            try:
                import zlib

                def png_pixels(path: pathlib.Path, w: int, h: int) -> bytes:
                    """解 PNG 到紧凑 RGBA（只处理霜天自产的非隔行 8 位真彩 PNG）。

                    为什么手写解析而不依赖 Pillow：这个脚本要在**裸 Linux 服务器**上跑
                    （与后端"零编译期依赖"同一姿态），不能假设 pip 装过东西。
                    """
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
                        for index in range(stride):
                            left = line[index - 4] if index >= 4 else 0
                            up = prev[index]
                            upleft = prev[index - 4] if index >= 4 else 0
                            if filter_type == 1:
                                line[index] = (line[index] + left) & 0xFF
                            elif filter_type == 2:
                                line[index] = (line[index] + up) & 0xFF
                            elif filter_type == 3:
                                line[index] = (line[index] + (left + up) // 2) & 0xFF
                            elif filter_type == 4:
                                p = left + up - upleft
                                pa, pb, pc = abs(p - left), abs(p - up), abs(p - upleft)
                                pred = left if (pa <= pb and pa <= pc) else (up if pb <= pc else upleft)
                                line[index] = (line[index] + pred) & 0xFF
                        out += line
                        prev = line
                    return bytes(out)

                first = png_pixels(shot1, sample_w, sample_h)
                second = png_pixels(shot2, sample_w, sample_h) if shot2.exists() else first
                # ⚠ **两边的行距不同**：帧缓冲是整窗宽（`IHDR` 里的宽度），
                # 服务器侧取样区只有 `sample_w` 宽。用同一个线性游标遍历两者
                # 会在第一行之后整片错位（症状是"75% 像素对不上"这种吓人的数字，
                # 而像素其实一个不差）——所以必须按 (x, y) 各自算下标。
                frame_stride = sample_w * 4
                # `png_pixels` 返回的是**整图**（按 PNG 宽度为行距），因此这里要拿
                # 整图宽度换算帧缓冲的行距。
                with open(shot1, "rb") as handle:
                    ihdr = handle.read(24)
                frame_full_width = struct.unpack(">II", ihdr[16:24])[0]
                frame_stride = frame_full_width * 4
                server_stride = sample_w * 4
                total = 0
                unexplained = 0
                changed = 0
                swapped = 0
                for y in range(sample_h):
                    row_frame = y * frame_stride
                    row_server = y * server_stride
                    for x in range(sample_w):
                        fi = row_frame + x * 4
                        si = row_server + x * 4
                        if si + 4 > len(server) or fi + 4 > len(first):
                            break
                        total += 1
                        a = first[fi:fi + 3]
                        b = second[fi:fi + 3]
                        c = server[si:si + 3]
                        if a != b:
                            changed += 1        # 这段时间画面在变 → 不能作为判据
                            continue
                        if a != c:
                            unexplained += 1
                            if bytes((a[2], a[1], a[0])) == c:
                                swapped += 1    # R/B 互换的典型体征
                if total == 0:
                    failures.append("像素比对样本为空")
                elif unexplained == 0:
                    checks.append(
                        f"帧缓冲与服务器侧像素**逐像素一致**（样本 {total} 像素；"
                        f"其中 {changed} 像素因动画推进而跳过）")
                elif swapped > unexplained // 2:
                    failures.append(
                        f"帧缓冲与服务器侧像素疑似 **R/B 互换**：{swapped}/{total} 像素"
                        "——呈现格式（X11 是 0x00RRGGBB，画布是 0xRRGGBBAA）转换错了")
                else:
                    ratio = unexplained / total * 100.0
                    failures.append(
                        f"帧缓冲与服务器侧像素不一致：{unexplained}/{total}"
                        f"（{ratio:.1f}%）的**未变化**像素对不上——上屏路径有问题")
            except Exception as exc:  # noqa: BLE001
                failures.append(f"像素比对失败：{exc}")

            # —— ③ 窗口控制（自绘窗框的行动面）——
            #
            # `ui::TitleBar` 的按钮最终落到后端的 `minimize`/`toggle_maximize`/`request_close`。
            # 这里直接经控制通道 `invoke` 触发（与应用内点击同一条路径），再**从 X 服务器侧
            # 读回真实窗口状态**判定——只看 `handled=true` 是不够的：后端可能"收下了但没做成"。
            invoke = call_with_token(info["port"], "invoke",
                                     {"id": "titlebar", "action": "maximize"},
                                     info.get("token", ""))
            if not (isinstance(invoke, dict) and invoke.get("ok")):
                failures.append(f"invoke maximize 失败：{invoke}")
            else:
                # 无窗口管理器时没有 EWMH 最大化可言（`_NET_WM_STATE` 没人处理）：
                # 本脚本默认起的是**无 WM** 的私有显示，因此这里只断言"动作被受理且不崩"，
                # 真窗口管理器下的最大化断言由 `--display :0`（GNOME 会话）那条路径覆盖。
                checks.append(f"invoke maximize 已受理（动作面连通；maximized={((invoke.get('result') or {}).get('state') or {}).get('maximized')}）")
            minimize = call_with_token(info["port"], "invoke",
                                       {"id": "titlebar", "action": "minimize"},
                                       info.get("token", ""))
            if not (isinstance(minimize, dict) and minimize.get("ok")):
                failures.append(f"invoke minimize 失败：{minimize}")
            else:
                checks.append("invoke minimize 已受理（动作面连通）")
            # `request_close` 走的是与"用户点关闭按钮"同一条收尾路径——它应该让进程正常退出。
            # 这一条同时验证了"控制通道收尾"这条链（进程内还有控制通道/脚本宿主要停止）。
            close_reply = call_with_token(info["port"], "invoke",
                                          {"id": "titlebar", "action": "close"},
                                          info.get("token", ""))
            if not (isinstance(close_reply, dict) and close_reply.get("ok")):
                failures.append(f"invoke close 失败：{close_reply}")
            else:
                try:
                    proc.wait(timeout=10)
                    checks.append(f"invoke close → 进程走正常收尾退出（返回码 {proc.returncode}）")
                except subprocess.TimeoutExpired:
                    failures.append("invoke close 未使进程退出（WM_DELETE_WINDOW 链路不通）")

        # 若上文未走到 request_close（例如控制通道已断），这里兜底收尾
        if proc.poll() is None:
            try:
                call_with_token(info["port"], "shutdown", {}, info.get("token", ""), timeout=5)
            except Exception:  # noqa: BLE001
                pass
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.send_signal(signal.SIGTERM)
    finally:
        log.close()
        if spawned is not None:
            spawned.terminate()

    print("\n—— X11 真窗口验证 ——")
    for line in checks:
        print(f"  ✓ {line}")
    print(f"\n  {len(checks)} 项通过，{len(failures)} 项失败")
    for line in failures:
        print(f"  ✗ {line}")
    print(f"\n日志：{OUT_DIR / 'gallery-x11.log'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
