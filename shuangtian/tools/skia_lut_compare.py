"""**Skia 的 mask gamma LUT 逐位复刻 + 与霜天 γ 的对照**（可复算，不依赖 C++ 构建）。

源头：`google/skia` @ 8643b1d `src/core/SkMaskGamma.cpp`
      `SkTMaskGamma_build_correcting_lut()`（全文 128 行，本文件把它逐行翻译成 Python）。

为什么要这个脚本（2026-10-04 的教训）：
调研 Skia 时我**凭代码结构推断**出「`adjustedContrast = contrast × linDst` 是随背景变暗而渐强」，
还据此写了结论。把公式算出来才发现**说反了**——`dst = 1 − src` 是“对背景的猜测”，
所以黑字时 `linDst = 1`（对比度全量生效）、白字时 `linDst = 0`（**对比度完全失效**）。
**读公式不算曲线，等于没读**；这个脚本就是那把尺子。

用法：
    python tools/skia_lut_compare.py            # 打印对照表
    python tools/skia_lut_compare.py --png out.png   # 额外画曲线图
"""

import sys

# ---------------------------------------------------------------- Skia 的构件

def srgb_to_linear(u: float) -> float:
    """SkSRGBColorSpaceLuminance::toLuma（sRGB 规范常数）。"""
    return u / 12.92 if u <= 0.04045 else ((u + 0.055) / 1.055) ** 2.4


def linear_to_srgb(u: float) -> float:
    """SkSRGBColorSpaceLuminance::fromLuma。"""
    return u * 12.92 if u <= 0.0031308 else 1.055 * (u ** (1 / 2.4)) - 0.055


def apply_contrast(a: float, contrast: float) -> float:
    """SkMaskGamma.cpp 的 apply_contrast：srca + (1−srca)·c·srca。"""
    return a + (1.0 - a) * contrast * a


def skia_lut(src_u8: int, contrast: float, device_gamma: float = 0.0) -> list[int]:
    """逐行复刻 SkTMaskGamma_build_correcting_lut，返回 256 项 LUT。

    device_gamma = 0.0 表示 sRGB（Skia 的 `SkColorSpaceLuminance::Fetch` 约定）。
    """
    to_luma = srgb_to_linear if device_gamma == 0.0 else (lambda v: v ** device_gamma)
    from_luma = linear_to_srgb if device_gamma == 0.0 else (lambda v: v ** (1 / device_gamma))

    src = src_u8 / 255.0
    lin_src = to_luma(src)
    dst = 1.0 - src                 # “对背景的猜测”：源的感知反色
    lin_dst = to_luma(dst)
    adjusted = contrast * lin_dst   # ← 关键的一行
    table = []
    for i in range(256):
        raw = i / 255.0
        srca = apply_contrast(raw, adjusted)
        dsta = 1.0 - srca
        out = from_luma(lin_src * srca + dsta * lin_dst)
        result = (out - dst) / (src - dst)   # 反解 blitter 会做的事
        table.append(max(0, min(255, round(255.0 * result))))
    return table


def gebai_lut(gamma: float) -> list[int]:
    """霜天的映射：α' = 1 − (1−α)^(1/γ)。"""
    return [max(0, min(255, round(255.0 * (1.0 - (1.0 - i / 255.0) ** (1.0 / gamma)))))
            for i in range(256)]


# ---------------------------------------------------------------- 对照输出

PROBE = [32, 64, 96, 128, 160, 192, 224, 255]


def main() -> int:
    rows = []
    for label, src in [("黑墨 / 白底 (src=0)", 0), ("白墨 / 黑底 (src=255)", 255)]:
        for contrast in (0.0, 0.25, 0.5, 1.0):
            rows.append((f"{label}  contrast={contrast:.2f}", skia_lut(src, contrast)))
    rows.append(("恒等（不校正）", list(range(256))))
    for gamma in (2.2, 1.0, 0.6):
        rows.append((f"霜天 γ={gamma}", gebai_lut(gamma)))

    print("输入 α（×255）      " + "".join(f"{v:>7}" for v in PROBE))
    for label, table in rows:
        print(f"{label:<26}" + "".join(f"{table[v]:>7}" for v in PROBE))

    print("\n--- 关键量：adjustedContrast = contrast × linDst ---")
    for src in (0, 255):
        lin_dst = srgb_to_linear(1.0 - src / 255.0)
        print(f"src={src:>3}  猜测背景 dst={255-src:>3}  linDst={lin_dst:.3f}  "
              f"⇒ contrast=1 时实际加成 {lin_dst:.3f}")
    print("\n判读：黑字时 linDst=1 ⇒ 对比度**全量生效**；白字时 linDst=0 ⇒ **完全失效**。")
    print("      所以 Skia 的对比度增强**只服务深字浅底**，不是“深色主题加更多墨”。")

    if "--png" in sys.argv:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        x = list(range(256))
        fig, ax = plt.subplots(figsize=(9, 5))
        ax.plot(x, skia_lut(0, 0.0), label="Skia 黑墨/白底 contrast=0（= 线性反解）")
        ax.plot(x, skia_lut(0, 1.0), label="Skia 黑墨/白底 contrast=1")
        ax.plot(x, skia_lut(255, 0.0), label="Skia 白墨/黑底 contrast=0")
        ax.plot(x, x, "--", label="恒等")
        ax.plot(x, gebai_lut(0.6), label="霜天 γ=0.6")
        ax.plot(x, gebai_lut(2.2), label="霜天 γ=2.2")
        ax.set_xlabel("输入覆盖率 α (×255)")
        ax.set_ylabel("输出 α (×255)")
        ax.legend()
        ax.grid(True, alpha=0.3)
        out = sys.argv[sys.argv.index("--png") + 1]
        fig.savefig(out, dpi=110, bbox_inches="tight")
        print(f"\n[OK] 曲线图 → {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
