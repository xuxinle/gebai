"""真窗口连点探针（SendInput 路径）。

## 为什么不能只用控制通道

`input.mouse{kind:"click"}` 在服务端**固定发** `MouseDown → MouseUp → Click`，
不经过后端的消息泵——于是"后端把一次物理手势译成了什么"这件事完全测不到。
本脚本走 `SendInput`（系统输入队列 → 前台窗口），量的是**用户那条链路**。

## 量什么

应用侧：`#drill-button` 的 `on_click` 激活次数（`#drill-state` 的「计数 N」）。
框架侧：脚本宿主绑到同一元素的 `click/dblclick/mousedown/mouseup` 条数。

两个量一起看才能把"多出来的那次"归因到具体事件类型。

用法：py -3 click_gesture_probe.py --ctl <control.json> --hwnd <hwnd>
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import sys
import time

sys.path.insert(0, r"C:\Users\Administrator\code\gebai\shuangtian\tools")
import st_client_lib  # noqa: E402

USER32 = ctypes.WinDLL("user32", use_last_error=True)
try:
    ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)
    USER32.SetThreadDpiAwarenessContext(-4)
except Exception as error:  # noqa: BLE001
    print("DPI 感知声明失败:", error)

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


def send_mouse(flags: int, x: float, y: float) -> None:
    screen_w = USER32.GetSystemMetrics(0)
    screen_h = USER32.GetSystemMetrics(1)
    ax = int(round(x * 65535 / max(1, screen_w - 1)))
    ay = int(round(y * 65535 / max(1, screen_h - 1)))
    item = INPUT(type=0)
    item.mi = MOUSEINPUT(dx=ax, dy=ay, mouseData=0,
                         dwFlags=flags | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK,
                         time=0, dwExtraInfo=None)
    if USER32.SendInput(1, ctypes.byref(item), ctypes.sizeof(INPUT)) != 1:
        raise OSError(f"SendInput 失败：{ctypes.get_last_error()}")


def client_origin(hwnd: int) -> tuple[int, int]:
    point = wt.POINT(0, 0)
    USER32.ClientToScreen(hwnd, ctypes.byref(point))
    return point.x, point.y


def bring_front(hwnd: int) -> None:
    USER32.ShowWindow(hwnd, 9)                       # SW_RESTORE
    USER32.SetWindowPos(hwnd, wt.HWND(-1), 0, 0, 0, 0, 0x0040 | 0x0001 | 0x0002)
    USER32.keybd_event(0x12, 0, 0, 0)                # 让本进程被视作可信输入源
    USER32.keybd_event(0x12, 0, 2, 0)
    USER32.SetForegroundWindow(hwnd)
    time.sleep(0.4)


class Client:
    def __init__(self, ctl_path: str):
        info = st_client_lib.load_control(ctl_path)
        self.port = info["port"]
        self.token = info.get("token", "")

    def call(self, method: str, params: dict | None = None) -> dict:
        return st_client_lib.call_with_token(self.port, method, params or {}, self.token)

    def script(self, code: str) -> dict:
        return self.call("script", {"code": code})

    def center_of(self, element_id: str) -> tuple[float, float]:
        hit = self.call("find", {"selector": "#" + element_id})
        box = hit["result"]["matches"][0]["bounds"]
        return box["x"] + box["width"] / 2, box["y"] + box["height"] / 2

    def activations(self) -> int:
        text = self.call("get", {"id": "drill-state"})["result"]["props"]["text"]
        return int(text.split("计数 ")[1].split(" ")[0])

    def raw(self) -> dict:
        state = self.call("script", {"state": True})["result"].get("state", {})
        return {key: state.get(key, 0) for key in ("c", "db", "d", "u")}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ctl", required=True)
    parser.add_argument("--hwnd", type=int, required=True)
    parser.add_argument("--element", default="drill-button")
    parser.add_argument("--idle", type=float, default=1.5)
    args = parser.parse_args()

    client = Client(args.ctl)
    scale = client.call("metrics")["result"]["device_scale"]

    # 绑定用元素句柄（自带「元素 id|事件」去重）：全局 on() 重复执行会叠加，
    # 一次点击被计两次——那是量具的坑。
    client.script(
        "state.c = 0; state.db = 0; state.d = 0; state.u = 0;"
        f"$('#{args.element}').on('click',function(){{ state.c = state.c + 1; return 0; }});"
        f"$('#{args.element}').on('dblclick',function(){{ state.db = state.db + 1; return 0; }});"
        f"$('#{args.element}').on('mousedown',function(){{ state.d = state.d + 1; return 0; }});"
        f"$('#{args.element}').on('mouseup',function(){{ state.u = state.u + 1; return 0; }});"
        "'ok'"
    )

    bring_front(args.hwnd)
    lx, ly = client.center_of(args.element)
    ox, oy = client_origin(args.hwnd)
    sx, sy = ox + lx * scale, oy + ly * scale
    print(f"{args.element} 逻辑({lx:.1f},{ly:.1f}) -> 屏幕({sx:.1f},{sy:.1f}) scale={scale}")
    send_mouse(MOUSEEVENTF_MOVE, sx, sy)
    time.sleep(0.3)

    rows: list[dict] = []

    def press_release(hold_ms: int = 30) -> None:
        send_mouse(MOUSEEVENTF_LEFTDOWN, sx, sy)
        time.sleep(hold_ms / 1000.0)
        send_mouse(MOUSEEVENTF_LEFTUP, sx, sy)

    def scenario(label: str, presses: int, gap_ms: int) -> None:
        time.sleep(args.idle)   # 跨场景静默：真实双击判定是跨场景的（否则上一击会并进来）
        base = client.activations()
        client.script("state.c = 0; state.db = 0; state.d = 0; state.u = 0; 'r'")
        for index in range(presses):
            if index:
                time.sleep(gap_ms / 1000.0)
            press_release()
        time.sleep(0.6)
        row = {"gesture": label, "activations": client.activations() - base, "events": client.raw()}
        rows.append(row)
        print(f"  {label:<24} activations={row['activations']} events={json.dumps(row['events'])}")

    scenario("single", 1, 0)
    scenario("double(60ms)", 2, 60)
    scenario("two_single(700ms)", 2, 700)
    scenario("triple(60ms)", 3, 60)
    scenario("three_single(700ms)", 3, 700)
    print(json.dumps(rows, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
