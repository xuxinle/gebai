#!/usr/bin/env python3
"""在 gdb 下启动应用并做一次 hello，崩溃时打印 C++ 回溯（定位无头控制通道崩溃）。"""
import json
import os
import socket
import struct
import subprocess
import sys
import time

ROOT = "/workspace/gebai/shuangtian"
app = sys.argv[1] if len(sys.argv) > 1 else "mdeditor"
ctl = f"/tmp/st-visual/gdb-{app}-control.json"
for path in (ctl, f"/tmp/st-visual/gdb-{app}.log"):
    if os.path.exists(path):
        os.remove(path)

gdb = subprocess.Popen(
    ["gdb", "-q", "-batch",
     "-ex", "set pagination off",
     "-ex", f"run --headless --control-port 0 --control-file {ctl}",
     "-ex", "bt 25",
     "-ex", "info threads",
     "--args", f"{ROOT}/build/dev/bin/{app}"],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True)

port = None
for _ in range(120):
    if os.path.exists(ctl):
        try:
            info = json.load(open(ctl))
            if info.get("port"):
                port = info["port"]
                break
        except Exception:
            pass
    time.sleep(0.25)
print(f"port={port}")


def call(method, params=None, timeout=20):
    sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    body = json.dumps({"id": 1, "method": method, "params": params or {}}).encode()
    sock.sendall(struct.pack(">I", len(body)) + body)
    header = b""
    while len(header) < 4:
        chunk = sock.recv(4 - len(header))
        if not chunk:
            raise RuntimeError("对端关闭")
        header += chunk
    (length,) = struct.unpack(">I", header)
    payload = b""
    while len(payload) < length:
        payload += sock.recv(length - len(payload))
    sock.close()
    return json.loads(payload)


for method, params in [("ping", {}), ("hello", {}), ("metrics", {}), ("tree", {"depth": 1})]:
    try:
        print(f"  {method}: {json.dumps(call(method, params), ensure_ascii=False)[:150]}")
    except Exception as error:  # noqa: BLE001
        print(f"  {method}: 失败 {error}")
        break

time.sleep(1.5)
if gdb.poll() is None:
    print("gdb 仍在运行 → 进程未崩溃，主动终止")
    gdb.terminate()
out, _ = gdb.communicate(timeout=30)
print("=== gdb 输出 ===")
print("\n".join(out.splitlines()[-40:]))
