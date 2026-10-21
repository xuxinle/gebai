#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""逐 DPI 对比霜天 vs VS Code（同物理字号）。

关键前提（上一轮踩过）：
  VS Code 的物理字号 = `devicePixelRatio × editor.fontSize(CSS)`。
  **必须逐 DPI 实测**，不能假设某一档就代表全部——13px CSS 在 144 DPI 下是 19.5px 物理，
  在 96 DPI 下是 13px 物理，两者笔画宽度差 1.5 倍，覆盖率分布完全不同。

做法：
  1. 临时把系统 DPI 设成目标值（注册表 `AppliedDPI` + `Win8DpiScaling`），广播 WM_SETTINGCHANGE；
  2. 启动 VS Code（新 profile），CDP 读 dpr 与 fontSize，截图；
  3. 霜天以同一物理字号渲染；
  4. 恢复原 DPI。

用法：python tools/dw_ab_dpi_matrix.py [--restore]
  --restore  只恢复 DPI（若上次异常退出没恢复）
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
PROBE = ROOT / "build" / "probe" / "dw_ab_probe.exe"
DPI_LIST = [96, 120, 144, 168, 192]
PORT = 9345


def ps(cmd: str) -> str:
    r = subprocess.run(["pwsh", "-NoProfile", "-Command", cmd],
                       capture_output=True, text=True)
    return (r.stdout or "") + (r.stderr or "")


def read_dpi() -> int:
    out = ps('(Get-ItemProperty -Path "HKCU:\\Control Panel\\Desktop\\WindowMetrics" '
             '-Name AppliedDPI -ErrorAction SilentlyContinue).AppliedDPI')
    try:
        return int(out.strip().splitlines()[0])
    except Exception:
        return 96


def set_dpi(dpi: int) -> bool:
    """设系统 DPI 并广播（需注销/重启资源管理器才完全生效；这里尽力而为）。"""
    cmds = [
        f'Set-ItemProperty -Path "HKCU:\\Control Panel\\Desktop\\WindowMetrics" '
        f'-Name AppliedDPI -Value {dpi} -Type DWord',
        'Set-ItemProperty -Path "HKCU:\\Control Panel\\Desktop" '
        '-Name Win8DpiScaling -Value 1 -Type DWord',
        'Set-ItemProperty -Path "HKCU:\\Control Panel\\Desktop" '
        f'-Name LogPixels -Value {dpi} -Type DWord',
        # 广播设置变更（尽力，部分进程需重启）
        'Add-Type @\"\n'
        'using System;using System.Runtime.InteropServices;\n'
        'public class BC { [DllImport("user32.dll")] '
        'public static extern IntPtr SendMessageTimeout(IntPtr h,uint m,IntPtr w,string l,'
        'uint f,uint t,out IntPtr r); }\n'
        '\"@\n'
        '$r=[IntPtr]::Zero\n'
        '[BC]::SendMessageTimeout([IntPtr]0xffff,0x001A,[IntPtr]::Zero,"ImmersiveColorSet",'
        '2,3000,[ref]$r) | Out-Null',
    ]
    for c in cmds:
        ps(c)
    return True


def vscode_measure(dpi: int) -> dict | None:
    """起 VS Code，读 dpr/fontSize 并截编辑器区域。"""
    profile = ROOT / ".st" / f"vscode-dpi-{dpi}"
    sample = D / "vscode-sample.py"
    if not sample.exists():
        sample.write_text("\n".join([
            "# 霜天渲染 中文 Abc 123", "# 疆馨密霸繁悬整靓霾", "# 一二十上下三人",
            "# Hamburgefonstiv III", "# illumination minimal",
            "# 0123456789 3.14 42", "# ->=>(){}[]; if (x) {", "",
        ]) + "\n", encoding="utf-8")
    args = [VSCODE, str(sample), "--new-window", "--disable-extensions",
            "--disable-workspace-trust", f"--user-data-dir={profile}",
            f"--remote-debugging-port={PORT}", "--locale", "en"]
    proc = subprocess.Popen(args)
    info = None
    try:
        pages = None
        for _ in range(40):
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
        time.sleep(7)
        with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/json/list", timeout=3) as r:
            pages = [t for t in json.loads(r.read()) if t.get("type") == "page"]
        from websockets.sync.client import connect
        with connect(pages[0]["webSocketDebuggerUrl"]) as ws:
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
                    import base64
                    out = D / f"vsc-dpi{dpi}.png"
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


def bands_of(a, bg, minpix=8):
    ink = np.abs(a - np.asarray(bg, float)).max(2) > 20
    prof = ink.sum(1)
    rr = np.where(prof > minpix)[0]
    if rr.size == 0:
        return []
    out = []
    s = q = rr[0]
    for y in rr[1:]:
        if y - q > 3:
            out.append((int(s), int(q) + 1))
            s = y
        q = y
    out.append((int(s), int(q) + 1))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--restore", action="store_true")
    a = ap.parse_args()
    orig = read_dpi()
    print(f"当前系统 DPI: {orig}（{orig / 96 * 100:.0f}%）")
    if a.restore:
        set_dpi(orig)
        print("已重新广播当前 DPI")
        return 0

    rows = []
    try:
        for dpi in DPI_LIST:
            print(f"\n=== 切换到 {dpi} DPI（{dpi / 96 * 100:.0f}%）===")
            set_dpi(dpi)
            time.sleep(2)
            cur = read_dpi()
            print(f"  注册表 AppliedDPI = {cur}")
            info = vscode_measure(dpi)
            if not info:
                print("  ⚠ VS Code 测量失败，跳过")
                continue
            fs_px = float(info["fs"].replace("px", ""))
            phys = fs_px * float(info["dpr"])
            print(f"  VS Code: dpr={info['dpr']} fontSize={fs_px}px → 物理 {phys:.1f}px")
            rows.append(dict(dpi=dpi, cur=cur, dpr=info["dpr"], css=fs_px,
                             phys=phys, shot=info["shot"]))
    finally:
        print(f"\n恢复系统 DPI → {orig}")
        set_dpi(orig)

    # 汇总
    print(f"\n{'='*72}")
    print(f"{'目标DPI':>8}{'注册表':>8}{'VS Code dpr':>12}{'CSS字号':>9}{'物理字号':>9}")
    print("-" * 72)
    for r in rows:
        print(f"{r['dpi']:>8}{r['cur']:>8}{r['dpr']:>12}{r['css']:>9.1f}{r['phys']:>9.1f}")
    out = D / "dpi-matrix.json"
    out.write_text(json.dumps(rows, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"\n已写 {out}")
    print("注：DPI 变更通常需要**注销/重启**才对已运行的程序完全生效——"
          "本脚本只能改注册表并广播，VS Code 若仍读旧值，结论以 dpr/物理字号实测为准。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
