#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""逐 DPI 对比霜天 vs VS Code（用 --force-device-scale-factor 精确控制 dpr）。

为什么用这个参数而不是改系统 DPI：
  改 `HKCU\\...\\AppliedDPI` 只对**新登录会话**生效，已运行的进程仍读旧值——
  实测把注册表改成 96/120/168/192，VS Code 的 `devicePixelRatio` 始终是 1.5。
  而 `--force-device-scale-factor` 直接作用于渲染进程，dpr 立刻改变，
  语义上等价于"在这台机器上用 N% 缩放"，且可随时恢复。

对比口径（关键，上一轮踩过）：**只比物理字号**。
  物理字号 = dpr × CSS fontSize。双方对齐物理字号后，覆盖率分布才可比。

用法：python tools/dw_ab_dpi_force.py [--factors 1.0 1.25 1.5 1.75 2.0]
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
PROBE = ROOT / "build" / "probe" / "dw_ab_probe.exe"
PORT = 9356

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


def capture(factor: float) -> dict | None:
    sample = D / "vscode-sample.py"
    sample.write_text(SAMPLE, encoding="utf-8")
    profile = ROOT / ".st" / "vscode-dpi-probe"
    args = [VSCODE, str(sample), "--new-window", "--disable-extensions",
            "--disable-workspace-trust", f"--user-data-dir={profile}",
            f"--force-device-scale-factor={factor}",
            f"--remote-debugging-port={PORT}", "--locale", "en"]
    proc = subprocess.Popen(args)
    info = None
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
        with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/json/list", timeout=3) as r:
            pages = [t for t in json.loads(r.read()) if t.get("type") == "page"]
        from websockets.sync.client import connect
        with connect(pages[0]["webSocketDebuggerUrl"]) as ws:
            # **必须等编辑器真正加载**：刚启动时显示欢迎页（Welcome），
            # 那时截到的是宣传图与按钮，根本量不到代码文本（上一轮就在这里出的错：
            # 截到了 "Welcome to VS Code" 大图，统计出的"行"全是界面元素）。
            # 判据：存在 .monaco-editor 且其内含 .view-line（代码行）至少 3 行。
            ready_expr = ("(() => {const ed=document.querySelector('.monaco-editor');"
                          "if(!ed) return 'no-editor';"
                          "const n=ed.querySelectorAll('.view-line').length;"
                          "return n>=3 ? 'ok:'+n : 'lines:'+n;})()")
            ready = False
            for _ in range(40):
                ws.send(json.dumps({"id": 90, "method": "Runtime.evaluate",
                                    "params": {"expression": ready_expr, "returnByValue": True}}))
                while True:
                    m = json.loads(ws.recv())
                    if m.get("id") == 90:
                        st = m["result"]["result"].get("value", "")
                        break
                if isinstance(st, str) and st.startswith("ok"):
                    print(f"  编辑器就绪（{st}）")
                    ready = True
                    break
                time.sleep(0.8)
            if not ready:
                print("  ⚠ 编辑器未就绪（可能停在欢迎页）")
            time.sleep(1.5)
            expr = ("JSON.stringify({dpr:devicePixelRatio,"
                    "fs:getComputedStyle(document.querySelector('.monaco-editor')||document.body).fontSize})")
            ws.send(json.dumps({"id": 1, "method": "Runtime.evaluate",
                                "params": {"expression": expr, "returnByValue": True}}))
            while True:
                m = json.loads(ws.recv())
                if m.get("id") == 1:
                    info = json.loads(m["result"]["result"]["value"])
                    break
            ws.send(json.dumps({"id": 2, "method": "Page.captureScreenshot",
                                "params": {"format": "png"}}))
            while True:
                m = json.loads(ws.recv())
                if m.get("id") == 2:
                    out = D / f"vsc-f{factor:.2f}.png"
                    out.write_bytes(base64.b64decode(m["result"]["data"]))
                    info["shot"] = str(out)
                    break
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            pass
        subprocess.run(["taskkill", "/F", "/IM", "Code.exe"], capture_output=True)
    return info


