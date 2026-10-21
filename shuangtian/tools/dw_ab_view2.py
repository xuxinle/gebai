#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""最易读的观感对比：同一样本，多路径**上下紧贴叠放**（相邻行对比最准）。

产物（build/dwab/figs/）：
  cmp-stack-light.png   浅底：每个样本 4 行紧贴（霜天默认 / taps收窄 / DWrite / 浏览器）
  cmp-stack-dark.png    深底：同上（霜天 / DWrite）
  cmp-blend.png         红蓝叠加：霜天=红、浏览器=蓝；紫=一致，看得到"软在哪儿"
  cmp-stroke.png        单条笔画横向剖面（数值化之前先给人眼看的形状）

用法：python tools/dw_ab_view2.py   （无参数；与 dw_ab_view.py 互补）
"""
import json
import pathlib

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
D = ROOT / "build" / "dwab"
FIGS = D / "figs"
FIGS.mkdir(exist_ok=True)
FONT, FONT_B = "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/msyhbd.ttc"


def font(sz, bold=False):
    try:
        return ImageFont.truetype(FONT_B if bold else FONT, sz)
    except Exception:
        return ImageFont.load_default()


def load(p):
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.uint8)


def cov(arr, fg, bg):
    d = np.asarray(fg, float) - np.asarray(bg, float)
    return np.clip(((arr.astype(np.float64) - bg) * d).sum(2) / float((d * d).sum()), 0, 1)


def bands(t, min_ink=1.0, gap=2):
    prof = t.sum(1)
    rows = np.where(prof > min_ink)[0]
    if rows.size == 0:
        return []
    out, s, p = [], rows[0], rows[0]
    for y in rows[1:]:
        if y - p > gap:
            out.append((int(s), int(p) + 1)); s = y
        p = y
    out.append((int(s), int(p) + 1))
    return out


def htrim(t, b):
    c = np.where(t[b[0]:b[1]].sum(0) > 0.5)[0]
    return (int(c[0]), int(c[-1]) + 1) if c.size else None


LIGHT = [("霜天 · 应用默认", "st-light-default.png", (216, 39, 40)),
         ("霜天 · taps 收窄", "stsweep-0_51_205_51_0-g1.0.png", (44, 160, 44)),
         ("DWrite 直调", "dw-natural-light.ppm", (31, 119, 180)),
         ("浏览器 · 真窗口", "br-clean-light.png", (148, 103, 189))]
DARK = [("霜天 · 应用默认", "st-dark.png", (216, 39, 40)),
        ("DWrite 直调", "dw-natural-dark.ppm", (31, 119, 180))]

SAMPLES = ["han-body", "han-dense", "latin-body", "digits", "mixed"]


def stack(sources, theme, out_name, bg, fg, zoom, shade):
    imgs, bs, cs = {}, {}, {}
    for name, f, color in sources:
        p = D / f
        if not p.exists():
            print("缺", p); continue
        imgs[name] = load(p)
        cs[name] = cov(imgs[name], fg, bg)
        bs[name] = bands(cs[name])
    meta = json.loads((D / f"rows-{theme}.json").read_text(encoding="utf-8"))
    li = {m["label"]: i for i, m in enumerate(meta["rows"])}
    ref = list(imgs)[-1]

    # 每个样本：取所有路径里最宽的那一版宽度
    prepared = []
    for lab in SAMPLES:
        if li.get(lab, 10**9) >= len(bs[ref]):
            continue
        per = []
        for name in imgs:
            b = bs[name][li[lab]]
            h = htrim(cs[name], b)
            if h is None:
                continue
            per.append((name, imgs[name][b[0]:b[1], h[0]:h[1]], sources and
                        [c for n, f, c in sources if n == name][0]))
        if per:
            wmax = max(c.shape[1] for _, c, _ in per)
            hmax = max(c.shape[0] for _, c, _ in per)
            items = []
            for name, c, color in per:
                pad_h = hmax - c.shape[0]
                cc = np.pad(c, ((0, pad_h), (0, 0), (0, 0)), mode="edge")
                pad_w = wmax - cc.shape[1]
                cc = np.pad(cc, ((0, 0), (0, pad_w), (0, 0)), mode="edge")
                items.append((name, cc, color))
            prepared.append((lab, items, hmax, wmax))

    lab_w = 150
    line_h = 3
    gap = 22
    total_h = 84 + sum((h * zoom + line_h) * len(it) + gap for _, it, h, _ in prepared) + 44
    total_w = lab_w + max(w * zoom for _, _, _, w in prepared) + 26
    canvas = Image.new("RGB", (total_w, total_h), bg)
    dr = ImageDraw.Draw(canvas)
    dr.text((14, 12), "同一段文字 · 四种渲染：上下紧贴叠放（相邻两行最容易看出厚薄）",
            font=font(23, True), fill=fg)
    dr.text((14, 46), f"{zoom}× 像素放大（NEAREST，不插值）。每组从上到下 = "
                      + " → ".join(n for n, _, _ in sources),
            font=font(12), fill=shade)

    y = 76
    for lab, items, hmax, wmax in prepared:
        block_h = (hmax * zoom + line_h) * len(items)
        dr.text((8, y + block_h // 2 - 8), lab, font=font(12), fill=shade)
        for i, (name, arr, color) in enumerate(items):
            tile = Image.fromarray(arr).resize((arr.shape[1] * zoom, arr.shape[0] * zoom), Image.NEAREST)
            x = lab_w
            canvas.paste(tile, (x, y))
            # 左侧色条标识路径
            dr.rectangle([x - 5, y, x - 2, y + tile.height], fill=color)
            y += tile.height + line_h
        y += gap
    dr.text((14, total_h - 34), "说明：亮度差、笔画芯部是否发灰、字间距是否有「虚」的边缘——"
                                "这三项在三、四行（DWrite / 浏览器）明显更硬。",
            font=font(12), fill=shade)
    canvas.save(FIGS / out_name)
    print("wrote", FIGS / out_name, canvas.size)


def blend(out_name):
    """红蓝叠加：霜天=红通道，浏览器=蓝通道。紫=一致；偏红=霜天更浓；偏青=霜天更淡。"""
    st = load(D / "st-light-default.png")
    br = load(D / "br-clean-light.png")
    bg, fg = np.array([255., 255., 255.]), np.array([15., 23., 42.])
    ts, tb = cov(st, fg, bg), cov(br, fg, bg)
    h = min(ts.shape[0], tb.shape[0]); w = min(ts.shape[1], tb.shape[1])
    ts, tb = ts[:h, :w], tb[:h, :w]
    # 正确的双色叠加（墨水在白底上的语义）：
    #   R = 255 - 255*tb   （蓝墨水吃掉红通道）
    #   B = 255 - 255*ts   （红墨水吃掉蓝通道）
    #   G = 255 - 255*max(ts,tb)
    # 两边都满墨 → 黑；只有霜天有墨 → 红；只有浏览器有墨 → 蓝；都没有 → 白。
    r = 255 - 255 * tb
    b = 255 - 255 * ts
    g = 255 - 255 * np.maximum(ts, tb)
    rgb = np.stack([r, g, b], axis=2).clip(0, 255).astype(np.uint8)

    meta = json.loads((D / "rows-light.json").read_text(encoding="utf-8"))
    li = {m["label"]: i for i, m in enumerate(meta["rows"])}
    bs = bands(ts)
    picks = ["han-body", "han-dense", "latin-body"]
    zoom = 6
    lab_w = 150
    blocks = []
    for lab in picks:
        i = li[lab]
        if i >= len(bs):
            continue
        b0 = bs[i]
        blocks.append((lab, rgb[b0[0] - 2:b0[1] + 2, :520]))
    tw = max(c.shape[1] for _, c in blocks) * zoom
    th = sum((c.shape[0] * zoom + 30) for _, c in blocks)
    canvas = Image.new("RGB", (lab_w + tw + 24, th + 118), (255, 255, 255))
    dr = ImageDraw.Draw(canvas)
    dr.text((14, 12), "红蓝叠加：哪里不一样，一眼看出", font=font(24, True), fill=(15, 23, 42))
    dr.text((14, 48), "黑 = 两者重合的墨；红 = 只有霜天有墨（霜天更浓）；蓝 = 只有浏览器有墨（霜天更淡）。"
                      "看笔画边缘：红蓝分居两侧 = 霜天把墨摊到了邻边，而不是同一处变淡。",
            font=font(12), fill=(90, 90, 90))
    y = 88
    for lab, c in blocks:
        dr.text((8, y + 8), lab, font=font(12), fill=(130, 130, 130))
        tile = Image.fromarray(c).resize((c.shape[1] * zoom, c.shape[0] * zoom), Image.NEAREST)
        canvas.paste(tile, (lab_w, y))
        y += tile.height + 30
    canvas.save(FIGS / out_name)
    print("wrote", FIGS / out_name, canvas.size)


def glyph_diff(out_name, zoom=7):
    """单字：四种渲染 + 差值热图。不做任何行/列挑选，完全稳健。"""
    bg, fg = np.array([255., 255., 255.]), np.array([15., 23., 42.])
    srcs = [("霜天 · 应用默认", "st-light-default.png"),
            ("霜天 · taps 收窄", "stsweep-0_51_205_51_0-g1.0.png"),
            ("DWrite 直调", "dw-natural-light.ppm"),
            ("浏览器 · 真窗口", "br-clean-light.png")]
    meta = json.loads((D / "rows-light.json").read_text(encoding="utf-8"))
    li = {m["label"]: i for i, m in enumerate(meta["rows"])}

    samples = [("han-dense", 2), ("han-body", 2), ("digits", 3)]
    panels = []   # (lab, name, rgb_array)
    for lab, ng in samples:
        i = li[lab]
        text = meta["rows"][i]["text"]
        covs = {}
        crops = {}
        for name, f in srcs:
            p = D / f
            if not p.exists():
                continue
            arr = load(p)
            t = cov(arr, fg, bg)
            b = bands(t)[i]
            h = htrim(t, b)
            full = arr[b[0]:b[1], h[0]:h[1]]
            per = full.shape[1] / max(len(text), 1)
            cut = int(round(per * ng))
            crops[name] = full[:, :cut]
            covs[name] = cov(full[:, :cut], fg, bg)
        w = min(c.shape[1] for c in crops.values())
        hgt = min(c.shape[0] for c in crops.values())
        for name, _ in srcs:
            if name in crops:
                panels.append((lab, name, crops[name][:hgt, :w]))
        # 差值热图：霜天默认 − 浏览器
        a = covs.get("霜天 · 应用默认")
        b = covs.get("浏览器 · 真窗口")
        if a is not None and b is not None:
            d = a[:hgt, :w] - b[:hgt, :w]
            lim = max(0.35, float(np.abs(d).max()))
            norm = np.clip(d / lim, -1, 1)
            # 负（霜天更淡）= 蓝，正（霜天更浓）= 红
            rgb = np.zeros((*norm.shape, 3))
            rgb[..., 0] = 255 - 200 * np.clip(-norm, 0, 1)   # 蓝处降红
            rgb[..., 1] = 255 - 200 * np.abs(norm) * 0.55
            rgb[..., 2] = 255 - 200 * np.clip(norm, 0, 1)    # 红处降蓝
            panels.append((lab, "差值：霜天−浏览器", rgb.clip(0, 255).astype(np.uint8)))

    from collections import OrderedDict
    by_lab = OrderedDict()
    for lab, name, arr in panels:
        by_lab.setdefault(lab, []).append((name, arr))

    label_w, gap, sep = 132, 18, 24
    col_w = max(max(a.shape[1] for _, a in v) for v in by_lab.values()) * zoom
    row_h = max(max(a.shape[0] for _, a in v) for v in by_lab.values()) * zoom
    ncol = max(len(v) for v in by_lab.values())

    W = label_w + ncol * (col_w + sep) + sep
    H = 118 + ncol and 118 + len(by_lab) * (row_h + 46) + 40
    canvas = Image.new("RGB", (W, H), (255, 255, 255))
    dr = ImageDraw.Draw(canvas)
    dr.text((14, 12), "单字放大 + 差值热图：霜天的墨到底差在哪里", font=font(24, True), fill=(15, 23, 42))
    dr.text((14, 50), "上四行 = 四种渲染（7× 像素放大）；最后一行 = 霜天减浏览器的覆盖率差"
                      "（红 = 霜天更浓，蓝 = 霜天更淡，白 = 一致）。", font=font(12), fill=(90, 90, 90))

    # 列头（按第一组样本）
    first = list(by_lab.values())[0]
    for ci, (name, _) in enumerate(first):
        dr.text((label_w + ci * (col_w + sep), 84), name, font=font(13, True),
                fill=(216, 39, 40) if "差值" in name else (15, 23, 42))

    y = 112
    for lab, items in by_lab.items():
        dr.text((6, y + row_h // 2 - 8), lab, font=font(12), fill=(130, 130, 130))
        for ci, (name, arr) in enumerate(items):
            tile = Image.fromarray(arr).resize((arr.shape[1] * zoom, arr.shape[0] * zoom), Image.NEAREST)
            x = label_w + ci * (col_w + sep)
            if ci % 2 == 0:
                dr.rectangle([x - 5, y - 5, x + col_w + 5, y + row_h + 5], fill=(248, 248, 250))
        for ci, (name, arr) in enumerate(items):
            tile = Image.fromarray(arr).resize((arr.shape[1] * zoom, arr.shape[0] * zoom), Image.NEAREST)
            canvas.paste(tile, (label_w + ci * (col_w + sep), y))
        y += row_h + 46
    dr.text((14, H - 30), "差值图里的蓝色集中在哪里，就是霜天“没到的那些墨”的位置。",
            font=font(12), fill=(110, 110, 110))
    canvas.save(FIGS / out_name)
    print("wrote", FIGS / out_name, canvas.size)


def cross_section(out_name, zoom=1):
    """笔画横截面：横向扫过一条竖笔画，把覆盖率画成曲线。看"坡"陡不陡。

    这是给人眼看的"形状"而不是数字：陡坡=硬，缓坡=糊。
    """
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib import font_manager
    matplotlib.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei", "DejaVu Sans"]
    matplotlib.rcParams["axes.unicode_minus"] = False

    bg, fg = np.array([255., 255., 255.]), np.array([15., 23., 42.])
    srcs = [("霜天 应用默认", "st-light-default.png", "#d62728", 2.6),
            ("霜天 taps 收窄", "stsweep-0_51_205_51_0-g1.0.png", "#2ca02c", 2.6),
            ("DWrite 直调", "dw-natural-light.ppm", "#1f77b4", 2.6),
            ("浏览器 真窗口", "br-clean-light.png", "#9467bd", 2.6)]
    meta = json.loads((D / "rows-light.json").read_text(encoding="utf-8"))
    li = {m["label"]: i for i, m in enumerate(meta["rows"])}

    fig, axes = plt.subplots(1, 2, figsize=(14, 4.6), dpi=110)
    fig.suptitle("笔画横截面：同一笔画，把覆盖率画成“山丘”——坡越陡 = 越硬",
                 fontsize=15, fontweight="bold")

    for ax, lab in zip(axes, ["latin-thin", "han-thin"]):
        i = li[lab]
        curves = {}
        hh = None
        for name, f, color, lw in srcs:
            p = D / f
            if not p.exists():
                continue
            t = cov(load(p), fg, bg)
            bs = bands(t)
            if i >= len(bs):
                continue
            seg = t[bs[i][0]:bs[i][1]]
            curves[name] = (seg, color, lw)
            hh = seg.shape[0] if hh is None else min(hh, seg.shape[0])
        if not curves:
            continue
        names = list(curves)
        ref = curves[names[-1]][0][:hh]

        # **自动找差异最大的一行**：逐行算“四条曲线之间的极差”，取极差最大的那一行。
        # （固定取“墨量最大行”往往落在实心区，四条重合、没有信息量——改掉）
        # **只看行首前 44px**：长串拉丁（"illumination minimal"）两侧 advance 微差会累积，
        # 行末差好几个像素，那比的就是“字距”而不是“渲染”。行首不受影响。
        W = min(44, ref.shape[1])
        stack = np.stack([curves[n][0][:hh, :W] for n in names])
        spread_row = (stack.max(0) - stack.min(0)).mean(1)
        yy = int(np.argmax(spread_row))
        row_stack = stack[:, yy, :]
        spread_col = row_stack.max(0) - row_stack.min(0)
        x_peak = int(np.argmax(spread_col))
        lo, hi = max(0, x_peak - 7), min(W, x_peak + 9)
        cols = np.arange(W)

        for name in names:
            seg, color, lw = curves[name]
            ax.plot(cols[lo:hi], seg[yy, lo:hi], "o-", lw=lw, ms=5, color=color,
                    label=name, alpha=0.92)
        ax.set_title(f"样本：{lab}　自动定位差异最大处（第 {yy} 行，x={lo}~{hi}）", fontsize=11.5)
        ax.axhline(1.0, color="#333", ls=":", lw=1.2)
        ax.text(ax.get_xlim()[0], 1.015, "满黑 1.0", fontsize=8.5, color="#666")
        ax.set_xlabel("水平像素位置", fontsize=10)
        ax.set_ylabel("覆盖率（该像素多黑）", fontsize=10)
        ax.set_ylim(-0.03, 1.05)
        ax.grid(alpha=0.25)
        if lab == "latin-thin":
            ax.legend(fontsize=9.5, loc="lower left")

    fig.text(0.5, 0.015, "曲线顶到 1.0 且两侧边坡陡 = 笔画芯实、边缘干净；"
                          "顶点停在 0.87 附近、边坡宽缓 = 芯没到满黑（红/紫线正是这样）。",
             ha="center", fontsize=10.5, color="#444")
    fig.tight_layout(rect=(0, 0.04, 1, 0.93))
    fig.savefig(FIGS / out_name, facecolor="white")
    plt.close(fig)
    print("wrote", FIGS / out_name)


if __name__ == "__main__":
    stack(LIGHT, "light", "cmp-stack-light.png", (255, 255, 255), (15, 23, 42), 3, (110, 110, 110))
    stack(DARK, "dark", "cmp-stack-dark.png", (15, 17, 21), (232, 238, 249), 3, (160, 165, 175))
    blend("cmp-blend.png")
    glyph_diff("cmp-glyph-diff.png")
