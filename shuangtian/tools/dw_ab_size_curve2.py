#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""密集采集 VS Code 各物理字号的覆盖率基准，用于拟合**连续映射** γ(px)/c(px)。

为什么必须连续映射（而不是分档）：
  物理字号 = cssFontSize × devicePixelRatio，两个都是连续量
  （css 可 10~30 任意小数，dpr 可 1.0/1.25/1.5/1.75/2.0/2.25/2.5…）。
  分档硬编码会在档位边界**跳变**，而且永远覆盖不全。

为什么改字号要写配置并重启：
  实测往 `.monaco-editor .view-line` 注入 `font-size` **无效**——
  VS Code 用内联样式 + 固定的行高/列宽，CSS 注入被覆盖（computed 仍是 14px）。
  必须写 `settings.json` 的 `editor.fontSize` 再重启。

用法：python tools/dw_ab_size_curve2.py [--css 13 15 18 22] [--dpr 1.0 1.5 2.0]
"""
import argparse
import base64
import json
import pathlib
import subprocess
import time
import urllib.request

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
D = ROOT / "build" / "dwab"
VSCODE = r"C:\Users\Administrator\AppData\Local\Programs\Microsoft VS Code\Code.exe"
PROFILE = ROOT / ".st" / "vscode-curve"
SETTINGS = PROFILE / "User" / "settings.json"
PORT = 9399

SAMPLE = "\n".join([
    "# 霜天渲染 中文 Abc 123",
    "# 疆馨密霸繁悬整靓霾",
    "# 一二十上下三人",
    "# Hamburgefonstiv III",
    "# illumination minimal",
    "# 0123456789 3.14 42",
    "# ->=>(){}[]; if (x) {",
    "",
]) + "\n"

BASE_CFG = {
    "editor.fontFamily": "Consolas, 'Microsoft YaHei'",
    "editor.fontLigatures": False,
    "editor.lineHeight": 0,
    "workbench.colorTheme": "Default Dark+",
    "window.zoomLevel": 0,
    "editor.minimap.enabled": False,
    "breadcrumbs.enabled": False,
    "editor.renderLineHighlight": "none",
    "workbench.startupEditor": "none",
    "editor.glyphMargin": False,
    "editor.folding": False,
    "editor.lineNumbers": "on",
}


def write_cfg(css):
    SETTINGS.parent.mkdir(parents=True, exist_ok=True)
    cfg = dict(BASE_CFG)
    cfg["editor.fontSize"] = css
    SETTINGS.write_text(json.dumps(cfg, indent=2), encoding="utf-8")


def run_one(dpr: float, css: float):
    sample = D / "vscode-sample.py"
    sample.write_text(SAMPLE, encoding="utf-8")
    args = [VSCODE, str(sample), "--new-window", "--disable-extensions",
            "--disable-workspace-trust", f"--user-data-dir={PROFILE}",
            f"--force-device-scale-factor={dpr}",
            f"--remote-debugging-port={PORT}", "--locale", "en"]
    proc = subprocess.Popen(args)
    try:
        pages = None
        for _ in range(50):
            time.sleep(0.6)
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/json/list", timeout=1) as r:
                    pages = [t for t in json.loads(r.read()) if t.get("type") == "page"]
                if pages:
                    break
            except Exception:
                continue
        if not pages:
            return None
        from websockets.sync.client import connect
        with connect(pages[0]["webSocketDebuggerUrl"]) as ws:
            def ev(expr, mid):
                ws.send(json.dumps({"id": mid, "method": "Runtime.evaluate",
                                    "params": {"expression": expr, "returnByValue": True}}))
                while True:
                    m = json.loads(ws.recv())
                    if m.get("id") == mid:
                        return m["result"]["result"].get("value")
            ok = False
            for _ in range(45):
                if ev("(()=>{const e=document.querySelector('.monaco-editor');"
                      "return e?e.querySelectorAll('.view-line').length:0;})()", 1) >= 3:
                    ok = True
                    break
                time.sleep(0.8)
            if not ok:
                return None
            time.sleep(1.5)
            # 读**真实**生效字号：.view-line 的高度与 span 的 font-size
            # ⚠ 读 `.monaco-editor` 的 fontSize 是**错的**：那是容器，恒为 13px。
            #   真实字号在 `.view-line span` 上（实测配置 24 → span 24px、行高 32）。
            info = ev("JSON.stringify({dpr:devicePixelRatio,"
                      "textFs:(()=>{const s=document.querySelector('.view-line span');"
                      "return s?getComputedStyle(s).fontSize:'none';})(),"
                      "lineH:(()=>{const l=document.querySelector('.view-line');"
                      "return l?l.getBoundingClientRect().height:0;})()})", 2)
            ws.send(json.dumps({"id": 3, "method": "Page.captureScreenshot",
                                "params": {"format": "png"}}))
            while True:
                m = json.loads(ws.recv())
                if m.get("id") == 3:
                    out = D / f"curve-dpr{dpr:.2f}-css{css}.png"
                    out.write_bytes(base64.b64decode(m["result"]["data"]))
                    d = json.loads(info)
                    d["shot"] = str(out)
                    return d
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=6)
        except Exception:
            pass
        subprocess.run(["taskkill", "/F", "/IM", "Code.exe"], capture_output=True)


def code_rows(path):
    img = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)
    H, W = img.shape[:2]
    BG = np.array([18., 19., 20.])
    FG = np.array([0x8B, 0x94, 0x9E], float)
    dv = FG - BG
    L = np.linalg.norm(dv)
    U = dv / L
    t = np.clip(((img - BG) @ U) / L, 0, 1)
    proj = ((img - BG) @ U)[..., None] * U
    resid = np.linalg.norm((img - BG) - proj, axis=2)
    mask = (t > 0.10) & (resid < 45)
    x0, x1 = int(W * 0.06), int(W * 0.50)
    prof = mask[:, x0:x1].sum(1)
    thr = max(12, int((x1 - x0) * 0.01))
    rr = np.where(prof > thr)[0]
    if rr.size == 0:
        return []
    groups = []
    s = q = rr[0]
    for y in rr[1:]:
        if y - q > 6:
            groups.append((s, q + 1))
            s = y
        q = y
    groups.append((s, q + 1))
    out = []
    for y0, y1 in groups:
        h = y1 - y0
        if not (5 <= h <= 80):
            continue
        band = mask[y0:y1, x0:x1]
        cols = np.where(band.sum(0) > 0)[0]
        if not cols.size or cols[-1] - cols[0] < 25:
            continue
        v = t[y0:y1, x0:x1][band]
        if v.size < 60:
            continue
        out.append(dict(n=int(v.size), solid=100 * (v > 0.85).mean(),
                        full=100 * (v > 0.95).mean(), mean=float(v.mean()), h=int(h)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--css", nargs="*", type=float, default=[13, 15, 17, 20, 24])
    ap.add_argument("--dpr", nargs="*", type=float, default=[1.0, 1.5, 2.0])
    a = ap.parse_args()
    rows = []
    for dpr in a.dpr:
        for css in a.css:
            write_cfg(css)
            time.sleep(0.3)
            print(f"--- dpr={dpr} css={css} ---")
            info = run_one(dpr, css)
            if not info:
                print("  失败")
                continue
            real_css = float(info["textFs"].replace("px", ""))
            dpr_v = float(info["dpr"])
            phys = real_css * dpr_v
            ls = code_rows(pathlib.Path(info["shot"]))
            if not ls:
                print("  无代码行")
                continue
            hh = float(np.median([x["h"] for x in ls]))
            med = {k: float(np.median([x[k] for x in ls])) for k in ("solid", "full", "mean")}
            print(f"  真实 css={real_css} dpr={dpr_v} → 物理 {phys:.1f}px 行高 {hh:.0f}  "
                  f"实心 {med['solid']:.1f}% 平均 {med['mean']:.3f} ({len(ls)}行)")
            rows.append(dict(dpr=dpr_v, css=real_css, phys=phys, line_h=hh,
                             lines=len(ls), **med))
    rows.sort(key=lambda r: r["phys"])
    (D / "size-curve-vscode.json").write_text(
        json.dumps(rows, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"\n{'='*64}")
    print(f"{'物理px':>8}{'css':>6}{'dpr':>6}{'行高':>6}{'实心%':>8}{'满黑%':>8}{'平均':>7}")
    print("-" * 58)
    for r in rows:
        print(f"{r['phys']:>8.1f}{r['css']:>6.0f}{r['dpr']:>6.2f}{r['line_h']:>6.0f}"
              f"{r['solid']:>8.1f}{r['full']:>8.1f}{r['mean']:>7.3f}")
    (D / "size-curve-vscode.json").write_text(
        json.dumps(rows, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"\n已写 {D / 'size-curve-vscode.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
