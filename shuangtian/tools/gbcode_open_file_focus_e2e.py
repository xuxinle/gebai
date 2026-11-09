#!/usr/bin/env python3
"""gbcode 回归：**打开文件后键盘输入必须立刻可用**。

## 这条缺陷长什么样

用户报「所有键盘输入回显都很慢」。实测真相不是慢，是**输入全部丢失**：

- 打开文件后 `editor.focused == false`；
- 真键盘打字 8/8 次 `handled=false`、帧号零增长、光标不动；
- 用鼠标点一下编辑区（拿到焦点）后，回显 16~23ms——完全正常。

根因：`open_path()` 不能碰 `editor`（帧首调用时那个指针可能是上一次 `build()`
留下的悬垂指针），聚焦只能等到 `build()` 里装载新文件那一处——而那里原先没有聚焦。

## 判据为什么用「光标真的动了」

`focused` 属性单独看不够：焦点可能在旧实例上，而新实例没拿到。所以先断言
`focused == true`，再**真发键盘输入**并断言编辑器光标位置变了。
两步都过才算通过——只验前者会漏「焦点在了但输入送不进去」，
只验后者在空文档上会碰巧通过。

用法：python tools/gbcode_open_file_focus_e2e.py [--bin <exe>]
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
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


def send_char(ch: str) -> None:
    code = ord(ch)
    for flags in (KEYEVENTF_UNICODE, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP):
        inp = INPUT(type=INPUT_KEYBOARD)
        inp.u.ki = KEYBDINPUT(wVk=0, wScan=code, dwFlags=flags, time=0, dwExtraInfo=None)
        user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def focus_window(pid: int) -> bool:
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
    user32.ShowWindow(found[0], 9)
    time.sleep(0.15)
    user32.SetForegroundWindow(found[0])
    time.sleep(0.4)
    return user32.GetForegroundWindow() == found[0]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="build/dev/bin/gbcode.exe")
    ap.add_argument("--control-file", default=None)
    args = ap.parse_args()

    root = Path(__file__).resolve().parent.parent
    ctl = args.control_file or str(root / "tmp" / "gbcode-open-focus-ctl.json")
    try:
        os.remove(ctl)
    except OSError:
        pass
    Path(ctl).parent.mkdir(parents=True, exist_ok=True)

    proc = subprocess.Popen(
        [str(root / args.bin), "--control-port", "0", "--control-file", ctl], cwd=str(root)
    )
    port = token = ""
    for _ in range(120):
        try:
            with open(ctl, encoding="utf-8") as f:
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

    failures = []
    try:
        time.sleep(2.5)
        if not focus_window(proc.pid):
            raise SystemExit("无法把 gbcode 窗口拉到前台（测量前提）")

        # 用命令面板打开一个真实文件（与用户路径一致）
        call("input.key", {"key": "P", "ctrl": True, "shift": True})
        time.sleep(1.0)
        call("input.text", {"text": "DESIGN", "id": "palette-input"})
        time.sleep(0.6)
        call("input.key", {"key": "Enter"})
        time.sleep(2.5)

        editor = call("get", {"id": "editor"})
        props = editor.get("props") or {}
        lines = int(props.get("lines") or 0)
        print(f"  打开文件：lines={lines}")
        if lines <= 0:
            failures.append("文件没打开（lines=0），本用例前提不成立")

        # ① 焦点
        focused = props.get("focused") is True or props.get("focused") == "True"
        print(f"  ① editor.focused = {props.get('focused')}")
        if not focused:
            failures.append("打开文件后编辑器未获得焦点（键盘输入会全部丢失）")

        # ② 真键盘输入 → 光标必须移动
        before_cursor = props.get("cursor")
        for _ in range(3):
            send_char("X")
        time.sleep(0.6)
        after = (call("get", {"id": "editor"}).get("props") or {})
        after_cursor = after.get("cursor")
        print(f"  ② cursor {before_cursor} -> {after_cursor}")
        if str(before_cursor) == str(after_cursor):
            failures.append("真实键盘输入后光标未移动（输入没进编辑器）")

    finally:
        try:
            proc.kill()
        except Exception:
            pass

    if failures:
        print("  FAIL")
        for f in failures:
            print("    - " + f)
        return 1
    print("  PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
