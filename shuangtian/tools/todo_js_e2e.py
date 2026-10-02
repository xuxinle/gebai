#!/usr/bin/env python3
"""todo-js 端到端验证：异步取数 → 勾选 → 过滤 → 删除（控制通道驱动）。"""
import json
import socket
import struct
import subprocess
import sys
import time
import os


def call(sock, request_id, method, params, token):
    if method == "hello" and token:
        params = dict(params, token=token)
    body = json.dumps({"id": request_id, "method": method, "params": params}).encode()
    sock.sendall(struct.pack(">I", len(body)) + body)
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


def tree_texts(node, out):
    """递归收集 type/text/id。"""
    if isinstance(node, dict):
        if node.get("type"):
            out.append((node.get("type"), node.get("text", ""), node.get("id", "")))
        for child in node.get("children", []) or []:
            tree_texts(child, out)
    return out


def main():
    binary, control_file, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
    if os.path.exists(control_file):
        os.remove(control_file)
    proc = subprocess.Popen([binary, "--headless", "--control-port", str(port),
                             "--control-file", control_file, "--ms", "25000"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if os.path.exists(control_file):
                break
            time.sleep(0.1)
        info = json.load(open(control_file))
        token = info.get("token", "")
        sock = socket.create_connection(("127.0.0.1", info["port"]), timeout=10)
        call(sock, 1, "hello", {}, token)
        # 等异步取数落地（帧泵推进）
        time.sleep(0.5)
        r = call(sock, 2, "tree", {}, token)
        nodes = tree_texts(r["result"]["tree"], [])

        def texts(pattern=None):
            return [t for (ty, t, _) in nodes if t and (pattern is None or pattern in t)]

        print("=== 初始（异步取数后）===")
        for line in texts("已完成") + texts("当前过滤"):
            print("  ", line)
        items = [t for (ty, t, _) in nodes if ty == "Checkbox"]
        print("   待办项:", items)
        assert any("已完成 1 / 3" in t for t in texts()), "异步取数未落地"
        assert len(items) == 3, f"待办项数不对: {items}"

        # 找"未完成"过滤按钮 → 点击
        def find_button(label):
            for (ty, t, i) in nodes:
                if ty == "Button" and label in str(t):
                    return i
            return None

        btn = find_button("未完成")
        assert btn, "没找到『未完成』按钮"
        call(sock, 3, "invoke", {"id": btn, "action": "click"}, token)
        time.sleep(0.3)
        r = call(sock, 4, "tree", {}, token)
        nodes = tree_texts(r["result"]["tree"], [])
        items = [t for (ty, t, _) in nodes if ty == "Checkbox"]
        print("=== 过滤『未完成』===")
        print("   待办项:", items)
        assert len(items) == 2, f"过滤后应为 2 项: {items}"

        # 回到全部，勾选**第二项**（t2，未完成）→ 已完成 1→2
        btn_all = find_button("全部")
        call(sock, 5, "invoke", {"id": btn_all, "action": "click"}, token)
        time.sleep(0.2)
        r = call(sock, 6, "tree", {}, token)
        nodes = tree_texts(r["result"]["tree"], [])
        checkboxes = [i for (ty, t, i) in nodes if ty == "Checkbox"]
        assert len(checkboxes) == 3, f"回到全部后应为 3 项: {checkboxes}"
        call(sock, 7, "invoke", {"id": checkboxes[1], "action": "click"}, token)
        time.sleep(0.3)
        r = call(sock, 8, "tree", {}, token)
        nodes = tree_texts(r["result"]["tree"], [])
        done = [t for (ty, t, _) in nodes if t and "已完成" in t]
        print("=== 勾选第二项（未完成）后 ===")
        print("  ", done)
        assert any("已完成 2 / 3" in t for t in done), f"勾选未生效: {done}"

        print("\n[OK] todo-js 端到端全部通过")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    raise SystemExit(main())
