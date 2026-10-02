#!/usr/bin/env python3
"""打开命令面板后截图（验证 overlay 视觉）。

用法: python3 tools/st_shoot_palette.py <binary> <out.png> <port>
"""
import json
import os
import socket
import struct
import subprocess
import sys
import time


def call(sock, method, params, token):
    if method == "hello" and token:
        params = dict(params, token=token)
    body = json.dumps({"id": 1, "method": method, "params": params}).encode()
    sock.sendall(struct.pack(">I", len(body)) + body)
    header = b""
    while len(header) < 4:
        chunk = sock.recv(4 - len(header))
        if not chunk:
            raise RuntimeError("关闭")
        header += chunk
    (length,) = struct.unpack(">I", header)
    payload = b""
    while len(payload) < length:
        payload += sock.recv(length - len(payload))
    return json.loads(payload)


def flat(node, out=None):
    if out is None:
        out = []
    if isinstance(node, dict):
        if node.get("type"):
            out.append(node)
        for child in node.get("children", []) or []:
            flat(child, out)
    return out


def main():
    binary, out_png, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
    ctl = f"/tmp/pal_{port}.json"
    if os.path.exists(ctl):
        os.remove(ctl)
    proc = subprocess.Popen([binary, "--headless", "--control-port", str(port),
                             "--control-file", ctl, "--ms", "25000"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(150):
            if os.path.exists(ctl):
                break
            time.sleep(0.1)
        info = json.load(open(ctl))
        token = info.get("token", "")
        sock = socket.create_connection(("127.0.0.1", info["port"]), timeout=5)
        call(sock, "hello", {}, token)
        time.sleep(0.8)
        # 打开菜单 → 点第一项（新建文件 → 命令面板）
        nodes = flat(call(sock, "tree", {}, token)["result"]["tree"])
        mb = next(n for n in nodes if str(n.get("id", "")).endswith("menubar"))
        call(sock, "invoke", {"id": mb["id"], "action": "click"}, token)
        time.sleep(0.6)
        nodes = flat(call(sock, "tree", {}, token)["result"]["tree"])
        panel = next(n for n in nodes if n.get("type") == "MenuPanel")
        bounds = panel["bounds"]
        call(sock, "input.mouse", {"kind": "click", "x": bounds["x"] + 40,
                                   "y": bounds["y"] + 22, "button": 1, "click_count": 1}, token)
        time.sleep(0.8)
        reply = call(sock, "capture", {"encode": "file", "path": out_png}, token)
        if not reply.get("ok"):
            raise RuntimeError(reply)
        print(f"[OK] {out_png}")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    raise SystemExit(main())
