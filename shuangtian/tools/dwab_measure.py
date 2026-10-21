#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 VS Code 截图里**稳健地**量出代码行的覆盖率分布。

前几轮反复踩的坑（逐条对应代码里的防护）：
1. 读 `.monaco-editor` 的 fontSize 是错的 —— 那是容器，恒为 13px；
   真实字号在 `.view-line span` 上。（本模块不读字号，改由调用方给）
2. 区域按**比例**切会把文字切掉 —— 高 DPI 图宽 2400px 时 `x<50%` 会切到中途。
   改为**按实际墨迹边界**定位。
3. 行分组漏掉「行高 < 5px」的界面横线，也漏掉「跨行选中背景」。
   改为：先按墨迹行投影分组，再用**行高一致性**筛（同一页行高应基本相同）。
4. 当前行高亮背景（#242526）会被误当墨迹 —— 它色相与字色差得远，
   用「到 底→字色 连线的垂距 < 45」把它排除。
5. 文字颜色不一定是注释灰 #8B949E —— 本模块**自动检测**主文字色。
"""
import numpy as np
from PIL import Image


def detect_text_color(img, bg):
    """自动检测主文字色：取「与底色差得远」的像素里最亮的那个簇。"""
    a = img
    dist = np.abs(a - np.asarray(bg, float)).max(2)
    cand = a[dist > 60]
    if cand.size == 0:
        return None
    # 最亮 10% 的均值当主文字色（代码区里越亮越可能是正文/注释）
    lum = cand @ np.array([0.299, 0.587, 0.114])
    bright = cand[lum >= np.percentile(lum, 90)]
    return np.median(bright, axis=0)


def measure_lines(img, bg=None, text_color=None, min_lines=3):
    """返回每行代码的覆盖率统计；失败返回 None。

    img: HxWx3 float 数组；bg: 底色（None = 自动取众数）。
    """
    a = img
    H, W = a.shape[:2]
    if bg is None:
        flat = a.reshape(-1, 3)
        uq, ct = np.unique(flat.astype(np.uint8), axis=0, return_counts=True)
        bg = uq[np.argmax(ct)].astype(float)
    bg = np.asarray(bg, float)
    if text_color is None:
        text_color = detect_text_color(a, bg)
    if text_color is None:
        return None
    fg = np.asarray(text_color, float)

    dv = fg - bg
    n = np.linalg.norm(dv)
    if n < 20:
        return None
    U = dv / n
    t = np.clip(((a - bg) @ U) / n, 0, 1)
    proj = ((a - bg) @ U)[..., None] * U
    resid = np.linalg.norm((a - bg) - proj, axis=2)

    # 只保留「色相接近主文字色」的像素：排除高亮背景/别的语法色
    mask = (t > 0.08) & (resid < 50)

    # 先找左右墨迹边界（按列投影），去掉界面装饰
    colprof = mask.sum(0)
    cthr = max(3, int(H * 0.002))
    cols = np.where(colprof > cthr)[0]
    if cols.size < 10:
        return None
    x0, x1 = int(cols[0]), int(cols[-1]) + 1
    # 若最右侧是 minimap（宽而均匀），裁掉
    if x1 - x0 > W * 0.85:
        x1 = x0 + int((x1 - x0) * 0.85)

    sub = mask[:, x0:x1]
    prof = sub.sum(1)
    rthr = max(2, int((x1 - x0) * 0.004))
    rr = np.where(prof > rthr)[0]
    if rr.size == 0:
        return None

    groups = []
    s = q = rr[0]
    for y in rr[1:]:
        if y - q > 2:
            groups.append((s, q + 1))
            s = y
        q = y
    groups.append((s, q + 1))

    # 用行高一致性筛：文字行的高度应集中在某个范围
    heights = [b - a_ for a_, b in groups]
    if not heights:
        return None
    med_h = float(np.median(heights))
    keep = [(a_, b) for a_, b in groups
            if med_h * 0.45 <= (b - a_) <= med_h * 2.2 and (b - a_) >= 4]

    out = []
    for y0, y1 in keep:
        band = sub[y0:y1]
        cols2 = np.where(band.sum(0) > 0)[0]
        if not cols2.size or cols2[-1] - cols2[0] < (x1 - x0) * 0.08:
            continue
        v = t[y0:y1, x0:x1][band]
        if v.size < 50:
            continue
        out.append(dict(n=int(v.size), h=int(y1 - y0),
                        solid=100 * float((v > 0.85).mean()),
                        full=100 * float((v > 0.95).mean()),
                        mean=float(v.mean())))
    if len(out) < min_lines:
        return None
    return out, dict(bg=bg.tolist(), fg=fg.tolist(), x=(x0, x1))
