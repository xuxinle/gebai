"""真实窗口下的「AI 控制态」验证：置灰控件连真实鼠标点击都点不动。

为什么不只用控制通道 `invoke`：那条通道**按其本性绕过 enabled**（它是给自动化用的
「直接触发」入口），所以 `invoke btn-split` 即使按钮置灰也会被调用——真正拦住它的是
数据层门禁。而用户按的是**鼠标**，走的才是 enabled 检查。两者都要验，缺一不可：

- 数据层（`Project::can_edit()`）：`tools/caiyun_director_e2e.py` 的第 6 步验；
- UI 层（`set_enabled(false)`）：本脚本验（需真窗口，无头下没有鼠标系统事件）。

用法：
    # 先以真窗口 + AI 控制态启动（--ai-control 会把人控件置灰）
    caiyun.exe --ai-control --control-file tmp/win2.json
    python tools/caiyun_lock_realwindow.py tmp/win2.json
"""
import ctypes
import ctypes.wintypes as w
import json
import socket
import struct
import subprocess
import sys
import time

ctypes.windll.shcore.SetProcessDpiAwareness(2)
USER32 = ctypes.windll.user32

# 窗口的设计宽度（逻辑像素）：用来从实际物理尺寸反推 DPI 倍率——
# 写死 1.5 会在 100% 屏上把点击坐标全打偏。
DESIGN_WIDTH = 1180.0


def rpc(port, token):
    sock = socket.create_connection(("127.0.0.1", port), timeout=10)

    def call(method, params=None, seq=[0]):
        seq[0] += 1
        body = json.dumps({"id": seq[0], "method": method, "params": params or {}}).encode()
        sock.sendall(struct.pack(">I", len(body)) + body)
        header = b""
        while len(header) < 4:
            header += sock.recv(4 - len(header))
        length = struct.unpack(">I", header)[0]
        payload = b""
        while len(payload) < length:
            payload += sock.recv(length - len(payload))
        return json.loads(payload)

    call("hello", {"token": token})
    return call


def find_window():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq caiyun.exe", "/FO", "CSV"],
                         capture_output=True, text=True).stdout
    pids = {int(line.split(",")[1].strip('"')) for line in out.strip().splitlines()[1:] if line.strip()}
    found = []

    def cb(hwnd, _):
        pid = w.DWORD()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value in pids and USER32.IsWindowVisible(hwnd):
            name = ctypes.create_unicode_buffer(256)
            USER32.GetWindowTextW(hwnd, name, 256)
            if name.value:
                rect = w.RECT()
                USER32.GetWindowRect(hwnd, ctypes.byref(rect))
                found.append((hwnd, name.value, rect))
        return True

    USER32.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, w.HWND, w.LPARAM)(cb), 0)
    if not found:
        raise SystemExit("找不到 caiyun 窗口（是否以 --ai-control 无头启动？）")
    return found[0]


def main():
    control_file = sys.argv[1] if len(sys.argv) > 1 else "tmp/win2.json"
    info = json.load(open(control_file, encoding="utf-8"))
    call = rpc(info["port"], info.get("token", ""))
    hwnd, name, rect = find_window()
    print(f"真实窗口：{name} @ ({rect.left},{rect.top})-({rect.right},{rect.bottom})")

    # 逻辑 → 物理：用窗口实际宽度反推 DPI 倍率（设计宽 1180）。
    scale = (rect.right - rect.left) / DESIGN_WIDTH
    print(f"DPI 倍率：{scale:.2f}")
    USER32.SetForegroundWindow(hwnd)
    time.sleep(0.6)

    failures = []
    for element_id in ("inspector-opacity", "inspector-speed", "btn-split", "btn-render",
                       "director-script-path"):
        match = call("find", {"selector": "#" + element_id})["result"]["matches"][0]
        bounds = match["bounds"]
        # 点靠右一点：滑杆要靠右才能改变取值（点中心可能正好是当前值）
        cx = bounds["x"] + int(bounds["width"] * 0.8)
        cy = bounds["y"] + bounds["height"] // 2
        sx = int(rect.left + cx * scale)
        sy = int(rect.top + cy * scale)

        before = call("get", {"id": "timeline"})["result"]["props"]
        USER32.SetCursorPos(sx, sy)
        time.sleep(0.1)
        USER32.mouse_event(0x0002, 0, 0, 0, 0)   # 左键 down
        USER32.mouse_event(0x0004, 0, 0, 0, 0)   # 左键 up
        time.sleep(0.35)
        after = call("get", {"id": "timeline"})["result"]["props"]
        clip = call("get", {"id": "inspector-opacity"})["result"]["props"]

        changed = (before["clip_count"] != after["clip_count"] or
                   abs(float(clip.get("value", 1.0)) - 1.0) > 1e-3)
        status = "CHANGED!" if changed else "ok"
        if changed:
            failures.append(element_id)
        print(f"  {element_id:26s} 真实点击 → {status}")

    # 播放按钮必须仍然可用（查看不算编辑）
    match = call("find", {"selector": "#transport-play"})["result"]["matches"][0]
    bounds = match["bounds"]
    sx = int(rect.left + (bounds["x"] + bounds["width"] // 2) * scale)
    sy = int(rect.top + (bounds["y"] + bounds["height"] // 2) * scale)
    USER32.SetCursorPos(sx, sy)
    USER32.mouse_event(0x0002, 0, 0, 0, 0)
    USER32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(1.2)
    playing = call("get", {"id": "timeline"})["result"]["props"]["transport"]
    print(f"  transport-play             真实点击 → transport={playing}（应不再是 paused）")
    if playing.startswith("paused"):
        failures.append("transport-play（查看也被锁死了）")

    if failures:
        raise SystemExit(f"AI 控制态下这些控件仍被真实点击改动了：{failures}")
    print("\n[OK] 置灰控件对真实鼠标完全失效；播放（查看）仍可用")


if __name__ == "__main__":
    main()
