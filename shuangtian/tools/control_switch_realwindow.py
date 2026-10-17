"""真实窗口下验证「人可切换主动控制 / 自动接管」——两个应用都要过。

为什么要真实窗口：控制通道的 `invoke activate` 走的是"直接触发"，而用户按的是**鼠标**。
两者在 `Switch` 上确实汇到同一个 `toggle()`，但"以为会汇"不等于"验过会汇"——
这个脚本用真 `mouse_event` 点那个开关，再用控制通道回读状态。

用法：
    caiyun.exe --control-file tmp/caiyun-real.json      (真窗口，不带 --headless)
    python tools/control_switch_realwindow.py caiyun tmp/caiyun-real.json
    python tools/control_switch_realwindow.py louyue tmp/louyue-real.json
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

# Windows 控制台默认 GBK：不重配的话，含「✓/✗」的输出会直接抛 UnicodeEncodeError
# （与另两个 e2e 脚本同一处理）。
if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

# 每个应用的探针：开关 id、锁定态判据（属性名 + 值）、被锁的编辑动作、可用动作
CASES = {
    "caiyun": {
        "app": "caiyun.exe",
        "switch": "control-switch",
        "state_probe": ("director", "edit_locked", "true"),
        "locked_edit": ("timeline", "set_opacity", "1 0.3"),
        "readonly": ("timeline", "seek", "2.0"),
        "design_width": 1180,
    },
    "louyue": {
        "app": "louyue.exe",
        "switch": "poster-control-switch",
        "state_probe": ("poster", "edit_locked", "true"),
        "locked_edit": ("canvas", "set_opacity", "0 0.3"),
        "readonly": ("canvas", "zoom_out", None),
        "design_width": 1180,
    },
}


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


def find_window(image_name):
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {image_name}", "/FO", "CSV"],
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
                found.append((hwnd, name.value))
        return True

    USER32.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, w.HWND, w.LPARAM)(cb), 0)
    if not found:
        raise SystemExit(f"找不到 {image_name} 的可见窗口（是否以 --headless 启动？）")
    hwnd, title = found[0]

    # ⚠ 必须用**客户区**原点，不能用 `GetWindowRect`：后者含标题栏/边框，
    # 而控制通道的逻辑坐标从客户区左上角算。用窗口矩形会让点击整体下移一个
    # 标题栏高（实测：开关在逻辑 y=402，实际点到了 624）——然后被误读成
    # “框架的 on_change 没接上”。
    origin = w.POINT(0, 0)
    USER32.ClientToScreen(hwnd, ctypes.byref(origin))
    client = w.RECT()
    USER32.GetClientRect(hwnd, ctypes.byref(client))
    return hwnd, title, origin, (client.right - client.left, client.bottom - client.top)


def click_at(hwnd, origin, bounds, scale):
    """按逻辑 bounds 点控件中心（逻辑 → 屏幕物理，基准是客户区原点）。"""
    cx = bounds["x"] + bounds["width"] / 2.0
    cy = bounds["y"] + bounds["height"] / 2.0
    sx = int(origin.x + cx * scale)
    sy = int(origin.y + cy * scale)
    USER32.SetForegroundWindow(hwnd)
    time.sleep(0.2)
    USER32.SetCursorPos(sx, sy)
    time.sleep(0.1)
    USER32.mouse_event(0x0002, 0, 0, 0, 0)
    USER32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.45)
    return sx, sy


def main():
    if len(sys.argv) < 3:
        raise SystemExit("用法：control_switch_realwindow.py <caiyun|louyue> <控制文件>")
    name = sys.argv[1]
    case = CASES[name]
    info = json.load(open(sys.argv[2], encoding="utf-8"))
    call = rpc(info["port"], info.get("token", ""))
    hwnd, title, origin, client_size = find_window(case["app"])
    # DPI 倍率取**控制通道自己的** `device_scale`，不从窗口像素反推：
    # 反推要假设窗口的逻辑尺寸，而它随布局变（镂月两侧面板 260px、裁云不同），
    # 实测反推把点击点算到 (1530,179)，而开关实际在 (996,106)——差距全被误读成
    # “框架的 on_change 没接上”。
    metrics = call("metrics")["result"]
    scale = float(metrics.get("device_scale") or 1.0)
    print(f"[{name}] 真实窗口「{title}」 客户区 {client_size[0]}x{client_size[1]} · "
          f"物理 {metrics.get('physical_width')}x{metrics.get('physical_height')} · DPI {scale:.2f}")

    element, prop, locked_value = case["state_probe"]

    def locked():
        return call("get", {"id": element})["result"]["props"].get(prop) == locked_value

    def switch_bounds():
        return call("find", {"selector": "#" + case["switch"]})["result"]["matches"][0]["bounds"]

    failures = []
    if locked():
        raise SystemExit("起始状态已是 AI 接管——请用默认（人工主导）启动再跑")

    # ① 人点开关 → 进入 AI 接管
    sx, sy = click_at(hwnd, origin, switch_bounds(), scale)
    if not locked():
        failures.append("人点开关后没进入 AI 接管（鼠标点击没走 on_change）")
        print(f"  点开关 ({sx},{sy}) → 未切换  ✗")
    else:
        print(f"  点开关 ({sx},{sy}) → AI 接管  ✓")

    # ② 接管后编辑必须被拒、查看仍可用
    eid, action, arg = case["locked_edit"]
    if call("invoke", {"id": eid, "action": action, "argument": arg})["result"]["handled"]:
        failures.append(f"接管后 {action} 仍可用")
        print(f"  接管后 {action} → 仍可用  ✗")
    else:
        print(f"  接管后 {action} → 被拒  ✓")
    rid, raction, rarg = case["readonly"]
    params = {"id": rid, "action": raction}
    if rarg is not None:
        params["argument"] = rarg
    if not call("invoke", params)["result"]["handled"]:
        failures.append(f"接管后查看动作 {raction} 也被拦了（锁成死界面）")
        print(f"  接管后 {raction}（查看）→ 被拦  ✗")
    else:
        print(f"  接管后 {raction}（查看）→ 可用  ✓")

    # ③ 人再点一次 → 拿回主导
    sx, sy = click_at(hwnd, origin, switch_bounds(), scale)
    if locked():
        failures.append("人再点开关后没拿回主导")
        print(f"  再点开关 ({sx},{sy}) → 仍是接管  ✗")
    else:
        print(f"  再点开关 ({sx},{sy}) → 人工主导  ✓")
    if not call("invoke", {"id": eid, "action": action, "argument": arg})["result"]["handled"]:
        failures.append(f"拿回主导后 {action} 仍被拒")
        print(f"  拿回主导后 {action} → 仍被拒  ✗")
    else:
        print(f"  拿回主导后 {action} → 恢复  ✓")

    if failures:
        raise SystemExit("失败：" + "；".join(failures))
    print(f"\n[OK] {name}：人可自己切换主动控制/自动接管（真实鼠标）")


if __name__ == "__main__":
    main()
