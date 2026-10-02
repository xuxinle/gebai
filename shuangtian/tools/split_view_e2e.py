#!/usr/bin/env python3
"""SplitView 端到端验证（codeeditor 真实应用）：拖拽分栏 + 截图留证。

验证：
  1. 侧栏宽度符合初始比例
  2. 拖拽手柄 → 侧栏宽度变化（真实鼠标事件）
  3. 拖到超范围 → 夹取到 min_ratio
  4. 键盘/动作面（invoke step_forward）
  5. 截图（三态：初始/加宽/收窄）

用法: python3 tools/split_view_e2e.py <binary> <ctl.json> <port> <shots-dir>
"""
import json
import os
import socket
import struct
import subprocess
import sys
import time


def read_frame(sock, timeout=10.0):
    sock.settimeout(timeout)
    header = b""
    while len(header) < 4:
        chunk = sock.recv(4 - len(header))
        if not chunk:
            raise RuntimeError("连接关闭")
        header += chunk
    (length,) = struct.unpack(">I", header)
    payload = b""
    while len(payload) < length:
        chunk = sock.recv(length - len(payload))
        if not chunk:
            raise RuntimeError("响应截断")
        payload += chunk
    return json.loads(payload)


def call(sock, rid, method, params, token):
    if method == "hello" and token:
        params = dict(params, token=token)
    body = json.dumps({"id": rid, "method": method, "params": params}).encode()
    sock.sendall(struct.pack(">I", len(body)) + body)
    return read_frame(sock)


def main():
    binary, control_file, port, shots = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
    os.makedirs(shots, exist_ok=True)
    if os.path.exists(control_file):
        os.remove(control_file)
    proc = subprocess.Popen([binary, "--headless", "--control-port", str(port),
                             "--control-file", control_file, "--ms", "40000"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rid = [0]

    def nid():
        rid[0] += 1
        return rid[0]

    try:
        for _ in range(150):
            if os.path.exists(control_file):
                break
            time.sleep(0.1)
        info = json.load(open(control_file))
        token = info.get("token", "")
        sock = socket.create_connection(("127.0.0.1", info["port"]), timeout=15)
        assert call(sock, nid(), "hello", {}, token).get("ok")
        call(sock, nid(), "wait", {"for": "frames", "frames": 3, "timeout_ms": 5000}, token)

        def get(id_):
            return call(sock, nid(), "get", {"id": id_}, token)["result"]

        def prop(id_, name):
            props = get(id_)["props"]
            return props.get(name)

        # ① 初始比例（代码里 set_ratio(0.22)）
        split = get("sidebar-split")
        print(f"[0] split 属性: {split['props']}")
        ratio0 = float(prop("sidebar-split", "ratio"))
        sidebar0 = get("sidebar")["bounds"]["width"]
        split_w = split["bounds"]["width"]
        assert abs(ratio0 - 0.22) < 0.001, ratio0
        expected = (split_w - 8.0) * 0.22
        assert abs(sidebar0 - expected) < 2.0, (sidebar0, expected)
        print(f"[1] 初始侧栏宽 {sidebar0:.0f}px（比例 {ratio0}，总宽 {split_w:.0f}）")

        call(sock, nid(), "capture", {"encode": "file",
                                     "path": os.path.join(shots, "split-1-initial.png")}, token)

        # ② 拖拽手柄：从手柄中心拖到 split 左侧 + 60% 处
        handle = call(sock, nid(), "get", {"id": "sidebar-split"}, token)["result"]["bounds"]
        # 手柄 x 位置：split.x + (split.w-8)*0.22 + 4
        handle_x = handle["x"] + (handle["width"] - 8.0) * ratio0 + 4.0
        handle_y = handle["y"] + handle["height"] / 2
        target_x = handle["x"] + (handle["width"] - 8.0) * 0.6 + 4.0
        call(sock, nid(), "input.mouse", {"kind": "drag", "x": handle_x, "y": handle_y,
                                          "to_x": target_x, "to_y": handle_y}, token)
        call(sock, nid(), "wait", {"for": "frames", "frames": 3, "timeout_ms": 5000}, token)
        ratio1 = float(prop("sidebar-split", "ratio"))
        assert abs(ratio1 - 0.6) < 0.02, ratio1
        sidebar1 = get("sidebar")["bounds"]["width"]
        assert sidebar1 > sidebar0, (sidebar0, sidebar1)
        print(f"[2] 拖拽后侧栏 {sidebar0:.0f}px → {sidebar1:.0f}px（比例 {ratio1:.2f}）")
        call(sock, nid(), "capture", {"encode": "file",
                                     "path": os.path.join(shots, "split-2-dragged.png")}, token)

        # ③ 拖到超范围 → 夹取
        call(sock, nid(), "input.mouse", {"kind": "drag", "x": target_x, "y": handle_y,
                                          "to_x": handle["x"] - 500, "to_y": handle_y}, token)
        call(sock, nid(), "wait", {"for": "frames", "frames": 3, "timeout_ms": 5000}, token)
        ratio2 = float(prop("sidebar-split", "ratio"))
        min_ratio = float(prop("sidebar-split", "min_ratio"))
        assert abs(ratio2 - min_ratio) < 0.01, (ratio2, min_ratio)
        print(f"[3] 拖出左边界被夹取到 min_ratio={min_ratio}（比例 {ratio2:.2f}）")

        # ④ 动作面：step_forward 恢复
        call(sock, nid(), "invoke", {"id": "sidebar-split", "action": "step_forward"}, token)
        ratio3 = float(prop("sidebar-split", "ratio"))
        assert ratio3 > ratio2, (ratio2, ratio3)
        print(f"[4] invoke step_forward: {ratio2:.2f} → {ratio3:.2f}")

        # ⑤ 回到 0.5 收尾截图（手性视觉核验：分界线在正中）
        call(sock, nid(), "invoke", {"id": "sidebar-split", "action": "set",
                                     "argument": "0.5"}, token)
        call(sock, nid(), "wait", {"for": "frames", "frames": 3, "timeout_ms": 5000}, token)
        ratio4 = float(prop("sidebar-split", "ratio"))
        assert abs(ratio4 - 0.5) < 0.001, ratio4
        print(f"[5] invoke set 0.5 → 比例 {ratio4}")
        call(sock, nid(), "capture", {"encode": "file",
                                     "path": os.path.join(shots, "split-3-centered.png")}, token)

        print(f"\n[OK] SplitView 端到端全部通过（截图在 {shots}）")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    raise SystemExit(main())
