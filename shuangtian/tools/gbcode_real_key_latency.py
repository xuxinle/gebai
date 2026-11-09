#!/usr/bin/env python3
"""用**真实键盘输入**（SendInput）测 gbcode 的回显延迟。

## 为什么不能用控制通道的 `input.text`

那条路走 socket：到达后主循环 `poll()` 立即处理并置 `repaint`，
**绕开了"后端事件队列要靠轮询发现"这一环**。用户敲的是真键盘（`WM_CHAR`），
两者不是同一条链路——用 `input.text` 测会得出"不慢"的结论，与用户感受相反。
（实测：`input.text` 的 p50 = 19ms，而真键盘在修复前 350~900ms。）

## 量具

`capture.hash` 整屏哈希：每次要回读 3533×1922 物理像素（~2.7MB），单次几十毫秒——
所以轮询间隔实际是「一次哈希往返」，分辨率 ~30ms。这个精度足够区分 19ms 与 400ms，
但不要用它去断言 10ms 级差异。

用法：
  python tools/gbcode_real_key_latency.py --bin build/dev/bin/gbcode.exe --tag before
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import statistics
import subprocess
import sys
import time
from ctypes import wintypes

sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent))
from st_client_lib import call_with_token  # noqa: E402

user32 = ctypes.WinDLL("user32", use_last_error=True)

INPUT_KEYBOARD = 1
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_UNICODE = 0x0004


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", wintypes.WORD),
        ("wScan", wintypes.WORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


class _INPUTunion(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("padding", ctypes.c_byte * 32)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", _INPUTunion)]


def send_unicode_char(ch: str) -> None:
    """发一个字符：WM_CHAR 由系统从 wScan 合成（与真实键盘同一路径）。"""
    code = ord(ch)
    for flags in (KEYEVENTF_UNICODE, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP):
        inp = INPUT(type=INPUT_KEYBOARD)
        inp.u.ki = KEYBDINPUT(wVk=0, wScan=code, dwFlags=flags, time=0, dwExtraInfo=None)
        user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def focus_window(pid: int) -> bool:
    """把该进程的主窗口拉到前台（否则按键落不到它上面）。"""
    hwnd = user32.GetForegroundWindow()
    target = None
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def enum_proc(h, _l):
        wpid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(wpid))
        if wpid.value == pid and user32.IsWindowVisible(h):
            found.append(h)
        return True

    user32.EnumWindows(enum_proc, 0)
    if not found:
        return False
    target = found[0]
    user32.ShowWindow(target, 9)  # SW_RESTORE
    time.sleep(0.15)
    user32.SetForegroundWindow(target)
    time.sleep(0.35)
    return user32.GetForegroundWindow() == target


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--control-file", required=True)
    ap.add_argument("--keystrokes", type=int, default=10)
    ap.add_argument("--tag", default="?")
    args = ap.parse_args()

    try:
        os.remove(args.control_file)
    except OSError:
        pass

    proc = subprocess.Popen(
        [args.bin, "--control-port", "0", "--control-file", args.control_file],
        cwd=str(__import__("pathlib").Path(__file__).resolve().parent.parent),
    )
    port = token = None
    for _ in range(120):
        try:
            with open(args.control_file, encoding="utf-8") as f:
                info = json.load(f)
            port, token = info["port"], info.get("token", "")
            if call_with_token(port, "ping", {}, token=token):
                break
        except Exception:
            pass
        time.sleep(0.25)
    else:
        proc.kill()
        raise SystemExit("控制通道未就绪")

    def call(method, params=None):
        r = call_with_token(port, method, params or {}, token=token)
        return r.get("result", r)

    time.sleep(2.5)
    if not focus_window(proc.pid):
        print(f"[{args.tag}] ⚠ 无法把窗口拉到前台，测量无效", flush=True)
        proc.kill()
        return 2

    # 打开命令面板 → 让一个 Input 拿到焦点（真实用户也是这么打字的）
    call("input.key", {"key": "P", "ctrl": True, "shift": True})
    time.sleep(1.2)
    if not call("get", {"id": "palette-input"}).get("props", {}).get("focused"):
        print(f"[{args.tag}] ⚠ 命令面板输入框未获焦，测量无效", flush=True)
        proc.kill()
        return 2

    latencies = []
    for _ in range(args.keystrokes):
        h0 = call("capture.hash", {}).get("hash")
        t0 = time.perf_counter()
        send_unicode_char("q")
        changed = None
        while time.perf_counter() - t0 < 3.0:
            if call("capture.hash", {}).get("hash") != h0:
                changed = (time.perf_counter() - t0) * 1000.0
                break
        latencies.append(changed if changed is not None else 3000.0)
        time.sleep(0.6)

    s = sorted(latencies)
    n = len(s)
    m = call("metrics", {})
    print(
        f"[{args.tag}] 真实键盘回显 p50={statistics.median(s):.0f}ms "
        f"p95={s[int(n * 0.95)]:.0f}ms  (paint={m.get('paint_ms')}ms "
        f"wall_p50={m.get('wall_p50_ms')}ms frames={m.get('frames')})",
        flush=True,
    )
    print(f"[{args.tag}] raw={[round(x) for x in latencies]}", flush=True)
    proc.kill()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
