"""受控字号阶梯的「字号 → 理想 gamma」测量（霜天 vs 浏览器）。

与 `tools/text_ab_diff.py` 同一覆盖率口径，但作用在 `gen_text_ab_sizes.py` 生成的
**同字同串、只变字号**的对照页上——这样逐行差异只来自字号，不混入内容差异。

对每个字号：先取浏览器/霜天两侧的墨迹紧包围盒（去掉排版位置差），
再算「墨量比 = Σ覆盖 / Σ覆盖」。理想 gamma 由该字号在不同 gamma 下的
墨量比插值求根（比值 = 1 的那个 gamma）。

用法：python tools/size_gamma_fit.py <gamma=值>=<png> ...
"""

import json
import sys

import numpy as np
from PIL import Image

FG = np.array([0x0F, 0x17, 0x2A], dtype=float)
BG = np.array([255.0, 255.0, 255.0])


def cov(path, y0, y1):
    """**固定行带**的覆盖率图（两侧用同一个 y 区间）。

    不按各自墨迹包围盒裁剪：两侧的字形高度不同（霜天 82% 于浏览器——那是**行高/em 盒**
    差异，不是字形差异），各自裁剪会让积分区间不同，墨量比被“盒高差”污染
    （实测踩到：所有比值为 0.81~0.89，看似“霜天偏轻”，其实是量尺切短了）。
    """
    a = np.asarray(Image.open(path).convert("RGB")).astype(float)[y0:y1, :, :]
    d = BG - FG
    c = np.clip(((BG - a) @ d) / float(d @ d), 0.0, 1.0)
    # 水平仍按墨迹裁（宽度比 ≈ 1.005，几乎无偏差），垂直**不裁**。
    rx = np.where((c > 0.12).any(axis=0))[0]
    if rx.size == 0:
        return None
    return c[:, rx[0]:rx[-1] + 1]


def main() -> int:
    boxes_path = sys.argv[1]
    browser = sys.argv[2]
    runs = [a.split("=", 1) for a in sys.argv[3:]]   # [(gamma, png)]
    boxes = json.loads(open(boxes_path).read())
    sizes = [10, 11, 12, 13, 14, 15, 16, 18, 20, 24, 28, 32]

    print(f"{'逻辑px':>7}{'物理px':>7}  " + "  ".join(f"γ{r[0]:>4}" for r in runs) + "   理想γ")
    print("-" * 84)
    for i, size in enumerate(sizes):
        y0, y1 = boxes[i][1], boxes[i][2]
        ref = cov(browser, y0, y1)
        if ref is None:
            continue
        ink_ref = ref.sum()
        ratios = []
        for _g, path in runs:
            a = cov(path, y0, y1)
            ratios.append(a.sum() / ink_ref if a is not None else float("nan"))
        # 插值求根：比值 = 1 处的 gamma（在相邻两点间线性插值）
        gammas = [float(g) for g, _ in runs]
        ideal = None
        for k in range(len(ratios) - 1):
            lo, hi = ratios[k], ratios[k + 1]
            if (lo - 1.0) * (hi - 1.0) <= 0.0 and lo != hi:
                t = (1.0 - lo) / (hi - lo)
                ideal = gammas[k] + t * (gammas[k + 1] - gammas[k])
                break
        row = f"{size:>7}{size * 1.5:>7.1f}  " + "  ".join(f"{r:>6.3f}" for r in ratios)
        row += f"   {('%.3f' % ideal) if ideal is not None else '  —  '}"
        print(row)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
