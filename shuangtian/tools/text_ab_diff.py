"""文字渲染 A/B 逐行对比（霜天 vs 浏览器）——**两侧各自按墨迹投影自动切行**。

## 为什么不能用任何"坐标对齐"

霜天与浏览器的**基线算法不同**（霜天 `draw(origin)` 的 origin 是文本框顶、基线由内部加
ascent；浏览器 `top:` 放行盒顶边），偏移随字号变化、不是常数。前两版报告先后栽在
"按逻辑 top 切带"与"按固定窗口切带"上，症状都是某几行**只切到半个字**（字高 5px、
峰值 0.00、墨量比 0.00~2.8 这种离谱值）——那是**量尺切错**，不是渲染差异。

本版改为：对每一侧单独做**行投影剖面**（每物理行的墨迹总量），找出被空白行隔开的
连续带，再**按顺序配对**（两侧都是同一份 28 行表，顺序必然一致）。这样：
  · 版面差异（行位置/行高）——刻意不比，它不属于"字体渲染"
  · 字形差异（笔画粗细/锐度/覆盖）——本报告量的正是它

## 口径

  · 画布 700×670 逻辑 px、deviceScaleFactor 1.5 → 1050×1005 物理像素
  · 覆盖率 = 像素投影到 [白底, 该行前景色] 连线上的 0..1 参数，**逐通道投影**
    （亚像素渲染下 R/G/B 被不同强度点亮，只看亮度会把"彩边"读成"覆盖率低"）
  · **前景色按行取**——用正文色量 muted/faint/primary 会把"颜色本来就浅"读成"没到满黑"

判据：peak 满黑 / solid >0.85 / mid 过渡带（越低越锐）/ ink 墨量 / 显著差 >0.15 占比
用法：python tools/text_ab_diff.py <霜天PNG> <浏览器PNG>
"""

import json
import pathlib
import sys

import numpy as np
from PIL import Image

TEXT, MUTED, FAINT, PRIMARY = (
    (0x0F, 0x17, 0x2A), (0x56, 0x64, 0x7C), (0x66, 0x76, 0x8C), (0x25, 0x63, 0xEB))

# 行表（顺序必须与 tools/text_ab_page.html 和 text_ab_page_probe.cpp 完全一致）
ROWS = [
    (TEXT, "中文标题（常规）", False), (TEXT, "英数符号 Il1|O0", False),
    (TEXT, "中文标点", False), (TEXT, "箭头/几何/星号符号", False),
    (TEXT, "路径与冒号", False), (TEXT, "全角字符", False),
    (TEXT, "font_xs 12档", False), (TEXT, "font_sm 13档", False),
    (TEXT, "font_base 15档", False), (TEXT, "font_lg 17档", False),
    (TEXT, "font_xl 20档", False),
    (TEXT, "色 text", False), (MUTED, "色 muted", False),
    (FAINT, "色 faint", False), (PRIMARY, "色 primary", False),
    (TEXT, "字重 Regular", False), (TEXT, "字重 Bold", False),
    (TEXT, "字重 SemiBold(霜天=合成)", True),
    (TEXT, "等宽代码", False), (TEXT, "等宽命名空间", False),
    (TEXT, "等宽数字符号", False), (TEXT, "等宽中文注释", False),
    (TEXT, "11px 小字", False), (TEXT, "12px 小字", False), (TEXT, "13px 小字", False),
    (TEXT, "连字 AVATAR fi fl", False), (TEXT, "行高第一行", False),
    (TEXT, "行高第二行", False),
]

BG = np.array([255.0, 255.0, 255.0])


