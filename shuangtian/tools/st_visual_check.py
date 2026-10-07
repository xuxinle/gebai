#!/usr/bin/env python3
"""霜天视觉与稳定性验证：
① dev 档跑完整交互序列（查询/操作/输入/DPI 切换/主题切换）；
② san 档（ASan+UBSan）跑同一序列，确认零 sanitizer 报告；
③ 产出多张截图供人眼核验（DPI 1x/2x、亮/暗主题、gallery 与 gbcode）。
"""
import glob
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

import st_client_lib  # 本地模块（同目录）：带 token/握手的单次调用

# 根目录可参数化（默认：脚本所在目录的上一级），Windows/Linux 通用；SHUANGTIAN_ROOT 可覆盖。
ROOT = os.environ.get("SHUANGTIAN_ROOT", os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
SHOTS = os.environ.get("SHUANGTIAN_SHOTS", os.path.join(tempfile.gettempdir(), "st-visual"))
_TOKENS: dict[int, str] = {}  # port → token（控制文件读出；hello 必须携带）
os.makedirs(SHOTS, exist_ok=True)


def launch(profile: str, app: str) -> tuple[int, subprocess.Popen]:
    ctl = f"{SHOTS}/{profile}-{app}-control.json"
    log_path = f"{SHOTS}/{profile}-{app}.log"
    if os.path.exists(ctl):
        os.remove(ctl)
    log = open(log_path, "wb")
    command = [f"{ROOT}/build/{profile}/bin/{app}", "--headless", "--control-port", "0",
               "--control-file", ctl, "--shots", SHOTS]
    # 渲染器可用环境变量钉住（默认 auto）：验证软件腿的**增量重绘**路径时传 software。
    renderer = os.environ.get("ST_VISUAL_RENDERER", "")
    if renderer:
        command.extend(["--renderer", renderer])
    if app == "gbcode":
        # 脚本能力默认关闭；gbcode 支持 `--enable-script`，这里显式开启以便覆盖该路径
        command.append("--enable-script")
    process = subprocess.Popen(command, stdout=log, stderr=log, stdin=subprocess.DEVNULL,
                               start_new_session=True)
    for _ in range(160):
        if os.path.exists(ctl):
            try:
                info = json.load(open(ctl))
                if info.get("port"):
                    _TOKENS[info["port"]] = info.get("token", "")
                    return info["port"], process
            except Exception:
                pass
        if process.poll() is not None:
            # 日志按 UTF-8 读（子进程输出为 UTF-8；默认编码在中文 Windows 上是 GBK，
            # 此前这里一读就 UnicodeDecodeError，把真正的启动错误盖掉了）
            text = open(log_path, encoding="utf-8", errors="replace").read()[-600:]
            print(f"  进程提前退出（code={process.returncode}）：{text}")
            return 0, process
        time.sleep(0.25)
    return 0, process


def call(port: int, method: str, params=None, timeout=90):
    # 每调用一条连接：非 hello/ping 由公共库自动前置握手（hello 门 + token）
    return st_client_lib.call_with_token(port, method, params, _TOKENS.get(port, ""), timeout)


def sequence(profile: str, app: str, shots: list[str]) -> int:
    port, process = launch(profile, app)
    if port == 0:
        return 1
    failures = 0
    try:
        for method, params in [("hello", {}), ("metrics", {})]:
            call(port, method, params)
        steps = [
            ("tree", {"depth": 2}),
            ("find", {"selector": "Button", "limit": 50}),
            ("visual", {}),
            ("capture", {"encode": "file", "path": f"{SHOTS}/{app}-{profile}-light.png"}),
            ("invoke", {"id": {"gallery": "btn-submit", "gbcode": "btn-theme"}[app],
                        "action": "click"}),
            ("input.mouse", {"kind": "click", "x": 420, "y": 720}),
            ("input.text", {"text": "霜天 · DPI 与流式",
                            "id": {"gallery": "input-search", "gbcode": "editor"}[app]}),
            ("wait", {"for": "stable", "timeout_ms": 2000}),
            # 脚本路径（仅 gbcode 开启）：读界面 → 改界面 → 绑定事件 → 触发 → 读回状态。
            # 放在"主题切换"之前：改完文本紧接着截图，人眼能立刻确认脚本真的生效了。
            *([("script", {"code": "$('#status').set({text: '脚本已驱动界面 ✓'}); "
                                   "$('#editor').props.language"})] if app == "gbcode" else []),
            *([("script", {"selector": "#btn-theme", "event": "click",
                           "on": "() => $('#status').set({text:'JS 捕获了点击 ✓'})"})]
              if app == "gbcode" else []),
            *([("invoke", {"id": "btn-theme", "action": "click"})] if app == "gbcode" else []),
            *([("get", {"id": "status"})] if app == "gbcode" else []),
            *([("script", {"state": True})] if app == "gbcode" else []),
            ("theme", {"mode": "dark"}),
            ("capture", {"encode": "file", "path": f"{SHOTS}/{app}-{profile}-dark.png"}),
            ("app", {"action": "set_scale", "scale": 2.0}),
            ("capture", {"encode": "file", "path": f"{SHOTS}/{app}-{profile}-dpi2.png"}),
            ("metrics", {}),
        ]
        for method, params in steps:
            try:
                reply = call(port, method, params)
                ok = reply.get("ok")
                if not ok:
                    print(f"    {method:11} !! {json.dumps(reply.get('error'), ensure_ascii=False)[:110]}")
                    failures += 1
            except Exception as error:  # noqa: BLE001
                print(f"    {method:11} !! 异常 {error}")
                failures += 1
                break
        metrics = call(port, "metrics")["result"]
        print(f"    metrics: backend={metrics['backend']} scale={metrics['device_scale']} "
              f"phys={metrics['physical_width']}x{metrics['physical_height']} frames={metrics['frames']} "
              f"p50={metrics.get('frame_p50_ms', 0):.1f}ms nodes={metrics['nodes']}")
        call(port, "shutdown")
        shots.extend(sorted(glob.glob(f"{SHOTS}/{app}-{profile}-*.png")))
    finally:
        time.sleep(0.4)
        if process.poll() is None:
            process.terminate()
    return failures


total = 0
shots: list[str] = []
for profile, note in [("dev", "常规档"), ("san", "ASan+UBSan")]:
    for app in ("gallery", "gbcode"):
        print(f"[{profile}/{app}] {note}")
        total += sequence(profile, app, shots)

print(f"\n失败步数: {total}")
print("截图：")
for path in shots:
    print(f"  {path}  {os.path.getsize(path)} bytes")

print("\n=== sanitizer 报告检查 ===")
for path in sorted(glob.glob(f"{SHOTS}/san-*.log")):
    text = open(path, encoding="utf-8", errors="replace").read()
    hits = [line for line in text.splitlines() if "AddressSanitizer" in line or "runtime error" in line or "LeakSanitizer" in line]
    print(f"  {os.path.basename(path)}: {'、'.join(hits) if hits else '无 sanitizer 报告'}")
sys.exit(0 if total == 0 else 1)
