#!/usr/bin/env python3
"""按精确顺序复现控制通道交互序列，定位崩溃点。"""
import json
import os
import socket
import struct
import subprocess
import time

ROOT = "/workspace/gebai/shuangtian"
SHOTS = "/tmp/st-visual"
os.makedirs(SHOTS, exist_ok=True)
app = os.environ.get("ST_APP", "mdeditor")
ctl = f"{SHOTS}/seq-{app}-control.json"
if os.path.exists(ctl):
    os.remove(ctl)
log = open(f"/tmp/st-visual/seq-{app}.log", "wb")
subprocess.Popen([f"{ROOT}/build/dev/bin/{app}", "--headless", "--control-port", "0",
                  "--control-file", ctl, "--shots", SHOTS],
                 stdout=log, stderr=log, stdin=subprocess.DEVNULL, start_new_session=True)
port = None
for _ in range(80):
    if os.path.exists(ctl):
        try:
            info = json.load(open(ctl))
            if info.get("port"):
                port = info["port"]
                break
        except Exception:
            pass
    time.sleep(0.25)
print(f"[{app}] port={port}")


def call(method, params=None, timeout=60):
    sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    body = json.dumps({"id": 1, "method": method, "params": params or {}}).encode()
    sock.sendall(struct.pack(">I", len(body)) + body)
    header = b""
    while len(header) < 4:
        chunk = sock.recv(4 - len(header))
        if not chunk:
            raise RuntimeError("连接被对端关闭（进程可能已崩溃）")
        header += chunk
    (length,) = struct.unpack(">I", header)
    payload = b""
    while len(payload) < length:
        chunk = sock.recv(length - len(payload))
        if not chunk:
            raise RuntimeError("响应截断（进程可能已崩溃）")
        payload += chunk
    sock.close()
    return json.loads(payload)


steps = [
    ("hello", {}),
    ("metrics", {}),
    ("tree", {"depth": 2}),
    ("find", {"selector": "Button", "limit": 50}),
    ("get", {"id": "editor"}),
    ("capture", {"encode": "file", "path": f"{SHOTS}/seq-{app}-1.png"}),
    ("invoke", {"id": "tool-bold", "action": "click"}),
    ("input.text", {"text": "DPI 与流式", "id": "editor"}),
    ("app", {"action": "set_scale", "scale": 2.0}),
    ("capture", {"encode": "file", "path": f"{SHOTS}/seq-{app}-2x.png"}),
    ("metrics", {}),
]
for method, params in steps:
    try:
        reply = call(method, params)
        body = reply.get("result", reply.get("error"))
        print(f"  {method:11} ok={reply.get('ok')} {json.dumps(body, ensure_ascii=False)[:120]}")
    except Exception as error:  # noqa: BLE001
        print(f"  {method:11} 失败: {error}")
        break
