#!/usr/bin/env python3
"""在 gdb 下启动应用并做一次 hello，崩溃时打印 C++ 回溯（定位无头控制通道崩溃）。"""
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

import st_client_lib  # 本地模块（同目录）：带 token/握手的单次调用

ROOT = os.environ.get("SHUANGTIAN_ROOT", os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
PROFILE = os.environ.get("ST_PROFILE", "dev")
SHOTS = os.environ.get("SHUANGTIAN_SHOTS", os.path.join(tempfile.gettempdir(), "st-visual"))
os.makedirs(SHOTS, exist_ok=True)
app = sys.argv[1] if len(sys.argv) > 1 else "mdeditor"
ctl = f"{SHOTS}/gdb-{app}-control.json"
for path in (ctl, f"{SHOTS}/gdb-{app}.log"):
    if os.path.exists(path):
        os.remove(path)

gdb = subprocess.Popen(
    ["gdb", "-q", "-batch",
     "-ex", "set pagination off",
     "-ex", f"run --headless --control-port 0 --control-file {ctl}",
     "-ex", "bt 25",
     "-ex", "info threads",
     "--args", f"{ROOT}/build/{PROFILE}/bin/{app}"],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True)

port = None
TOKEN = ""
for _ in range(120):
    if os.path.exists(ctl):
        try:
            info = json.load(open(ctl))
            if info.get("port"):
                port = info["port"]
                TOKEN = info.get("token", "")
                break
        except Exception:
            pass
    time.sleep(0.25)
print(f"port={port}")


def call(method, params=None, timeout=20):
    # 每调用一条连接：非 hello/ping 由公共库自动前置握手（hello 门 + token）
    return st_client_lib.call_with_token(port, method, params, TOKEN, timeout)


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
