"""gallery 屋架截图（WindowFrame 换壳后取证）。

用 `--frames` 让应用自己退出（不依赖客户端 quit 路径），截图经控制通道 `capture`。
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, "tools")
from st_client_lib import call_with_token, load_control

CTL = os.path.join("build", "final-shots", "frame-ctl.json")
SHOT = os.path.join("build", "final-shots", "gallery-windowframe.png")
os.makedirs(os.path.dirname(CTL), exist_ok=True)
if os.path.exists(CTL):
    os.remove(CTL)

proc = subprocess.Popen(["build/dev/bin/gallery.exe", "--headless", "--control-file", CTL,
                         "--frames", "400"])
info = {}
for _ in range(80):
    time.sleep(0.2)
    info = load_control(CTL)
    if info.get("port"):
        break
port, token = info.get("port"), info.get("token", "")
assert port, "控制通道未就绪"
try:
    reply = call_with_token(port, "capture", {"encode": "file", "path": SHOT}, token)
    print("截图:", (reply.get("result") or {}).get("path", reply))
    for element_id in ("app-root", "titlebar", "app-title", "theme-toggle", "dpi-toggle",
                       "btn-capture", "content-root", "nav-components"):
        reply = call_with_token(port, "get", {"id": element_id}, token)
        result = reply.get("result") or {}
        print(f"  #{element_id}: type={result.get('type')} bounds={result.get('bounds')}")
finally:
    try:
        proc.wait(timeout=40)
    except subprocess.TimeoutExpired:
        proc.kill()
        print("!! 应用未按时退出")
