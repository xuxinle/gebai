#!/usr/bin/env python3
"""截图工具：启动应用 → 等就绪 → capture 落盘 → 退出。

用法: python3 tools/st_shoot.py <binary> <out.png> <port> [额外参数...]
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
    binary, out_png, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
    extra = sys.argv[4:]
    ctl = f"/tmp/st_shoot_{port}.json"
    if os.path.exists(ctl):
        os.remove(ctl)
    args = [binary, "--headless", "--control-port", str(port), "--control-file", ctl,
            "--ms", "20000"] + extra
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(150):
            if os.path.exists(ctl):
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("控制文件未生成")
        info = json.load(open(ctl))
        token = info.get("token", "")
        # 重试连接（服务刚监听时可能还没 accept）
        sock = None
        for _ in range(30):
            try:
                sock = socket.create_connection(("127.0.0.1", info["port"]), timeout=5)
                break
            except OSError:
                time.sleep(0.2)
        if sock is None:
            raise RuntimeError("连不上控制通道")
        call(sock, "hello", {}, token)
        time.sleep(1.0)
        reply = call(sock, "capture", {"encode": "file", "path": out_png}, token)
        if not reply.get("ok"):
            raise RuntimeError(f"capture 失败: {reply}")
        print(f"[OK] {out_png} ({reply['result'].get('bytes', '?')} 字节)")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    raise SystemExit(main())
