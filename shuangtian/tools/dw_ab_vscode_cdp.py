#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""用 CDP 截 VS Code 的**编辑器内容区**，精确量它的字体渲染参数。

为什么用 CDP 而不是屏幕截图：
* Electron（VS Code）支持 --remote-debugging-port，可以直接拿到渲染树截图；
* 屏幕截图会被前台窗口遮挡（本机实测：全屏 IDE 盖住 VS Code，截到的是 IDE）；
* CDP 还能读到 VS Code 实际生效的 CSS（字号/字族/抗锯齿），避免猜。

用法：
  python tools/dw_ab_vscode_cdp.py            # 全流程：起 VS Code → 截图 → 分析
  python tools/dw_ab_vscode_cdp.py --analyze  # 只分析已有截图
"""
import argparse
import json
import pathlib
import subprocess
import sys
import time
import urllib.request

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
D = ROOT / "build" / "dwab"
VSCODE = r"C:\Users\Administrator\AppData\Local\Programs\Microsoft VS Code\Code.exe"
PORT = 9333

# VS Code Dark+ 主题的实际取值（默认 Dark+ 的 editor.background / foreground）
DARK_BG = np.array([30.0, 30.0, 30.0])      # #1E1E1E
DARK_FG = np.array([212.0, 212.0, 212.0])   # #D4D4D4


def make_sample():
    """样本文件：与 DWrite 行表同文本，方便逐行比对。"""
    path = D / "vscode-sample.py"
    lines = [
        "# 霜天渲染 中文 Abc 123",
        "# 疆馨密霸繁悬整靓霾",
        "# 一二十上下三人",
        "# Hamburgefonstiv III",
        "# illumination minimal",
        "# 0123456789 3.14 42",
        "# ->=>(){}[]; if (x) {",
        "",
        "def f(x):",
        "    return x + 1",
    ]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


def launch(sample):
    profile = ROOT / ".st" / "vscode-cdp-profile"
    args = [
        VSCODE, str(sample),
        "--new-window",
        "--disable-extensions",
        "--disable-workspace-trust",
        f"--user-data-dir={profile}",
        f"--remote-debugging-port={PORT}",
        "--locale", "en",
    ]
    print("启动:", " ".join(args))
    proc = subprocess.Popen(args)
    # 等 CDP 端口就绪
    for _ in range(40):
        time.sleep(0.5)
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/json/list", timeout=1) as r:
                targets = json.loads(r.read())
            pages = [t for t in targets if t.get("type") == "page"]
            if pages:
                print(f"CDP 就绪，{len(pages)} 个页面")
                return proc, pages
        except Exception:
            continue
    return proc, None


def cdp_screenshot(ws_url, out):
    """通过 WebSocket 调 Page.captureScreenshot。"""
    import base64
    try:
        from websockets.sync.client import connect
    except ImportError:
        # 退化：用 websocket-client
        import websocket
        ws = websocket.create_connection(ws_url, timeout=10)
        ws.send(json.dumps({"id": 1, "method": "Page.enable"}))
        ws.recv()
        ws.send(json.dumps({"id": 2, "method": "Page.captureScreenshot",
                            "params": {"format": "png"}}))
        while True:
            msg = json.loads(ws.recv())
            if msg.get("id") == 2:
                data = msg["result"]["data"]
                break
        ws.close()
    else:
        with connect(ws_url) as ws:
            ws.send(json.dumps({"id": 1, "method": "Page.enable"}))
            ws.recv()
            ws.send(json.dumps({"id": 2, "method": "Page.captureScreenshot",
                                "params": {"format": "png"}}))
            while True:
                msg = json.loads(ws.recv())
                if msg.get("id") == 2:
                    data = msg["result"]["data"]
                    break
    out.write_bytes(base64.b64decode(data))
    return out


def analyze(shot):
    img = np.asarray(Image.open(shot).convert("RGB"), dtype=np.float64)
    h, w = img.shape[:2]
    print(f"截图 {w}x{h}")
    bgmask = (np.abs(img - DARK_BG).max(2) < 10)
    print(f"  #1E1E1E 占比 {100.0 * bgmask.mean():.1f}%")
    if bgmask.mean() < 0.05:
        print("  ⚠ 编辑器主体很少——可能主题不同或被遮挡")
        flat = img.reshape(-1, 3)
        uniq, counts = np.unique(flat.astype(np.uint8), axis=0, return_counts=True)
        for i in np.argsort(-counts)[:3]:
            r, g, b = uniq[i]
            print(f"    主色 #{r:02X}{g:02X}{b:02X}  {100.0 * counts[i] / len(flat):.1f}%")
    t = np.clip(((img - DARK_BG) * (DARK_FG - DARK_BG)).sum(2)
                / float(((DARK_FG - DARK_BG) ** 2).sum()), 0, 1)
    ink = t > 0.15
    n = int(ink.sum())
    if n == 0:
        print("  ⚠ 无墨像素")
        return None
    ch = (img.max(2) - img.min(2))[ink]
    st = dict(ink=n,
              solid=100.0 * (t > 0.85).sum() / n,
              trans=100.0 * ((t > 0.15) & (t < 0.85)).sum() / n,
              full=100.0 * (t > 0.99).sum() / n,
              mean=float(t[ink].mean()),
              chroma=float(ch.mean()),
              chroma99=float(np.percentile(ch, 99)))
    print(f"  墨像素 {n}")
    print(f"  实心(>0.85) {st['solid']:.1f}%  过渡 {st['trans']:.1f}%  满黑(>0.99) {st['full']:.1f}%")
    print(f"  平均覆盖 {st['mean']:.3f}")
    print(f"  彩边均值 {st['chroma']:.1f}  P99 {st['chroma99']:.0f}")
    verdict = ("灰度抗锯齿（无彩边，R=G=B）" if st["chroma"] < 8
               else "ClearType 亚像素（有明显彩边）")
    print(f"  ⇒ VS Code 用的是：{verdict}")
    return st


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--analyze", action="store_true")
    a = ap.parse_args()
    shot = D / "vscode-cdp.png"
    if not a.analyze:
        sample = make_sample()
        proc, pages = launch(sample)
        try:
            if not pages:
                print("CDP 未就绪", file=sys.stderr)
                return 1
            page = pages[0]
            print("页面:", page.get("title"), page.get("url")[:80])
            time.sleep(6)   # 等渲染完成（欢迎页/加载）
            # 重新取一次页面（窗口可能刚建好）
            with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/json/list", timeout=3) as r:
                pages = [t for t in json.loads(r.read()) if t.get("type") == "page"]
            page = pages[0]
            cdp_screenshot(page["webSocketDebuggerUrl"], shot)
            print("截图:", shot)
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except Exception:
                pass
            # Electron 有多个进程，确保都退
            subprocess.run(["taskkill", "/F", "/IM", "Code.exe"],
                           capture_output=True)
    if shot.exists():
        analyze(shot)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
