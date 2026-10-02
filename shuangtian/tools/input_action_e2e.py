#!/usr/bin/env python3
"""Input 单行动作面端到端验证（codeeditor 终端输入框，真实控制通道）。

验证（DESIGN §8.1.1「API 存在但动作面未实现」同族缺口的修复）：
  1. `invoke submit` 触发 on_submit（终端输出更新——不经键盘回车）
  2. `invoke clear` 清空输入框
  3. 未知动作被协议白名单拒绝（unsupported，而不是假成功）

用法: python3 tools/input_action_e2e.py <binary> <ctl.json> <port>
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
    binary, control_file, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
    if os.path.exists(control_file):
        os.remove(control_file)
    proc = subprocess.Popen([binary, "--headless", "--control-port", str(port),
                             "--control-file", control_file, "--ms", "30000"],
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

        def term_text():
            props = call(sock, nid(), "get", {"id": "terminal-output"}, token)["result"]["props"]
            return props.get("text", props.get("content", ""))

        # ① submit（不经键盘 Enter）：设置值 → invoke submit → 终端输出更新
        before = term_text()
        call(sock, nid(), "set", {"id": "terminal-input", "props": {"value": "help"}}, token)
        r = call(sock, nid(), "invoke", {"id": "terminal-input", "action": "submit"}, token)
        assert r["result"]["handled"] is True, r
        call(sock, nid(), "wait", {"for": "frames", "frames": 3, "timeout_ms": 5000}, token)
        after = term_text()
        assert after != before and "help" in after, "submit 后终端输出未更新"
        print("[1] invoke submit：终端输出已更新（on_submit 触发）")

        # ② clear
        call(sock, nid(), "set", {"id": "terminal-input", "props": {"value": "langs"}}, token)
        r2 = call(sock, nid(), "invoke", {"id": "terminal-input", "action": "clear"}, token)
        assert r2["result"]["handled"] is True, r2
        value = call(sock, nid(), "get", {"id": "terminal-input"}, token)["result"]["props"].get("value")
        assert value == "", value
        print("[2] invoke clear：输入框已清空")

        # ③ 未知动作 → unsupported
        r3 = call(sock, nid(), "invoke", {"id": "terminal-input", "action": "no_such_action"}, token)
        assert r3.get("error", {}).get("code") == "unsupported", r3
        print("[3] 未知动作被拒：unsupported（而非假成功）")

        print("\n[OK] Input 动作面端到端全部通过")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    raise SystemExit(main())
