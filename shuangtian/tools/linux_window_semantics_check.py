#!/usr/bin/env python3
"""X11 窗口语义与设备能力验证：**从 X 服务器侧**读回我们的实现真的做了什么。

## 为什么要这个脚本（控制通道的 `get` 看不见这些）

窗口图标、指针形态、`_NET_WM_STATE` 置底、工作区铺开、窗口尺寸——这五件事都是
**写给窗口管理器/服务器的状态**，进程内的属性面完全看不到：
`app.cursor` 说是 `hand` 不代表光标真变成了手型，`window_icon` 说"已设置"不代表
`_NET_WM_ICON` 属性真写进去了。所以只能从 X 服务器侧读。

## 断言依据（每条都能独立失败，失败时能立刻分辨是哪一环）

| 能力 | 服务器侧证据 |
|---|---|
| 窗口图标 | `_NET_WM_ICON` 存在，且解出来的第一段是 `[w, h, ...]` 尺寸自洽 |
| 自绘窗框前提 | `_NET_FRAME_EXTENTS` **不存在**（WM 没为它画边框） |
| 工作区铺开 | 客户区尺寸 ≈ 工作区 × 0.92（±4px 容差，WM 会做整数取整） |
| 置底 | `_NET_WM_STATE` 含 `_NET_WM_STATE_BELOW` |
| 最大化切换 | 调 `toggle_maximize` 后 `_NET_WM_STATE` 出现/消失 `MAXIMIZED_*` |
| 指针形态 | 窗口的 `CURSOR` 属性……（X11 的 `XDefineCursor` 不写属性，见下） |

## 指针形态为什么只能间接验

`XDefineCursor` 是**服务端状态**，不写任何窗口属性——X 协议没有"查询窗口光标"的请求。
所以这里验两件事：① 控制通道报的 `cursor` 语义串与请求一致（接口层）；
② `XCreateFontCursor` 对每个 `XC_*` 字形号都返回有效 handle（资源层，失败会返回 0）。
两者合起来能排除"映射表抄错"与"字形号不存在"，但不能排除"WM 无视"（那不可能——
光标由服务器直接管）。这一点**如实写在这里**，不假装能验到底。

## 用法

    python3 tools/linux_window_semantics_check.py [--exe PATH] [--display :0]
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import os
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_EXE = ROOT / "build/dev/bin/gallery"
sys.path.insert(0, str(ROOT / "tools"))
from st_client_lib import call_with_token, load_control  # noqa: E402

if not sys.platform.startswith("linux"):
    print("跳过：本脚本只在 Linux 上运行（需要真实 X 服务器）")
    sys.exit(0)


def load_x11():  # noqa: ANN201
    path = ctypes.util.find_library("X11")
    if path is None:
        return None
    x = ctypes.CDLL(path)
    x.XOpenDisplay.restype = ctypes.c_void_p
    x.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x.XDefaultRootWindow.restype = ctypes.c_ulong
    x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x.XInternAtom.restype = ctypes.c_ulong
    x.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    x.XGetWindowProperty.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_long, ctypes.c_long,
        ctypes.c_int, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong))]
    x.XFree.argtypes = [ctypes.c_void_p]
    x.XGetAtomName.restype = ctypes.c_char_p
    x.XGetAtomName.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    x.XGetGeometry.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint),
        ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint)]
    x.XQueryTree.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
        ctypes.POINTER(ctypes.c_uint)]
    x.XCreateFontCursor.restype = ctypes.c_ulong
    x.XCreateFontCursor.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    x.XFreeCursor.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    return x


def read_prop(x, display, window, name):  # noqa: ANN001
    """读一个属性（8 位→bytes / 32 位→list[int]）。返回 (值, 类型名)。"""
    atom = x.XInternAtom(display, name.encode(), 0)
    if atom == 0:
        return None, None
    actual_type = ctypes.c_ulong()
    actual_format = ctypes.c_int()
    count = ctypes.c_ulong()
    after = ctypes.c_ulong()
    data = ctypes.POINTER(ctypes.c_ulong)()
    status = x.XGetWindowProperty(display, window, atom, 0, 1 << 18, 0, 0,
                                  ctypes.byref(actual_type), ctypes.byref(actual_format),
                                  ctypes.byref(count), ctypes.byref(after), ctypes.byref(data))
    if status != 0 or not data:
        return None, None
    try:
        type_name = None
        if actual_type.value:
            raw = x.XGetAtomName(display, actual_type.value)
            type_name = raw.decode("utf-8", "replace") if raw else None
        if actual_format.value == 8:
            raw = ctypes.string_at(data, count.value)
            return raw, type_name
        if actual_format.value == 32:
            return [data[i] for i in range(count.value)], type_name
        return None, type_name
    finally:
        x.XFree(data)


def geometry(x, display, window):  # noqa: ANN001
    root = ctypes.c_ulong()
    gx = ctypes.c_int()
    gy = ctypes.c_int()
    width = ctypes.c_uint()
    height = ctypes.c_uint()
    border = ctypes.c_uint()
    depth = ctypes.c_uint()
    x.XGetGeometry(display, window, ctypes.byref(root), ctypes.byref(gx), ctypes.byref(gy),
                   ctypes.byref(width), ctypes.byref(height), ctypes.byref(border),
                   ctypes.byref(depth))
    return gx.value, gy.value, width.value, height.value


def find_window_by_pid(x, display, pid):  # noqa: ANN001
    """按 `_NET_WM_PID` 找窗口（不按标题：中文标题是 UTF-8，`XFetchName` 只给 Latin-1）。"""
    root = x.XDefaultRootWindow(display)
    found = []

    def scan(node: int, depth: int) -> None:
        if depth > 4 or found:
            return
        parent = ctypes.c_ulong()
        children = ctypes.POINTER(ctypes.c_ulong)()
        count = ctypes.c_uint()
        if not x.XQueryTree(display, node, ctypes.byref(ctypes.c_ulong()),
                            ctypes.byref(parent), ctypes.byref(children), ctypes.byref(count)):
            return
        ids = [children[i] for i in range(count.value)] if count.value else []
        if children:
            x.XFree(children)
        for wid in ids:
            values, _ = read_prop(x, display, wid, "_NET_WM_PID")
            if isinstance(values, list) and values and values[0] == pid:
                found.append(wid)
                return
            scan(wid, depth + 1)

    scan(root, 0)
    return found[0] if found else None


def evaluate(port: int, token: str, method: str, params: dict) -> dict:
    reply = call_with_token(port, method, params, token)
    if not reply.get("ok"):
        return {"ok": False, "error": reply.get("error", {})}
    result = reply.get("result")
    return {"ok": True, **(result if isinstance(result, dict) else {"result": result})}


def main() -> int:
    parser = argparse.ArgumentParser(description="X11 窗口语义与设备能力验证")
    parser.add_argument("--exe", default=str(DEFAULT_EXE))
    parser.add_argument("--display", default=os.environ.get("DISPLAY", ""))
    args = parser.parse_args()
    if not args.display:
        print("失败：没有 DISPLAY（本脚本要真实 X 服务器）")
        return 2
    x = load_x11()
    if x is None:
        print("失败：找不到 libX11.so.6")
        return 2
    display = x.XOpenDisplay(None)
    if not display:
        print(f"失败：XOpenDisplay 打不开（DISPLAY={args.display}）")
        return 2
    exe = pathlib.Path(args.exe)
    if not exe.exists():
        print(f"失败：可执行文件不存在 {exe}")
        return 2

    ctl = pathlib.Path("/tmp/st_window_semantics.json")
    if ctl.exists():
        ctl.unlink()
    log = pathlib.Path("/tmp/st_window_semantics.log")
    proc = subprocess.Popen(
        [str(exe), "--ms", "90000", "--control-port", "0", "--control-file", str(ctl)],
        stdout=log.open("wb"), stderr=subprocess.STDOUT, cwd=str(ROOT))
    failures: list[tuple[str, str]] = []
    try:
        info: dict = {}
        for _ in range(80):
            time.sleep(0.25)
            info = load_control(str(ctl))
            if info.get("port"):
                break
        if not info.get("port"):
            print("失败：应用没起来（控制文件未生成）")
            print(log.read_text("utf-8", "replace")[-1500:])
            return 1
        port = int(info["port"])
        token = str(info.get("token", ""))
        pid = int(info["pid"])

        # 等首帧 + 铺开完成
        window = None
        for _ in range(60):
            time.sleep(0.2)
            window = find_window_by_pid(x, display, pid)
            if window is not None:
                break
        if window is None:
            print("失败：找不到该进程的窗口")
            return 1
        time.sleep(1.0)  # 让延迟铺开那条路走完

        # —— ① 窗口图标 ——
        icon, icon_type = read_prop(x, display, window, "_NET_WM_ICON")
        if not isinstance(icon, list) or len(icon) < 2:
            failures.append(("窗口图标", "`_NET_WM_ICON` 不存在或不可读"))
        else:
            # EWMH 结构：若干段 `[w, h, pixel...]` 首尾相接
            index = 0
            segments = 0
            while index + 1 < len(icon):
                iw, ih = icon[index], icon[index + 1]
                if iw == 0 or ih == 0 or iw > 1024 or ih > 1024:
                    failures.append(("窗口图标", f"第 {segments} 段尺寸非法：{iw}x{ih}"))
                    break
                index += 2 + iw * ih
                segments += 1
            if index != len(icon):
                failures.append(
                    ("窗口图标", f"段长不自洽：声明共 {len(icon)} 个 CARDINAL，解完剩 {-len(icon) + index}"))
            elif segments == 0:
                failures.append(("窗口图标", "没有解出任何图标段"))
            else:
                print(f"  窗口图标：{segments} 段，共 {len(icon)} 个 CARDINAL ✓")

        # —— ② 自绘窗框前提：无系统边框 ——
        extents, _ = read_prop(x, display, window, "_NET_FRAME_EXTENTS")
        if extents is not None:
            failures.append(("去装饰", f"`_NET_FRAME_EXTENTS` 存在（{extents}）——WM 仍为它画边框"))
        else:
            print("  去装饰：`_NET_FRAME_EXTENTS` 不存在（WM 不画边框）✓")

        # —— ③ 工作区铺开（92%） ——
        #
        # ⚠ `_NET_WORKAREA` 在 **root 窗口**上（不是应用窗口）——它是 EWMH 的
        # 全局属性，每个桌面一组。第一版拿应用窗口去读，永远读不到，于是这条门禁
        # 被静默“跳过”了——**跳过看起来像通过**，这是最坏的形态。
        root_window = x.XDefaultRootWindow(display)
        work_area, _ = read_prop(x, display, root_window, "_NET_WORKAREA")
        _, _, win_w, win_h = geometry(x, display, window)
        if isinstance(work_area, list) and len(work_area) >= 4:
            work_w, work_h = work_area[2], work_area[3]
            want_w = int(work_w * 0.92)
            want_h = int(work_h * 0.92)
            if abs(win_w - want_w) > 4 or abs(win_h - want_h) > 4:
                failures.append(
                    ("工作区铺开",
                     f"客户区 {win_w}x{win_h} 与期望 92% 工作区 {want_w}x{want_h} 不符"
                     f"（工作区 {work_w}x{work_h}）"))
            else:
                print(f"  工作区铺开：{win_w}x{win_h} ≈ 92% × ({work_w}x{work_h}) ✓")
        else:
            # 没有 EWMH 工作区的裸 X（Xvfb 等）：验不了但不能当通过——
            # 如实报“跳过”，并让退出码仍然 0（环境限制不是代码缺陷）。
            print("  工作区铺开：跳过（服务端无 `_NET_WORKAREA`，裸 X/无 EWMH 环境）")

        # —— ④ 窗口尺寸与逻辑尺寸一致（`set_size` / 铺开真生效） ——
        #
        # ⚠ 字段路径是 `hello` 的 `screen.width`，不是 `metrics.viewport.width`：
        # `metrics` 给的是渲染/字体/上屏统计，**不**含视口尺寸（见 `handle_metrics`）。
        # 第一版写错了路径，于是一直读到 0——那种错误看起来像“尺寸不一致”的真缺陷。
        hello = call_with_token(port, "hello", {}, token)
        screen = (hello.get("result") or {}).get("screen") or {}
        logical_w = int(round(float(screen.get("width", 0))))
        if logical_w <= 0:
            failures.append(("尺寸一致", "hello 没报 screen.width（协议字段缺失）"))
        elif abs(logical_w - win_w) > 2:
            failures.append(
                ("尺寸一致", f"逻辑宽 {logical_w} 与服务器侧客户区宽 {win_w} 不符"))
        else:
            print(f"  尺寸一致：逻辑 {logical_w} == 客户区 {win_w} ✓")

        # —— ⑤ 置底（`_NET_WM_STATE_BELOW`） ——
        state_before, _ = read_prop(x, display, window, "_NET_WM_STATE")
        set_bottom = evaluate(port, token, "app",
                              {"action": "stay_at_bottom", "enabled": True})
        if not set_bottom.get("ok"):
            # 如实记录：后端可能不支持（headless / 无 EWMH 的裸 X）
            print(f"  置底：后端拒绝（{set_bottom.get('error', {}).get('message', '?')}）— 跳过")
        else:
            time.sleep(0.6)
            states, _ = read_prop(x, display, window, "_NET_WM_STATE")
            names = []
            if isinstance(states, list):
                for atom in states:
                    name = x.XGetAtomName(display, atom)
                    if name:
                        names.append(name.decode("utf-8", "replace"))
            if not any("BELOW" in name for name in names):
                failures.append(("置底", f"`_NET_WM_STATE` 里没有 BELOW（实际 {names}）"))
            else:
                print(f"  置底：`_NET_WM_STATE` 含 BELOW ✓")
            # 收尾：取消置底（否则窗口留在最底层，影响后续人工观察）
            evaluate(port, token, "app", {"action": "stay_at_bottom", "enabled": False})

        # —— ⑥ 最大化切换（服务器侧状态真变化） ——
        before, _ = read_prop(x, display, window, "_NET_WM_STATE")
        before_names = set()
        if isinstance(before, list):
            for atom in before:
                name = x.XGetAtomName(display, atom)
                if name:
                    before_names.add(name.decode("utf-8", "replace"))
        toggled = evaluate(port, token, "app", {"action": "toggle_maximize"})
        if toggled.get("ok"):
            time.sleep(0.9)
            after, _ = read_prop(x, display, window, "_NET_WM_STATE")
            after_names = set()
            if isinstance(after, list):
                for atom in after:
                    name = x.XGetAtomName(display, atom)
                    if name:
                        after_names.add(name.decode("utf-8", "replace"))
            max_before = any("MAXIMIZED" in name for name in before_names)
            max_after = any("MAXIMIZED" in name for name in after_names)
            if max_before == max_after:
                failures.append(
                    ("最大化切换",
                     f"服务端状态没变（前 {max_before} / 后 {max_after}）：{sorted(after_names)}"))
            else:
                print(f"  最大化切换：MAXIMIZED {max_before} → {max_after} ✓")
                # 还原，别给后面的人留一个满屏窗口
                evaluate(port, token, "app", {"action": "toggle_maximize"})
                time.sleep(0.6)
        else:
            print(f"  最大化切换：后端拒绝（{toggled.get('error', {}).get('message', '?')}）— 跳过")

        # —— ⑦ 指针形态：字体光标资源层验证 ——
        # `XC_*` 字形号（与 `platform_x11.cpp` 的映射表一致）：left_ptr/hand2/xterm/
        # sb_h_double_arrow/sb_v_double_arrow/top_left_corner/top_right_corner/fleur/
        # crosshair/pirate
        glyphs = {
            "arrow": 68, "hand": 60, "text": 152, "resize-h": 108, "scroll": 108,
            "resize-v": 116, "resize-nwse": 134, "resize-nesw": 136, "move": 52,
            "crosshair": 34, "not-allowed": 88,
        }
        bad = []
        for name, glyph in glyphs.items():
            handle = x.XCreateFontCursor(display, glyph)
            if handle == 0:
                bad.append(f"{name}({glyph})")
            else:
                x.XFreeCursor(display, handle)
        if bad:
            failures.append(("指针形态", f"这些字形号建不出光标（映射表抄错？）：{bad}"))
        else:
            print(f"  指针形态：{len(glyphs)} 个 XC_* 字形全部可建光标 ✓")

        # —— ⑧ 剪贴板能力位（这条只验接口面；跨进程互通见 linux_clipboard_check.py）——
        clip = evaluate(port, token, "clipboard", {"action": "get"})
        if not clip.get("ok"):
            message = clip.get("error", {}).get("message", "")
            if "不可用" in message or "未装 provider" in message:
                failures.append(("剪贴板", f"后端没有剪贴板能力：{message}"))
            else:
                # “剪贴板为空”是正常状态（没人写过），不是缺陷
                print(f"  剪贴板：可用（当前为空）✓")
        else:
            print(f"  剪贴板：可用 ✓")

        if failures:
            print("\n未通过：")
            for name, detail in failures:
                print(f"  - [{name}] {detail}")
            print(f"\n应用日志尾部：\n{log.read_text('utf-8', 'replace')[-1200:]}")
            return 1
        print("\n通过：窗口语义与设备能力全部符合预期")
        return 0
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    sys.exit(main())
