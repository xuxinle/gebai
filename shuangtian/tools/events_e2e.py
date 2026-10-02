#!/usr/bin/env python3
"""事件流端到端验证（真实 gallery 应用 + 真实 TCP 控制通道）。

验证：
  1. 订阅 ui.changed 后，set 一个元素 → 收到带 changed 清单的事件（含目标 id）
  2. invoke 动作 → 同样进清单
  3. 事件帧的 version 单调递增

用法: python3 tools/events_e2e.py <binary> <ctl.json> <port>
"""
import json
import os
import socket
import struct
import subprocess
import sys
import time


def call(sock, request_id, method, params, token):
    if method == "hello" and token:
        params = dict(params, token=token)
    body = json.dumps({"id": request_id, "method": method, "params": params}).encode()
    sock.sendall(struct.pack(">I", len(body)) + body)
    return read_frame(sock)


def read_frame(sock, timeout=5.0):
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


def main():
    binary, control_file, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
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

        # 订阅连接
        sub = socket.create_connection(("127.0.0.1", info["port"]), timeout=10)
        hello = call(sub, 0, "hello", {"subscribe": True, "kinds": ["ui.changed"]}, token)
        assert hello.get("ok"), hello
        print("[0] 订阅连接已握手（kinds=ui.changed）")

        # 操作连接
        ctl = socket.create_connection(("127.0.0.1", info["port"]), timeout=10)
        assert call(ctl, nid(), "hello", {}, token).get("ok")
        call(ctl, nid(), "wait", {"for": "frames", "frames": 2, "timeout_ms": 5000}, token)

        # 找一个可改元素
        found = call(ctl, nid(), "find", {"selector": "Input", "limit": 1}, token)
        input_id = found["result"]["matches"][0]["id"]

        def collect_events(expected_id, action):
            """执行 action 后收集事件，返回带 changed 清单的第一帧。"""
            action()
            deadline = time.time() + 3.0
            while time.time() < deadline:
                try:
                    frame = read_frame(sub, timeout=max(0.1, deadline - time.time()))
                except socket.timeout:
                    break
                if "event" not in frame:
                    continue  # 响应帧
                data = frame.get("data", {})
                if expected_id in data.get("changed", []):
                    return frame
            return None

        # ① set → 事件带 changed 清单
        frame1 = collect_events(
            input_id,
            lambda: call(ctl, nid(), "set", {"id": input_id, "props": {"value": "事件测试"}}, token))
        assert frame1 is not None, f"set 后未收到带 {input_id} 的 ui.changed"
        assert frame1["event"] == "ui.changed"
        assert isinstance(frame1["data"]["version"], int)
        print(f"[1] set 后收到 ui.changed: changed={frame1['data']['changed']}, "
              f"version={frame1['data']['version']}")

        # ② invoke 动作 → 同样进清单（找一个 Button）
        found_btn = call(ctl, nid(), "find", {"selector": "Button", "limit": 1}, token)
        btn_id = found_btn["result"]["matches"][0]["id"]
        frame2 = collect_events(
            btn_id,
            lambda: call(ctl, nid(), "invoke", {"id": btn_id, "action": "focus"}, token))
        assert frame2 is not None, f"invoke 后未收到带 {btn_id} 的 ui.changed"
        assert frame2["data"]["version"] > frame1["data"]["version"], "version 未递增"
        print(f"[2] invoke 后收到 ui.changed: changed={frame2['data']['changed']}, "
              f"version={frame2['data']['version']}（已递增）")

        # ③ 输入事件（input.text）→ 焦点元素进清单
        call(ctl, nid(), "invoke", {"id": input_id, "action": "focus"}, token)
        time.sleep(0.1)
        frame3 = collect_events(
            input_id,
            lambda: call(ctl, nid(), "input.text", {"text": "abc"}, token))
        assert frame3 is not None, "input.text 后未收到带输入框的 ui.changed"
        print(f"[3] input.text 后收到 ui.changed: changed={frame3['data']['changed']}")

        print("\n[OK] 事件流端到端全部通过（changed 清单 + version 递增）")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    raise SystemExit(main())
