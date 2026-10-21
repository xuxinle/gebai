#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""密集采集 VS Code 各物理字号下的覆盖率基准（用于拟合连续映射）。

为什么不用分档而用连续映射：
  字号是**连续量**（13 × dpr，dpr 可为 1.0/1.25/1.5/1.75/2.0/2.25/2.5…），
  分档硬编码会在档位边界跳变，且永远覆盖不全。正确做法是
  逐字号采样 → 拟合 γ(px) / c(px) → 运行时按实际物理字号求值。

采样方式：`--force-device-scale-factor` 改 dpr，同时改 `editor.fontSize`
（通过 CDP 直接注入 CSS 变量），从而覆盖 13~30px 的连续物理字号。

用法：python tools/dw_ab_size_curve.py
"""
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
PORT = 9377

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


def run_one(dpr: float, css: float) -> dict | None:
    sample = D / "vscode-sample.py"
    sample.write_text(SAMPLE, encoding="utf-8")
    profile = ROOT / ".st" / "vscode-curve"
    args = [VSCODE, str(sample), "--new-window", "--disable-extensions",
            "--disable-workspace-trust", f"--user-data-dir={profile}",
            f"--force-device-scale-factor={dpr}",
            f"--remote-debugging-port={PORT}", "--locale", "en"]
    proc = subprocess.Popen(args)
    try:
        pages = None
        for _ in range(45):
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
            ready = False
            for _ in range(40):
                st = ev("(()=>{const e=document.querySelector('.monaco-editor');"
                        "if(!e)return 'no';const n=e.querySelectorAll('.view-line').length;"
                        "return n>=3?'ok':'no';})()", 90)
                if st == "ok":
                    ready = True
                    break
                time.sleep(0.8)
            if not ready:
                return None
            # 改字号：直接设置 VS Code 的配置（会热更新）
            ev(f"(()=>{{const c=document.querySelector('.monaco-editor');"
               f"if(c) c.style.fontSize='{css}px';"
               f"return document.querySelectorAll('.view-line').length;}})()", 91)
            time.sleep(1.2)
            info = ev("JSON.stringify({dpr:devicePixelRatio,"
                      "fs:getComputedStyle(document.querySelector('.monaco-editor')||document.body).fontSize})", 92)
            ws.send(json.dumps({"id": 93, "method": "Page.captureScreenshot",
                                "params": {"format": "png"}}))
            while True:
                m = json.loads(ws.recv())
                if m.get("id") == 93:
                    out = D / f"cur-dpr{dpr:.2f}-css{css:.0f}.png"
                    out.write_bytes(base64.b64decode(m["result"]["data"]))
                    d = json.loads(info)
                    d["shot"] = str(out)
                    return d
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            pass
        subprocess.run(["taskkill", "/F", "/IM", "Code.exe"], capture_output=True)


def code_rows(path):
    """返回每行代码的覆盖率统计（排除高亮背景/界面线/minimap）。"""
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
    # 编辑区：左侧 6% 起，右侧留 minimap
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
        if not (6 <= h <= 60):
            continue
        band = mask[y0:y1, x0:x1]
        cols = np.where(band.sum(0) > 0)[0]
        if not cols.size or cols[-1] - cols[0] < 30:
            continue
        v = t[y0:y1, x0:x1][band]
        if v.size < 80:
            continue
        out.append(dict(n=int(v.size), solid=100 * (v > 0.85).mean(),
                        full=100 * (v > 0.95).mean(), mean=float(v.mean())))
    return out


def main():
    # 覆盖 13~30 物理 px：改 dpr 或 css 都能变物理字号
    combos = [(1.0, 13), (1.0, 16), (1.0, 20), (1.0, 26),
              (1.25, 13), (1.25, 16), (1.5, 13), (1.5, 16),
              (1.75, 13), (2.0, 13), (2.0, 15)]
    rows = []
    for dpr, css in combos:
        print(f"\n--- dpr={dpr} css={css} → 物理 {dpr*css:.1f}px ---")
        info = run_one(dpr, css)
        if not info:
            print("  失败")
            continue
        f_css = float(info["fs"].replace("px", ""))
        phys = f_css * float(info["dpr"])
        ls = code_rows(pathlib.Path(info["shot"]))
        if not ls:
            print("  无可识别代码行")
            continue
        med = {k: float(np.median([x[k] for x in ls])) for k in ("solid", "full", "mean")}
        print(f"  实测 dpr={info['dpr']} css={f_css} → 物理 {phys:.1f}px  "
              f"实心 {med['solid']:.1f}% 满黑 {med['full']:.1f}% 平均 {med['mean']:.3f}  ({len(ls)} 行)")
        rows.append(dict(dpr=float(info["dpr"]), css=f_css, phys=phys, lines=len(ls), **med))
    # 按物理字号排序去重
    rows.sort(key=lambda r: r["phys"])
    out = D / "size-curve-vscode.json"
    out.write_text(json.dumps(rows, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"\n{'='*60}")
    print(f"{'物理px':>8}{'实心%':>8}{'满黑%':>8}{'平均':>7}{'行数':>6}")
    print("-" * 40)
    for r in rows:
        print(f"{r['phys']:>8.1f}{r['solid']:>8.1f}{r['full']:>8.1f}{r['mean']:>7.3f}{r['lines']:>6}")
    print(f"\n已写 {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
