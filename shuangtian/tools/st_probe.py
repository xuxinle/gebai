#!/usr/bin/env python3
"""按精确顺序复现控制通道交互序列，定位崩溃点。"""
import json
import os
import socket
import struct
import subprocess
import tempfile
import time

import st_client_lib  # 本地模块（同目录）：带 token/握手的单次调用

ROOT = os.environ.get("SHUANGTIAN_ROOT", os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
PROFILE = os.environ.get("ST_PROFILE", "dev")
SHOTS = os.environ.get("SHUANGTIAN_SHOTS", os.path.join(tempfile.gettempdir(), "st-visual"))
os.makedirs(SHOTS, exist_ok=True)
app = os.environ.get("ST_APP", "mdeditor")
ctl = f"{SHOTS}/seq-{app}-control.json"
if os.path.exists(ctl):
    os.remove(ctl)
log = open(f"{SHOTS}/seq-{app}.log", "wb")
subprocess.Popen([f"{ROOT}/build/{PROFILE}/bin/{app}", "--headless", "--control-port", "0",
                  "--control-file", ctl, "--shots", SHOTS],
                 stdout=log, stderr=log, stdin=subprocess.DEVNULL, start_new_session=True)
port = None
TOKEN = ""  # 鉴权 token（控制文件读出；hello 必须携带）
for _ in range(80):
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
print(f"[{app}] port={port}")


def call(method, params=None, timeout=60):
    # 每调用一条连接：非 hello/ping 由公共库自动前置握手（hello 门 + token）
    return st_client_lib.call_with_token(port, method, params, TOKEN, timeout)


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
