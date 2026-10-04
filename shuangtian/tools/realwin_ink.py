#!/usr/bin/env python3
"""真窗口浏览器基准：从截图里**反解红框**，逐框比整幅总墨量。

## 为什么反解红框而不是按坐标切

真窗口截图里有工具栏、滚动条、页面滚动位置——任何基于"窗口几何 + 页面坐标"的假设都会错
（实测踩到：滚动位置让框与页面坐标错开 200+ px）。红描边把框的位置变成**图里的红色直线**，
找出来即可，与窗口如何摆放无关。

## 为什么比整幅总墨量

裁剪/对齐都已被证明不可靠（见 `size_ink_oneshot.py` 的三条记录）。整幅总墨量不依赖二者，
代价是要求一框一行字。

用法：python tools/realwin_ink.py <基准截图.png> <霜天目录> <霜天文件名模板> <gamma...>
      模板用 {s} 占位字号，如 "st/{g}.{s}.png"
"""

import sys

import numpy as np
from PIL import Image

# 红框线检测阈值
RED_MIN, GREEN_MAX, BLUE_MAX, LINE_PIXELS = 180, 100, 100, 500
FG = np.array([0x0F, 0x17, 0x2A], dtype=float)
BG = np.array([255.0, 255.0, 255.0])


def find_boxes(img):
    """返回 [(y0, y1), ...]——红框的水平线两两配对。"""
    a = np.asarray(img.convert("RGB")).astype(int)
    red = (a[:, :, 0] > RED_MIN) & (a[:, :, 1] < GREEN_MAX) & (a[:, :, 2] < BLUE_MAX)
    rc = red.sum(axis=1)
    lines = [y for y in range(a.shape[0]) if rc[y] > LINE_PIXELS]
    return [(lines[k] + 1, lines[k + 1] - 1) for k in range(len(lines) - 1)]


def ink_in(img, y0, y1, x0, x1):
    a = np.asarray(img.convert("RGB")).astype(float)[y0:y1, x0:x1]
    d = BG - FG
    return float(np.clip(((BG - a) @ d) / float(d @ d), 0.0, 1.0).sum())


def main() -> int:
    ref_path, st_dir, tmpl = sys.argv[1], sys.argv[2], sys.argv[3]
    gammas = sys.argv[4:] or ["060", "075", "090", "105"]
    sizes = [int(x) for x in sys.stdin.read().lstrip("\ufeff").split()] if not sys.stdin.isatty() else []
    ref = Image.open(ref_path)
    boxes = find_boxes(ref)
    if not sizes:
        print(f"检测到 {len(boxes)} 个框；请在 stdin 给字号列表（与框顺序一致）")
        return 1
    if len(boxes) != len(sizes):
        print(f"框数 {len(boxes)} != 字号数 {len(sizes)}")
        return 1
    # 框内的列范围（避开左右边框）：用中间区域
    a = np.asarray(ref.convert("RGB")).astype(int)
    red = (a[:, :, 0] > RED_MIN) & (a[:, :, 1] < GREEN_MAX) & (a[:, :, 2] < BLUE_MAX)
    cols = np.where(red.sum(axis=0) > LINE_PIXELS)[0]
    x0, x1 = int(cols.min()) + 4, int(cols.max()) - 3

    print(f"红框 {len(boxes)} 个，列范围 {x0}..{x1}")
    print(f"{'逻辑px':>7}  " + "  ".join(f"γ{g:>4}" for g in gammas) + "   理想γ")
    print("-" * 70)
    for size, (y0, y1) in zip(sizes, boxes):
        # 框内只积分框的**内部**（去掉 1px 边框影响）
        ref_ink = ink_in(ref, y0, y1, x0, x1)
        ratios = []
        for g in gammas:
            path = f"{st_dir}/{tmpl.format(g=g, s=size)}"
            st = Image.open(path)
            # 霜天单行图：整幅积分（画布 640×96）
            ratios.append(ink_in(st, 0, st.size[1], 0, st.size[0]) / ref_ink if ref_ink > 0 else float("nan"))
        gv = [float(g[:1] + "." + g[1:]) for g in gammas]
        ideal = None
        for k in range(len(ratios) - 1):
            if (ratios[k] - 1.0) * (ratios[k + 1] - 1.0) <= 0.0 and ratios[k] != ratios[k + 1]:
                t = (1.0 - ratios[k]) / (ratios[k + 1] - ratios[k])
                ideal = gv[k] + t * (gv[k + 1] - gv[k])
                break
        print(f"{size:>7}  " + "  ".join(f"{r:>6.3f}" for r in ratios)
              + f"   {('%.3f' % ideal) if ideal is not None else '  —  '}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
