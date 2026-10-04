"""文字逐像素 A/B 报告（仅验证用）：比较霜天与浏览器在同一对照页上的渲染。

口径：**覆盖率** = (背景亮度 − 像素亮度) / (背景亮度 − 前景亮度)。
只用背景/前景两端的亮度，去掉了「文字颜色本身不是黑」带来的污染——
这正是 2026-10-04 第一次测量踩到的坑：Tone::Muted（#56647C）的亮度本来就只有 103，
按「相对背景的暗化量」当覆盖率算，会把「颜色本来就浅」误判成「笔画没到满黑」。

判据：
  peak    最黑像素的覆盖率（笔画是否达到满黑）
  solid   覆盖率 > 0.85 的实心像素数
  mid     0.15 < 覆盖率 < 0.85 的中间调像素数（= 过渡带）
  mid/solid  中间调占比（**越低越锐**，这是主判据）
  ink     Σ覆盖率（墨量；用来发现「更锐」实为「更细」的假改善）
"""

import sys

import numpy as np
from PIL import Image

FG = np.array([0x0F, 0x17, 0x2A], dtype=float)
BG = np.array([0xFF, 0xFF, 0xFF], dtype=float)


def coverage(path, box=None):
    im = Image.open(path).convert("RGB")
    a = np.asarray(im).astype(float)
    if box is not None:
        x0, y0, x1, y1 = box
        a = a[y0:y1, x0:x1]
    # 逐通道投影到 [BG, FG] 连线上，取 0..1 参数即覆盖率（对亚像素彩边稳健）
    direction = BG - FG
    denom = float(direction @ direction)
    return np.clip(((BG - a) @ direction) / denom, 0.0, 1.0)


def stats(path, tag, box=None):
    cov = coverage(path, box)
    solid = int((cov > 0.85).sum())
    mid = int(((cov > 0.15) & (cov < 0.85)).sum())
    half = int(((cov > 0.45) & (cov < 0.55)).sum())
    ratio = (mid / solid) if solid else -1.0
    print(
        f"  {tag:18s} peak={cov.max():.3f} solid={solid:6d} mid={mid:6d} "
        f"half={half:5d} mid/solid={ratio:6.3f} ink={cov.sum():9.1f}"
    )
    return cov


if __name__ == "__main__":
    rows = [
        ("browser", sys.argv[1]),
        ("shuangtian", sys.argv[2]),
    ]
    for tag, path in rows:
        stats(path, tag)
