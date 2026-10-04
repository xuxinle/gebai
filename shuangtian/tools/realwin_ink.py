"""真窗口浏览器基准的「字号 → 理想 γ」量尺（**浅底/深底共用一条路径**）。

## 口径（三个"不"）

  · **不裁剪**：比**整幅总墨量**——裁剪/对齐都已被证明不可靠（见 `DESIGN.md §4.3.7.16`
    记的三个量尺陷阱：按逻辑 top 切带、按各自墨迹包围盒取 min、按单侧元素框切带）。
    代价是要求一页只放一行字。
  · **不对齐**：行带由**行投影**自动分，不按窗口几何或页面坐标算。
  · **不猜底色**：背景/前景由 `--dark` 决定，取主题 token 原值。

## 真窗口截图的四个坑（都踩过，都在这里处理掉）

  1. 截图含浏览器工具栏 → 靠**框边框色**（青色 `#00E5FF`）找内容起点。
     ⚠ 用"连续 N 行接近底色"不行：深色主题下工具栏与页面**都是深色**，会把工具栏并进来
     （实测多出一条 142px 假带）。
  2. 窗口外有别的窗口露出 → 列范围取"**最长连续**符合页面的列区"，不按全图比例
     （否则行投影被整体抬高、所有带并成一条）。
  3. 一屏放不下全部框 → 只测**可见的前 N 框**（顺序一致，前 N 个就是前 N 个字号）。
  4. 移动窗口后要**重新截图**——改动窗口位置会让探测坐标与旧截图不一致。

用法：
  python tools/realwin_ink.py <基准png> <霜天目录> <文件名模板> <字号...> -- <gamma...>
  例：python tools/realwin_ink.py ref.png build/probe/realwin/st "{g}.{s}.png" \
          --sizes 10 11 12 14 16 -- 060 075 090
"""

import argparse
import sys

import numpy as np
from PIL import Image

# 主题 token（`src/ui/theme.cpp`）
LIGHT = {"bg": [255.0, 255.0, 255.0], "fg": [0x0F, 0x17, 0x2A]}
DARK = {"bg": [0x0A, 0x0F, 0x1A], "fg": [0xE8, 0xEE, 0xF9]}
EDGE = {"r_max": 120, "g_min": 170, "b_min": 190}   # 青边 `#00E5FF`
INK_TH = 0.02
EDGE_LINE_PIXELS = 200


def cov(arr, bg, fg):
    """覆盖率：像素投影到 [bg, fg] 连线上的 0..1 参数（逐通道，不用亮度）。"""
    b = np.asarray(bg, dtype=float)
    d = np.asarray(fg, dtype=float) - b
    return np.clip(((arr - b) @ d) / float(d @ d), 0.0, 1.0)


def edge_mask(arr):
    a = np.asarray(arr).astype(int)
    return ((a[:, :, 0] < EDGE["r_max"]) & (a[:, :, 1] > EDGE["g_min"])
            & (a[:, :, 2] > EDGE["b_min"]))


def find_edge_rows(img, min_pixels=EDGE_LINE_PIXELS):
    m = edge_mask(img)
    return [y for y in range(m.shape[0]) if m[y].sum() > min_pixels]


def page_columns(img, bg):
    """页面列范围 = 列中位数等于**背景色**的最长连续区（避开窗口外露出的别的窗口）。"""
    a = np.asarray(img.convert("RGB")).astype(float)
    med = np.median(a, axis=0)
    near = np.abs(med - np.asarray(bg, dtype=float)).max(axis=1) < 26.0
    best, start = (0, 0, 0), None
    for x, f in enumerate(near):
        if f and start is None:
            start = x
        elif not f and start is not None:
            if x - start > best[0]:
                best = (x - start, start, x)
            start = None
    if start is not None and len(near) - start > best[0]:
        best = (len(near) - start, start, len(near))
    return best[1] + 8, best[2] - 8


