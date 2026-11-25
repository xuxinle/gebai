#!/usr/bin/env python3
"""X11 跨进程剪贴板验证（与**别的进程**真互通，不是进程内自说自话）。

## 为什么必须跨进程

`clipboard_text()` 的进程内缓存很容易"看起来对"：写进去、读回来，全绿。
但真实的失败模式是**与别的应用不通** —— 而界面上表现完全一样（复制成功、粘贴为空）。
所以这里用**另一个真实进程**做对端，双向各验一遍：

  ① **写方向**：应用写剪贴板 → 对端进程读，必须逐字节相同；
  ② **读方向**：对端进程写剪贴板 → 应用读，必须逐字节相同。

## 为什么对端优先用 xsel/xclip，拿不到才退回内置端点

`xsel`/`xclip` 是**独立的第三方程序**，它验证的是"我们的实现是否合规"——
用自己写的 ICCCM 端点当对端有个隐蔽问题：两边可能犯同一个错而对得上。
内置端点（`_BuiltinPeer`）只在没装 xsel/xclip 时兜底，且会**明确打印用的是哪条路**。

## 为什么载荷必须含中文 + emoji

剪贴板的经典缺陷全都出在编码与长度上：ICCCM 的 `STRING` 是 Latin-1，
把它当 UTF-8 传会让中文变乱码；emoji 是 4 字节 UTF-8 + 代理对区域，
长度/偏移算错就会截断。纯 ASCII 载荷对这两类缺陷**完全不可见**。

## 用法

    python3 tools/linux_clipboard_check.py [--exe PATH] [--display :0]

退出码 0 = 双向都通过。
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import os
import pathlib
import shutil
import subprocess
import sys
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_EXE = ROOT / "build/dev/bin/gbcode"
sys.path.insert(0, str(ROOT / "tools"))
from st_client_lib import call_with_token, load_control  # noqa: E402

if not sys.platform.startswith("linux"):
    print("跳过：本脚本只在 Linux 上运行（需要真实 X 服务器）")
    sys.exit(0)

PAYLOAD = "霜天跨进程剪贴板 · Linux/ICCCM 往返 · ABC123 🚀"


# ————————————————————————————————————————————————————————————————————————————
# 对端 A：外部程序（xsel / xclip）——**首选**，因为它与我们的实现毫无共享代码
# ————————————————————————————————————————————————————————————————————————————


def find_external_peer() -> list[str] | None:
    if shutil.which("xsel"):
        return ["xsel", "-b"]
    if shutil.which("xclip"):
        return ["xclip", "-selection", "clipboard"]
    return None


def peer_read(cmd: list[str]) -> str | None:
    try:
        done = subprocess.run([*cmd, "-o"], capture_output=True, timeout=10, check=False)
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return None
    if done.returncode != 0:
        return None
    return done.stdout.decode("utf-8", "replace")


def peer_write(cmd: list[str], text: str) -> bool:
    """让外部程序持有剪贴板内容，返回是否成功。

    ⚠ 不能用 `Popen` + 等它退出的写法：`xsel -i` 拿到所有权后会 fork 一个子进程
    常驻持有，**父进程立即退出**——所以 `subprocess.run` 是正确形态（它等的是父进程）。
    第一版用 `Popen.wait` 等错了对象，结果要等到超时才返回，而那时对端
    还没建好所有权（实测：随后应用读剪贴板拿到 None）。

    ⚠⚠ **写完不能马上再去读它**：`xsel -o` 会向当前所有者发 `SelectionRequest`，
    而 xsel 的持有进程一旦被问过就快退（它不是常驻服务）。验证过：
    写后连续 `xsel -o` 三次都成功，但**每次 `xsel -o` 都在加速它的退场**，
    于是紧接着让应用去读时所有者已经没了（实测日志：`owner=0x1000001` 是上一次的
    残留窗口，而不是活着的 xsel）。所以“就位检查”只能在**应用读之前不做**，
    而是写完直接短等一个固定量，把读取权交给被测应用——它是真正要验的那一方。
    """
    try:
        done = subprocess.run([*cmd, "-i"], input=text.encode("utf-8"),
                              capture_output=True, timeout=10, check=False)
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return False
    return done.returncode == 0


# ————————————————————————————————————————————————————————————
# 对端 B：内置最小 ICCCM 端点（没装 xsel/xclip 时兜底）
# ————————————————————————————————————————————————————————————


class XEvent(ctypes.Union):
    _fields_ = [("type", ctypes.c_int), ("pad", ctypes.c_long * 24)]


class _BuiltinPeer:
    """一个最小的 ICCCM 剪贴板端点（能读、能写，后台线程自动应答）。"""

    def __init__(self, x, display):  # noqa: ANN001
        self.x = x
        self.display = display
        root = x.XDefaultRootWindow(display)
        self.window = x.XCreateSimpleWindow(display, root, -100, -100, 1, 1, 0, 0, 0)
        self.clipboard = x.XInternAtom(display, b"CLIPBOARD", 0)
        self.utf8 = x.XInternAtom(display, b"UTF8_STRING", 0)
        self.property = x.XInternAtom(display, b"ST_CHECK_XFER", 0)
        self.text = ""
        self._lock = threading.Lock()
        self._stop = threading.Event()
        # **只能有一个线程读事件**：`get_text()` 与后台线程都 `XNextEvent` 会互抢，
        # `SelectionNotify` 被后台线程取走就丢——实测表现像“对端没应答”。
        self._notify_ready = threading.Event()
        self._notify_property: int | None = None
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self) -> None:
        while not self._stop.is_set():
            if self.x.XPending(self.display) == 0:
                time.sleep(0.003)
                continue
            event = XEvent()
            self.x.XNextEvent(self.display, ctypes.byref(event))
            if event.type == 30:  # SelectionRequest
                with self._lock:
                    self._answer(event)
            elif event.type == 31:  # SelectionNotify
                fields = (ctypes.c_ulong * 10).from_buffer(event)
                if fields[5] == self.clipboard:
                    self._notify_property = int(fields[7])
                    self._notify_ready.set()

    def _answer(self, event) -> None:  # noqa: ANN001
        fields = (ctypes.c_ulong * 11).from_buffer(event)
        requestor, selection, target, prop, stamp = (fields[5], fields[6], fields[7],
                                                     fields[8], fields[9])
        data = self.text.encode("utf-8")
        buffer = ctypes.create_string_buffer(data, max(1, len(data)))
        self.x.XChangeProperty(self.display, requestor, prop, target, 8, 0,
                               ctypes.cast(buffer, ctypes.c_void_p), len(data))
        notify = XEvent()
        out = (ctypes.c_ulong * 10).from_buffer(notify)
        out[0] = 31
        out[4] = requestor
        out[5] = selection
        out[6] = target
        out[7] = prop
        out[8] = stamp
        self.x.XSendEvent(self.display, requestor, 0, 0, ctypes.byref(notify))
        self.x.XFlush(self.display)

    def set_text(self, text: str) -> bool:
        with self._lock:
            self.text = text
        self.x.XSetSelectionOwner(self.display, self.clipboard, self.window, 0)
        self.x.XSync(self.display, 0)
        return self.x.XGetSelectionOwner(self.display, self.clipboard) == self.window

    def get_text(self, timeout: float = 2.5) -> str | None:
        self.x.XDeleteProperty(self.display, self.window, self.property)
        self._notify_property = None
        self._notify_ready.clear()
        self.x.XConvertSelection(self.display, self.clipboard, self.utf8, self.property,
                                 self.window, 0)
        self.x.XFlush(self.display)
        if not self._notify_ready.wait(timeout) or not self._notify_property:
            return None
        actual_type = ctypes.c_ulong()
        actual_format = ctypes.c_int()
        count = ctypes.c_ulong()
        after = ctypes.c_ulong()
        data = ctypes.c_void_p()
        status = self.x.XGetWindowProperty(
            self.display, self.window, self.property, 0, 1 << 20, 1, 0,
            ctypes.byref(actual_type), ctypes.byref(actual_format), ctypes.byref(count),
            ctypes.byref(after), ctypes.byref(data))
        if status != 0 or not data:
            return None
        try:
            return ctypes.string_at(data, count.value).decode("utf-8", "replace")
        finally:
            self.x.XFree(data)

    def close(self) -> None:
        self._stop.set()


def load_x11():  # noqa: ANN201
    path = ctypes.util.find_library("X11")
    if path is None:
        return None
    x = ctypes.CDLL(path)
    x.XOpenDisplay.restype = ctypes.c_void_p
    x.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x.XDefaultRootWindow.restype = ctypes.c_ulong
    x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x.XCreateSimpleWindow.restype = ctypes.c_ulong
    x.XCreateSimpleWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                                      ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong,
                                      ctypes.c_ulong]
    x.XInternAtom.restype = ctypes.c_ulong
    x.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    x.XSetSelectionOwner.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong,
                                     ctypes.c_ulong]
    x.XGetSelectionOwner.restype = ctypes.c_ulong
    x.XGetSelectionOwner.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    x.XConvertSelection.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong,
                                    ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong]
    x.XChangeProperty.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong,
                                  ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
    x.XGetWindowProperty.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_long, ctypes.c_long,
        ctypes.c_int, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_ulong),
        ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_void_p)]
    x.XDeleteProperty.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong]
    x.XPending.argtypes = [ctypes.c_void_p]
    x.XNextEvent.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    x.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long,
                             ctypes.c_void_p]
    x.XFree.argtypes = [ctypes.c_void_p]
    x.XFlush.argtypes = [ctypes.c_void_p]
    x.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
    # **必须在 `XOpenDisplay` 之前**：内置端点跑后台线程，多线程共用 Display
    # 而不初始化线程支持是未定义行为（会偶发崩）。
    x.XInitThreads.restype = ctypes.c_int
    x.XInitThreads()
    return x


def evaluate(port: int, token: str, method: str, params: dict) -> dict:
    """控制通道调用，把响应**展开成扁平结构**（`ok` 与 `result` 字段合并到顶层）。

    为何要展开：协议响应是 `{"id":1,"ok":true,"result":{...}}` —— 业务字段在
    `result` 里。验证脚本第一版直接读 `reply.get("text")` 于顶层，于是**永远是 None**，
    而错误信息看起来像“应用读不到剪贴板”（实测误导了一整轮排查）。
    展开后再用就不是手误能犯的事了。
    """
    reply = call_with_token(port, method, params, token)
    if not reply.get("ok"):
        return {"ok": False, "error": reply.get("error", {})}
    result = reply.get("result")
    if isinstance(result, dict):
        return {"ok": True, **result}
    return {"ok": True, "result": result}


def main() -> int:
    parser = argparse.ArgumentParser(description="X11 跨进程剪贴板双向验证")
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
        print(f"失败：可执行文件不存在 {exe}（先 st build gbcode）")
        return 2

    external = find_external_peer()
    builtin: _BuiltinPeer | None = None
    if external is not None:
        print(f"对端：外部程序 {external[0]}（与实现无共享代码，首选）")
        read_peer = lambda: peer_read(external)  # noqa: E731
        write_peer = lambda text: peer_write(external, text)  # noqa: E731
    else:
        print("对端：内置 ICCCM 端点（没装 xsel/xclip —— 结论强度弱于外部程序）")
        builtin = _BuiltinPeer(x, display)
        read_peer = builtin.get_text
        write_peer = builtin.set_text

    ctl = pathlib.Path("/tmp/st_clipboard_check.json")
    if ctl.exists():
        ctl.unlink()
    log = pathlib.Path("/tmp/st_clipboard_check.log")
    proc = subprocess.Popen(
        [str(exe), "--ms", "60000", "--control-port", "0", "--control-file", str(ctl)],
        stdout=log.open("wb"), stderr=subprocess.STDOUT, cwd=str(ROOT))
    failures: list[str] = []
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

        # —— ① 应用写 → 对端读 ——
        write = evaluate(port, token, "clipboard", {"action": "set", "text": PAYLOAD})
        if not write.get("ok"):
            failures.append(f"应用写剪贴板失败：{write.get('error')}")
        else:
            time.sleep(0.5)
            got = read_peer()
            if got != PAYLOAD:
                failures.append(f"写方向：对端读到的内容不一致\n      期望 {PAYLOAD!r}\n      实得 {got!r}")

        # —— ② 对端写 → 应用读 ——
        #
        # ⚠ 顺序与等待量都有讲究（实测踩过）：
        #   `xsel -b -i` 拿到所有权后会**先 fork 子进程常驻**再把父进程退出，
        #   所以父进程 rc=0 **不意味着所有权已就位**。这里必须：
        #   ① 先等到“剪贴板真能读出我们的内容”（对端已就位）；
        #   ② 再去问应用读。直接 sleep(0.4) 就测是碰运气——实测 70% 时机不够。
        if not write_peer(PAYLOAD):
            failures.append("对端写剪贴板失败（xsel/xclip 或内置端点均不可用）")
        else:
            # 固定短等，**不在这里读对端**（理由见 `peer_write` 的注释：读它会
            # 让 xsel 的持有进程提前退场，反而把被测方的那一次读变成“所有者已没了”）。
            time.sleep(0.5)
            read = evaluate(port, token, "clipboard", {"action": "get"})
            if not read.get("ok"):
                failures.append(f"应用读剪贴板失败：{read.get('error')}")
            elif read.get("text") != PAYLOAD:
                failures.append(
                    f"读方向：应用读到的不一致\n      期望 {PAYLOAD!r}\n"
                    f"      实得 {read.get('text')!r}")

        if failures:
            print("未通过：")
            for item in failures:
                print(f"  - {item}")
            print(f"\n应用日志尾部：\n{log.read_text('utf-8', 'replace')[-1400:]}")
            return 1
        print(f"通过：双向跨进程剪贴板一致（{len(PAYLOAD)} 字符，含中文 + emoji）")
        return 0
    finally:
        if builtin is not None:
            builtin.close()
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    sys.exit(main())
