#!/usr/bin/env python3
"""候选修法扫描：对每个渲染档位，与浏览器逐带比对，报墨量/实心/过渡三个比值。

用 `tools/text_ab_diff.py` 的**同一套口径**（各自切带、按序配对、逐通道覆盖率投影），
避免"扫描脚本自己另写一套量法"导致的漂移。

用法：python tools/text_ab_sweep.py <name> <dir> <probe 路径> [--base 页面]
"""
import importlib.util
import json
import pathlib
import subprocess
import sys

import numpy as np
from PIL import Image

TOOLS = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("ab", TOOLS / "text_ab_diff.py")
ab = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ab)


def measure(name, d, st_png, boxes_meta, fonts):
    st_img = ab.load(st_png)
    br_img = ab.load(pathlib.Path(d) / f"ab-{name}-br.png")
    h = min(st_img.shape[0], br_img.shape[0])
    w = min(st_img.shape[1], br_img.shape[1])
    st_img, br_img = st_img[:h, :w], br_img[:h, :w]
    expect = [m for m in boxes_meta["rows"] if m["label"] != "（空行）"]
    bs, bb = ab.bands(st_img), ab.bands(br_img)
    assert len(bs) == len(bb) == len(expect), f"带数不符 {len(bs)}/{len(bb)}/{len(expect)}"
    mint = msol = mtra = 0.0
    n = 0
    for m, (sy0, sy1), (by0, by1) in zip(expect, bs, bb):
        fg = ab.hex_rgb(m["color"])
        ta = ab.tight(ab.coverage(st_img[sy0:sy1, :], fg))
        tb = ab.tight(ab.coverage(br_img[by0:by1, :], fg))
        if ta is None or tb is None:
            continue
        ww, hh = min(ta.shape[1], tb.shape[1]), min(ta.shape[0], tb.shape[0])
        sa, ma, ia = ab.stats(ta[:hh, :ww])
        sb, mb, ib = ab.stats(tb[:hh, :ww])
        if not (ib and sb):
            continue
        mint += ia / ib
        msol += sa / sb
        mtra += ma / mb if mb else 1.0
        n += 1
    return mint / n, msol / n, mtra / n, n


def main() -> int:
    name, d, probe = sys.argv[1], sys.argv[2], sys.argv[3]
    meta = json.loads((TOOLS / f"text_ab_{name}_rows.json").read_text())
    fonts = json.loads((pathlib.Path(d) / f"{name}.fonts.json").read_text())
    configs = [
        ("默认（fit=light, comp, filter）", []),
        ("gamma 0.90", ["0.9"]),
        ("gamma 0.80", ["0.8"]),
        ("gamma 0.70", ["0.7"]),
        ("gamma 0.60", ["0.6"]),
        ("filter 强度 1/4", ["--lcd-taps=2,20,212,20,2"]),
        ("filter 关闭", ["--nofilter"]),
        ("fit 关闭", ["--nofit"]),
        ("fit normal", ["--fit-normal"]),
        ("灰度", ["--gray"]),
        ("gamma 0.8 + filter 1/4", ["0.8", "--lcd-taps=2,20,212,20,2"]),
        ("gamma 0.7 + filter 1/4", ["0.7", "--lcd-taps=2,20,212,20,2"]),
        ("加墨（da+light）", ["--darken"]),
        ("加墨 + fit=Normal", ["--darken", "--fit-normal"]),
        ("加墨 + gamma0.95", ["--darken", "0.95"]),
        ("加墨 + fit=Normal + γ0.95", ["--darken", "--fit-normal", "0.95"]),
        ("加墨 γ1.00", ["--darken", "1.0"]),
        ("加墨 γ0.95", ["--darken", "0.95"]),
        ("加墨 γ0.92", ["--darken", "0.92"]),
        ("加墨 γ1.00 + 滤波1/4", ["--darken", "1.0", "--lcd-taps=2,20,212,20,2"]),
        ("加墨+strict-hints γ1.00", ["--darken", "1.0", "--strict-hints"]),
        ("加墨+strict-hints γ0.95", ["--darken", "0.95", "--strict-hints"]),
        ("strict-hints（无加墨）", ["--strict-hints"]),
    ]
    print(f"{'档位':<32}{'墨量比':>9}{'实心比':>9}{'过渡比':>9}{'行数':>6}")
    print("-" * 66)
    for tag, args in configs:
        env = {"ST_TEXT_LCD_TAPS": "", "ST_TEXT_LCD_FILTER": ""}
        taps = None
        real = []
        for a in args:
            if a.startswith("--lcd-taps="):
                taps = a.split("=", 1)[1]
            else:
                real.append(a)
        environ = dict(**__import__("os").environ)
        if taps:
            environ["ST_TEXT_LCD_TAPS"] = taps
        r = subprocess.run([probe, d, "1.5"] + real, capture_output=True, text=True,
                           env=environ)
        if r.returncode != 0:
            print(f"{tag:<32}  探针失败：{r.stderr.strip()[:60]}")
            continue
        ink, sol, tra, n = measure(name, d, pathlib.Path(d) / f"ab-{name}-st.png", meta, fonts)
        print(f"{tag:<32}{ink:>9.3f}{sol:>9.3f}{tra:>9.3f}{n:>6}")
    # 还原默认
    subprocess.run([probe, d, "1.5"], capture_output=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
