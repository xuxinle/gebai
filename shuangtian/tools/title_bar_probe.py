"""窗框（自绘标题栏）控制在无头下的行为探针。

重点验证三件事（都是"跨平台无差异"的落点）：
1. `#titlebar` 是 `ui::TitleBar`（类型/属性面/动作面齐备）；
2. 无窗口（headless）时**动作如实拒绝**（返回 unsupported / ok=false），而不是静默成功；
3. 画面照旧（属性面可读 `window_control_available=false`，但仍画三个按钮）。

用 `--frames N` 让应用自己退出，避免客户端 quit 路径带来的不确定性。
"""
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, "tools")
from st_client_lib import call_with_token, load_control

BUILD = "build/debug/bin/codeeditor.exe"
CTL = os.path.join("build", "e2e-codeeditor", "ctl-titlebar.json")
os.makedirs(os.path.dirname(CTL), exist_ok=True)
if os.path.exists(CTL):
    os.remove(CTL)

proc = subprocess.Popen([BUILD, "--headless", "--control-file", CTL, "--frames", "600"])
info = {}
for _ in range(80):
    time.sleep(0.2)
    info = load_control(CTL)
    if info.get("port"):
        break
port, token = info.get("port"), info.get("token", "")
assert port, "控制通道未就绪"


def call(method, params=None):
    reply = call_with_token(port, method, params, token)
    return reply.get("result", reply)


try:
    print("titlebar 命中数:", call("find", {"selector": "#titlebar"})["count"])
    snapshot = call("get", {"id": "titlebar"})
    print("type:", snapshot.get("type"), "| role:", snapshot.get("role"))
    print("props:", json.dumps(snapshot.get("props", {}), ensure_ascii=False, sort_keys=True))
    print("bounds:", json.dumps(snapshot.get("bounds", {})))
    print("title 内容:", json.dumps(snapshot.get("props", {}).get("title", ""), ensure_ascii=False))
    for action in ("minimize", "maximize", "close"):
        reply = call_with_token(port, "invoke", {"id": "titlebar", "action": action}, token)
        print(f"invoke {action} ->", json.dumps(reply, ensure_ascii=False))
    print("旧 id #title-text 命中数:", call("find", {"selector": "#title-text"})["count"])
    shot = call("capture", {"encode": "file", "path": os.path.join("build", "e2e-codeeditor", "titlebar-headless.png")})
    print("截图:", shot.get("path") if isinstance(shot, dict) else shot)
finally:
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        print("!! 应用未按时退出（--frames 未生效）")
