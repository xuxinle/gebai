"""预混合指数拟合：在「code 空间混合」与「线性空间混合」之间，实测最优点在哪。

背景：
  霜天当前在 sRGB code 空间混合（`out = bg + (fg−bg)·α`）。物理正确的做法是在**线性光**
  空间混合，等价于把覆盖率重映射为 `α' = α^(1/2.2)`（`linear_to_srgb` 的幂次近似）。
  但两个角度的实测都表明浏览器**既不是纯 code 也不是纯线性**：
    - code 空间：霜天比浏览器重 +13.6%（14px CJK）
    - 全线性   ：霜天比浏览器轻 −5.0%
  这正是 Skia `SkMaskGamma` / ClearType 的立足点：它们施加的是**部分**gamma + 对比度拉伸。

做法：
  从霜天 PNG 反解出它使用的覆盖率 α（code 空间投影），施加 `α' = α^g`，
  按 `out = bg + (fg−bg)·α'` 重新合成，与浏览器 PNG 比：
    score_code   = 在 code 空间求 mean|Δ|（与 `text_preblend_fit.py` 同口径）
    score_linear = 在线性覆盖率口径下比 solid/mid/ink（几何+合成双重可比）
  扫 g ∈ [0.40, 1.00]，g=1 = 现状（code 空间），g≈0.455 = 全线性。

用法：python text_premix_fit.py <browser.png> <ours.png>
"""
import sys

import numpy as np
from PIL import Image

FG = np.array([0x0F, 0x17, 0x2A], dtype=float)
BG = np.array([0xFF, 0xFF, 0xFF], dtype=float)


def srgb_to_linear(v):
    v = np.clip(v, 0.0, 1.0)
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def luminance(rgb):
    lin = srgb_to_linear(rgb / 255.0)
    return 0.2126 * lin[..., 0] + 0.7152 * lin[..., 1] + 0.0722 * lin[..., 2]


def linear_coverage(rgb):
    y = luminance(rgb)
    return np.clip((float(luminance(BG)) - y) / (float(luminance(BG)) - float(luminance(FG))), 0, 1)


def code_coverage(rgb):
    d = BG - FG
    return np.clip(((BG - rgb) @ d) / float(d @ d), 0.0, 1.0)


def best_shift(a, b, span=6):
    best = (0, 0, 1e9)
    for dy in range(-span, span + 1):
        for dx in range(-span, span + 1):
            score = np.abs(a - np.roll(np.roll(b, dy, axis=0), dx, axis=1)).mean()
            if score < best[2]:
                best = (dx, dy, score)
    return best[0], best[1]


def premix(alpha, gamma):
    """把 code 空间的覆盖率 α 重映射，使其近似「线性空间混合」的结果。

    黑字白底、线性正确时：code' = linear_to_srgb(1−α) ≈ (1−α)^(1/gamma)。
    code 空间混合给出 code' = 1−α'，两者相等 ⇒ **α' = 1 − (1−α)^(1/gamma)**。
    所以 gamma=1 是现状（code 空间），gamma→2.2 接近全线性（更**浅**、更细）；
    注意方向：深色字在线性空间混合后变**浅**，故覆盖率必须**下降**
    （指导书 §1.3 里写的 `a_corrected = powf(a, 1/gamma)` 对黑字白底是反的，
     它会把字压得更黑；该式只适用于浅色字深底的一侧。）
    """
    return 1.0 - np.power(np.clip(1.0 - alpha, 0.0, 1.0), 1.0 / gamma)


def compose(alpha, fg, bg):
    """按覆盖率把前景色合成到背景上（逐通道；alpha 可为 H×W×1）。"""
    a = alpha[..., None] if alpha.ndim == 2 else alpha
    return bg[None, None, :] + (fg[None, None, :] - bg[None, None, :]) * a


def metrics(cov):
    solid = int((cov > 0.85).sum())
    mid = int(((cov > 0.15) & (cov < 0.85)).sum())
    return dict(solid=solid, mid=mid, ink=float(cov.sum()),
                ratio=(mid / solid if solid else -1.0))


if __name__ == "__main__":
    br = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(float)
    ours = np.asarray(Image.open(sys.argv[2]).convert("RGB")).astype(float)
    h = min(br.shape[0], ours.shape[0])
    w = min(br.shape[1], ours.shape[1])
    br, ours = br[:h, :w], ours[:h, :w]

    bands = [("14px CJK", 0, 45), ("13.5px 拉丁", 45, 87), ("12px 路径", 87, 117),
             ("13.5px 等宽", 117, 150)]
    gammas = [round(1.0 + 0.1 * i, 2) for i in range(13)]  # 1.0(=code 空间现状) .. 2.2(≈全线性)
    overall = {}
    for name, y0, y1 in bands:
        if y0 >= h:
            continue
        b = br[y0:min(y1, h)]
        o = ours[y0:min(y1, h)]
        oc_l, bc_l = linear_coverage(o), linear_coverage(b)
        dx, dy = best_shift(oc_l, bc_l)
        b = np.roll(b, (dy, dx), axis=(0, 1))
        bc_l = linear_coverage(b)
        # 反解霜天使用的覆盖率（code 空间口径），只保留有墨像素做拟合
        alpha0 = code_coverage(o)
        mask = (alpha0 > 0.02) | (bc_l > 0.02)
        _mb = metrics(bc_l)
        print(f"\n=== {name} 对齐 dx={dx} dy={dy}  ink 像素 {int(mask.sum())} ===")
        base_code = float(np.abs(compose(alpha0, FG, BG) - b)[mask].mean())
        print(f"  现状(g=1.00, code 空间混合)  code mean|Δ|={base_code:.4f}  "
              f"[linear] solid={_mb['solid']:5d} mid={_mb['mid']:5d} "
              f"ink={_mb['ink']:8.1f}  ← 浏览器")
        rows = []
        for g in gammas:
            a = premix(alpha0, g)
            synth = compose(a, FG, BG)
            score_code = float(np.abs(synth - b)[mask].mean())
            cov = linear_coverage(synth)
            m = metrics(cov)
            mb = metrics(bc_l)
            ink_dev = (m['ink'] / mb['ink'] - 1.0) * 100.0
            solid_dev = (m['solid'] / max(1, mb['solid']) - 1.0) * 100.0
            rows.append((g, score_code, ink_dev, solid_dev, m['ratio']))
            if g in (1.0, 1.4, 1.6, 1.8, 2.0, 2.2):
                print(f"    g={g:.2f}  code mean|Δ|={score_code:.4f} ({'基线' if g==1.0 else f'{(score_code/base_code-1)*100:+.1f}%'})  "
                      f"linear: ink {ink_dev:+6.1f}%  solid {solid_dev:+6.1f}%  mid/solid={m['ratio']:.3f}")
        best = min(rows, key=lambda r: r[1])
        best_ink = min(rows, key=lambda r: abs(r[2]))
        print(f"  → code 口径最优 g={best[0]:.2f}（mean|Δ|={best[1]:.4f}）; "
              f"墨量最贴近浏览器 g={best_ink[0]:.2f}（{best_ink[2]:+.1f}%）")
        overall[name] = best[0]
    print("\n各带 code 口径最优 g：", overall)