def bands(img, x0, x1, bg, fg, th=INK_TH, gap=8):
    a = np.asarray(img.convert("RGB")).astype(float)[:, x0:x1, :]
    prof = cov(a, bg, fg).sum(axis=1) / max(1, (x1 - x0))
    on = prof > th
    out, start, blanks = [], None, 0
    for y, f in enumerate(on):
        if f:
            if start is None:
                start = y
            blanks = 0
        elif start is not None:
            blanks += 1
            if blanks > gap:
                out.append((start, y - blanks + 1))
                start = None
                blanks = 0
    if start is not None:
        out.append((start, len(on)))
    return [(a0, a1) for a0, a1 in out if a1 - a0 >= 5]


def ink_of(arr, x0, x1, y0, y1, bg, fg):
    c = cov(np.asarray(arr).astype(float)[y0:y1, x0:x1, :], bg, fg)
    rx = np.where((c > 0.12).any(axis=0))[0]
    return float(c[:, rx[0]:rx[-1] + 1].sum()) if rx.size else 0.0


def main() -> int:
    ap = argparse.ArgumentParser(description="真窗口基准的『字号 → 理想 γ』量尺")
    ap.add_argument("ref", help="真窗口浏览器截图（含对照页）")
    ap.add_argument("st_dir", help="霜天单行图目录")
    ap.add_argument("tmpl", help="文件名模板，{g}=gamma 串 {s}=字号，如 '{g}.{s}.png'")
    ap.add_argument("--sizes", nargs="+", type=int, required=True,
                    help="框内字号，**顺序须与页面上从上到下一致**")
    ap.add_argument("--gammas", nargs="+", required=True,
                    help="霜天侧测过的 gamma 串（文件名里用的形式，如 060 110）")
    ap.add_argument("--dark", action="store_true", help="深色主题（底 #0A0F1A 字 #E8EEF9）")
    args = ap.parse_args()

    theme = DARK if args.dark else LIGHT
    bg, fg = theme["bg"], theme["fg"]
    ref = Image.open(args.ref)
    rows = find_edge_rows(ref)
    if not rows:
        print("未找到框边框色——确认对照页由 gen_realwin_page.py 生成且已完整显示")
        return 1
    top = rows[0] - 1
    ref = ref.crop((0, top, ref.size[0], ref.size[1]))
    x0, x1 = page_columns(ref, bg)
    print(f"主题={'深' if args.dark else '浅'}；内容起始行 {top}（已裁）；页面列 {x0}..{x1}")

    bs = bands(ref, x0, x1, bg, fg)
    sizes = args.sizes
    print(f"行投影检出 {len(bs)} 条带（期望 {len(sizes)}）: {[b[1]-b[0] for b in bs]}")
    if len(bs) > len(sizes):
        print("带数多于字号数——先确认页面布局（框间距是否够大）")
        return 1
    if len(bs) < len(sizes):
        print(f"一屏只显示 {len(bs)}/{len(sizes)} 框，本次只测前 {len(bs)} 个字号")
        sizes = sizes[:len(bs)]

    gv = [float(g[:1] + "." + g[1:]) for g in args.gammas]
    print(f"{'逻辑px':>7}{'物理px':>8}  " + "  ".join(f"γ{g:>5}" for g in args.gammas)
          + "   理想γ")
    print("-" * (18 + 8 * len(args.gammas)))
    for size, (y0, y1) in zip(sizes, bs):
        ref_ink = ink_of(np.asarray(ref.convert("RGB")), x0, x1, y0, y1, bg, fg)
        ratios = []
        for g in args.gammas:
            st = np.asarray(Image.open(f"{args.st_dir}/{args.tmpl.format(g=g, s=size)}")
                            .convert("RGB"))
            ratios.append(ink_of(st, 0, st.shape[1], 0, st.shape[0], bg, fg) / ref_ink
                          if ref_ink > 0 else float("nan"))
        ideal = None
        for k in range(len(ratios) - 1):
            if (ratios[k] - 1.0) * (ratios[k + 1] - 1.0) <= 0.0 and ratios[k] != ratios[k + 1]:
                t = (1.0 - ratios[k]) / (ratios[k + 1] - ratios[k])
                ideal = gv[k] + t * (gv[k + 1] - gv[k])
                break
        print(f"{size:>7}{size * 1.5:>8.1f}  " + "  ".join(f"{r:>6.3f}" for r in ratios)
              + f"   {('%.3f' % ideal) if ideal is not None else '  —  '}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
