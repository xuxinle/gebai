#!/usr/bin/env python3
"""按区域抓高清截图（DPI 2x）供细节核验：预览区 / 工具栏 / 源码视图 / 大纲。

用法: python3 st_shot_region.py <app> '<JSON 区域表>' [--profile dev] [--theme dark] [--language cpp]
      python3 st_shot_region.py gbcode '{"full":null,"editor":{"x":0,"y":60,"width":700,"height":500}}'
"""
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
SHOTS = os.environ.get("SHUANGTIAN_SHOTS", os.path.join(tempfile.gettempdir(), "st-visual")) + "/hi"
os.makedirs(SHOTS, exist_ok=True)
app = sys.argv[1] if len(sys.argv) > 1 else "gallery"
regions = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {"full": None}
profile = "dev"
theme = ""
language = ""
extra_args = sys.argv[3:]
for index, arg in enumerate(extra_args):
    if arg == "--profile" and index + 1 < len(extra_args):
        profile = extra_args[index + 1]
    if arg == "--theme" and index + 1 < len(extra_args):
        theme = extra_args[index + 1]
    if arg == "--language" and index + 1 < len(extra_args):
        language = extra_args[index + 1]

suffix = ""
if theme:
    suffix += f"-{theme}"
if language:
    suffix += f"-{language}"
tag = f"{app}{suffix}"
ctl = f"{SHOTS}/hi-{tag}-control.json"
if os.path.exists(ctl):
    os.remove(ctl)
log_path = f"{SHOTS}/hi-{tag}.log"
log = open(log_path, "wb")
command = [f"{ROOT}/build/{profile}/bin/{app}", "--headless", "--control-port", "0",
           "--control-file", ctl, "--shots", SHOTS]
if theme:
    command += ["--theme", theme]
if language:
    command += ["--language", language]
process = subprocess.Popen(command, stdout=log, stderr=log, stdin=subprocess.DEVNULL,
                           start_new_session=True)
port = 0
TOKEN = ""
for _ in range(160):
    if os.path.exists(ctl):
        try:
            info = json.load(open(ctl))
            if info.get("port"):
                port = info["port"]
                TOKEN = info.get("token", "")
                break
        except Exception:
            pass
    if process.poll() is not None:
        print(f"进程提前退出（code={process.returncode}）：{open(log_path).read()[-800:]}")
        sys.exit(1)
    time.sleep(0.25)
if port == 0:
    print("未取得控制端口")
    sys.exit(1)
print(f"[{tag}] port={port}")


def call(method, params=None, timeout=120):
    # 每调用一条连接：非 hello/ping 由公共库自动前置握手（hello 门 + token）
    return st_client_lib.call_with_token(port, method, params, TOKEN, timeout)


call("hello")  # 鉴权门：hello 之前只允许 ping/hello 本身
call("app", {"action": "set_scale", "scale": 2.0})
for name, region in regions.items():
    path = f"{SHOTS}/{tag}-{name}.png"
    params = {"encode": "file", "path": path}
    if region:
        params["region"] = region
    reply = call("capture", params)
    saved = reply.get("result", {}).get("path", "")
    size = os.path.getsize(saved) if saved and os.path.exists(saved) else 0
    print(f"  {name}: {saved} {size} bytes")
call("shutdown")
time.sleep(0.3)
if process.poll() is None:
    process.terminate()