def load(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(float)


def coverage(sub, fg):
    d = BG - np.array(fg, dtype=float)
    return np.clip(((BG - sub) @ d) / float(d @ d), 0.0, 1.0)


def ink_bands(img, min_ink=0.08, gap=2):
    """行投影剖面切带：每物理行的墨迹量 > 阈值即视为有字，被空白行隔开成带。

    阈值取 `每行平均暗度`（对整幅画布宽度归一），不是绝对墨量：整行 1050px 里
    文字只占前几百 px，用大阈值（如 0.35）会**一带也切不出**（实测踩到：
    全文宽归一后剖面峰值只有 0.18）。行距必须 > 最大字号 × 1.5，否则相邻行的
    上伸部/下伸部会连成一带（实测 22px 行距只切出 12~17 带）。

    `gap` 允许 1~2 行空隙（下伸部与上伸部之间的细小断裂不应把一行切成两带）。
    """
    prof = ((255.0 - img) / 255.0).sum(axis=(1, 2)) / img.shape[1]   # 每行平均暗度
    on = prof > min_ink
    bands = []
    start = None
    blanks = 0
    for y, flag in enumerate(on):
        if flag:
            if start is None:
                start = y
            blanks = 0
        elif start is not None:
            blanks += 1
            if blanks > gap:
                bands.append((start, y - blanks + 1))
                start = None
                blanks = 0
    if start is not None:
        bands.append((start, len(on)))
    return bands


def stats(c):
    return (int((c > 0.85).sum()), int(((c > 0.15) & (c < 0.85)).sum()), float(c.sum()))


def main() -> int:
    st_img, br_img = load(sys.argv[1]), load(sys.argv[2])
    if st_img.shape != br_img.shape:
        print(f"尺寸不同：{st_img.shape} vs {br_img.shape}")
        return 1

    # 行带：**直接读浏览器 DOM 实测的元素框**（playwright 导出，见附件 `boxes.json`）。
    # 不自动切带：墨迹剖面的空白间距两侧不同（实测 29/30 带 vs 28 行），
    # 按带配对会错位——而错位导致的差异会被读成“渲染差异”（前几版就栽在这里）。
    boxes_path = pathlib.Path(sys.argv[3]) if len(sys.argv) > 3 else pathlib.Path(__file__).parent.parent / "build/probe/ab3/boxes.json"
    boxes = json.loads(boxes_path.read_text())
    if len(boxes) != len(ROWS):
        print(f"行框数 {len(boxes)} != 行表 {len(ROWS)}：先重跑对照页导出")
        return 1

    print(f"行带：浏览器 DOM 实测 {len(boxes)} 条（不自动切带）\n")
    print(f"{'行':<24}{'字高 霜/浏':>11}{'峰 霜/浏':>11}{'实心 霜天/浏览':>15}"
          f"{'过渡 霜天/浏览':>16}{'墨量比':>8}{'显著差':>8}")
    print("-" * 96)
    rows = []
    for i, (fg, label, expected) in enumerate(ROWS):
        y0, y1 = boxes[i][1], boxes[i][2]
        a = coverage(st_img[y0:y1, :, :], fg)
        b = coverage(br_img[y0:y1, :, :], fg)
        # 各自取墨迹紧包围盒（仅**水平**），只比“字的形状”，不比排版位置。
        #
        # ⚠ **垂直不能按各自包围盒裁剪**（实测踩到）：两侧字形盒高不同（霜天 ≈ 浏览器的
        # 82%——那是行高/em 盒差异，不是字形差异），各自裁剪再取 `min` 会把**较高那侧的墨
        # 截掉**，于是较低那侧被低估、比值系统性偏高（实测把所有行都抬到 1.03~1.33，
        # 据此得出的“小字偏重”结论是**量尺偏差**而非渲染事实）。
        def tight(c):
            rx = np.where((c > 0.12).any(axis=0))[0]
            if rx.size == 0:
                return None
            return c[:, rx[0]:rx[-1] + 1]
        ta, tb = tight(a), tight(b)
        if ta is None or tb is None:
            print(f"{label:<24}—— 一侧无墨迹（霜天={'有' if ta is not None else '无'}）")
            continue
        w = min(ta.shape[1], tb.shape[1])
        a2, b2 = ta[:, :w], tb[:, :w]
        sa, ma, ia = stats(a2)
        sb_, mb, ib = stats(b2)
        sig = float((np.abs(a2 - b2) > 0.15).mean()) * 100.0
        rows.append((label, sa, sb_, ma, mb, ia, ib, sig, expected))
        mark = " *" if expected else ""
        print(f"{label + mark:<24}{ta.shape[0]:>5}/{tb.shape[0]:<5}"
              f"{a2.max():>5.2f}/{b2.max():<5.2f}{sa:>7}/{sb_:<7}{ma:>7}/{mb:<8}"
              f"{ia / ib if ib else 0:>8.2f}{sig:>7.1f}%")
    if not rows:
        return 1
    ta_, tb_ = sum(r[5] for r in rows), sum(r[6] for r in rows)
    print("-" * 96)
    print(f"合计墨量：霜天 {ta_:.0f} / 浏览器 {tb_:.0f}   比值 {ta_ / tb_ if tb_ else 0:.3f}")
    print(f"平均显著差异：{np.mean([r[7] for r in rows]):.1f}%   "
          f"中位数 {np.median([r[7] for r in rows]):.1f}%")
    print("（* = 预期有差异：霜天无 SemiBold 字体面，走合成加粗）")
    print("差异最大的 6 行：")
    for r in sorted(rows, key=lambda r: -r[7])[:6]:
        print(f"  {r[0]:<24} 显著差 {r[7]:>5.1f}%   墨量比 {r[5] / r[6] if r[6] else 0:.2f}"
              f"   过渡/实心 霜天 {r[3] / r[1] if r[1] else 0:.3f}"
              f" 浏览 {r[4] / r[2] if r[2] else 0:.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