def cov_stats(a, bg):
    bgv = np.asarray(bg, float)
    L = a @ np.array([0.299, 0.587, 0.114])
    Lbg = bgv @ np.array([0.299, 0.587, 0.114])
    Lfg = np.percentile(L, 99.7)
    c = np.clip((L - Lbg) / (Lfg - Lbg), 0, 1)
    ink = c > 0.05
    if not ink.any():
        return None
    v = c[ink]
    return dict(n=int(ink.sum()), solid=100 * (v > 0.85).mean(),
                full=100 * (v > 0.95).mean(), mean=v.mean())


def vscode_lines(shot: pathlib.Path):
    """从 VS Code 截图取若干行文本（跳过当前行高亮），逐行统计。"""
    img = np.asarray(Image.open(shot).convert("RGB"), dtype=np.float64)
    h, w = img.shape[:2]
    # 编辑器主体色 = 屏幕中最常见的暗色
    flat = img.reshape(-1, 3)
    uniq, counts = np.unique(flat.astype(np.uint8), axis=0, return_counts=True)
    bgc = uniq[np.argmax(counts)].astype(np.float64)
    # 只在左 45% 宽、中间 60% 高处找文字（避开侧栏/状态栏/滚动条）
    sub = img[int(h * 0.12):int(h * 0.85), int(w * 0.03):int(w * 0.48)]
    mask = np.abs(sub - bgc).max(2) > 20
    prof = mask.sum(1)
    rr = np.where(prof > max(6, int(w * 0.004)))[0]
    if rr.size == 0:
        return []
    # 逐行真实底色（排除高亮行）
    out = []
    s = q = rr[0]
    groups = []
    for y in rr[1:]:
        if y - q > 4:
            groups.append((s, q + 1))
            s = y
        q = y
    groups.append((s, q + 1))
    for y0, y1 in groups:
        band = sub[y0:y1]
        # 该行右侧（无字处）取真实底色
        right = band[:, int(band.shape[1] * 0.85):].reshape(-1, 3)
        rbg = np.median(right, axis=0)
        st = cov_stats(band, rbg)
        if st and st["n"] > 40:
            out.append(st)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--factors", nargs="*", type=float,
                    default=[1.0, 1.25, 1.5, 1.75, 2.0])
    a = ap.parse_args()
    rows = []
    for f in a.factors:
        print(f"\n=== force-device-scale-factor={f} ===")
        info = capture(f)
        if not info:
            print("  ⚠ 截图失败")
            continue
        css = float(info["fs"].replace("px", ""))
        dpr = float(info["dpr"])
        phys = css * dpr
        lines = vscode_lines(pathlib.Path(info["shot"]))
        print(f"  dpr={dpr}  CSS={css}px  → 物理 {phys:.1f}px  行数 {len(lines)}")
        if lines:
            med = {k: float(np.median([x[k] for x in lines]))
                   for k in ("solid", "full", "mean")}
            print(f"  VS Code 中位：实心 {med['solid']:.1f}%  满黑 {med['full']:.1f}%  平均 {med['mean']:.3f}")
            rows.append(dict(factor=f, dpr=dpr, css=css, phys=phys, **med))
    print(f"\n{'='*66}")
    print(f"{'factor':>8}{'dpr':>6}{'CSS':>6}{'物理px':>8}{'实心%':>8}{'满黑%':>8}{'平均':>7}")
    print("-" * 66)
    for r in rows:
        print(f"{r['factor']:>8.2f}{r['dpr']:>6.2f}{r['css']:>6.1f}{r['phys']:>8.1f}"
              f"{r['solid']:>8.1f}{r['full']:>8.1f}{r['mean']:>7.3f}")
    (D / "dpi-force.json").write_text(
        json.dumps(rows, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"\n已写 {D / 'dpi-force.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
