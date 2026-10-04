"""gallery 组件页截图（窗框巡检卡落地取证）。

用 `--frames` 让应用自己退出（不依赖客户端 quit 路径），截图经控制通道 `capture`。
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, "tools")
from st_client_lib import call_with_token, load_control

CTL = os.path.join("build", "final-shots", "gallery-ctl.json")
SHOT = os.path.join("build", "final-shots", "gallery-components.png")
os.makedirs(os.path.dirname(CTL), exist_ok=True)
if os.path.exists(CTL):
    os.remove(CTL)

proc = subprocess.Popen(["build/debug/bin/gallery.exe", "--headless", "--control-file", CTL,
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
    # 先切到「组件」页（窗框巡检卡在那里）——概览页看不到它。
    call_with_token(port, "invoke", {"id": "nav-components", "action": "click"}, token)
    time.sleep(1.0)
    # 窗框巡检卡在「组件」页较下方（卡片很多）：用滚轮滚到底再截图。
    # 注：`set` 的 `scroll_offset` 在 `ScrollView` 上没有生效（其 `property_names`
    # 不含它）——这条踩坑记录就留在注释里：写"看起来对"的属性不报错、也不生效。
    for _ in range(7):
        call_with_token(port, "input.mouse", {"kind": "scroll", "x": 700, "y": 400, "delta": -3}, token)
        time.sleep(0.12)
    time.sleep(0.8)
    reply = call_with_token(port, "capture", {"encode": "file", "path": SHOT}, token)
    print("截图:", (reply.get("result") or {}).get("path", reply))
    snap = call_with_token(port, "get", {"id": "demo-titlebar"}, token).get("result", {})
    print("demo-titlebar 类型:", snap.get("type"))
    print("窗框巡检卡存在:", "card-titlebar" in str(call_with_token(port, "find", {"selector": "#card-titlebar"}, token)))
finally:
    try:
        proc.wait(timeout=40)
    except subprocess.TimeoutExpired:
        proc.kill()
        print("!! 应用未按时退出")
