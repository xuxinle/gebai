#!/usr/bin/env python3
"""采集 gallery 的渐变样本截图（供改前/改后逐像素对比）。

用法：
    python tools/capture_gradient_baseline.py <输出目录>

产物（写到指定目录，**不要写进仓库**——可重建的视觉证据快照）：
    <目录>/dark-full.png        整帧（暗色）
    <目录>/light-full.png       整帧（亮色）
    <目录>/dark-primary.png     主按钮区域放大（Primary 变体 = 双色渐变底）
    <目录>/dark-card.png        渐变卡片区域

退出码 0 = 采到全部样本。
"""
import base64
import io
import json
import os
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import st_client_lib  # noqa: E402

PROFILE = os.environ.get("ST_PROFILE", "dev")


def start_app(workdir):
    exe = os.path.join(ROOT, "build", PROFILE, "bin", "gallery.exe")
    ctl = os.path.join(workdir, "control.json")
    log_path = os.path.join(workdir, "gallery.log")
    if os.path.exists(ctl):
        os.remove(ctl)
    log = open(log_path, "wb")
    process = subprocess.Popen([exe, "--headless", "--control-port", "0",
                                "--control-file", ctl],
                               stdout=log, stderr=log, stdin=subprocess.DEVNULL,
                               start_new_session=True)
    for _ in range(160):
        if os.path.exists(ctl):
            try:
                info = json.load(open(ctl))
                if info.get("port"):
                    return info["port"], info.get("token", ""), process
            except Exception:
                pass
        time.sleep(0.25)
    process.kill()
    raise SystemExit("控制通道没起来（看 %s）" % log_path)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)

    port, token, process = start_app(out)
    try:
        def call(method, params=None):
            return st_client_lib.call_with_token(port, method, params or {}, token)

        def grab(path, region=None):
            params = {}
            if region is not None:
                params["region"] = region
            result = call("capture", params).get("result") or {}
            with open(path, "wb") as handle:
                handle.write(base64.b64decode(result["base64"]))
            return result

        def find(selector, limit=1):
            reply = call("find", {"selector": selector, "limit": limit}).get("result") or {}
            return reply.get("matches") or []

        # 驱动几帧，确保稳态（首帧可能还在应用主题）。
        for _ in range(3):
            call("metrics")

        call("theme", {"mode": "dark"})
        for _ in range(3):
            call("metrics")
        grab(os.path.join(out, "dark-full.png"))

        # Primary 变体的按钮：词表给它铺 primary→accent 双色渐变。
        primaries = find("Button[variant=primary]", 4) or find("Button", 8)
        if primaries:
            b = primaries[0]["bounds"]
            grab(os.path.join(out, "dark-primary.png"),
                 {"x": int(b["x"]), "y": int(b["y"]),
                  "width": int(b["width"]), "height": int(b["height"])})

        cards = find("Card", 4)
        if cards:
            c = cards[0]["bounds"]
            grab(os.path.join(out, "dark-card.png"),
                 {"x": int(c["x"]), "y": int(c["y"]),
                  "width": int(c["width"]), "height": int(c["height"])})

        call("theme", {"mode": "light"})
        for _ in range(3):
            call("metrics")
        grab(os.path.join(out, "light-full.png"))
    finally:
        try:
            process.kill()
            process.wait(timeout=10)
        except Exception:
            pass

    produced = sorted(name for name in os.listdir(out) if name.endswith(".png"))
    print("采样完成：", ", ".join(produced))
    return 0


if __name__ == "__main__":
    sys.exit(main())
