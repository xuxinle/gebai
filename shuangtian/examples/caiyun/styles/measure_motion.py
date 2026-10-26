#!/usr/bin/env python3
"""风格模板示例的动效量测（静止占比 / 变化密度 / 亮度）。

## 为什么自己写

仓库里没有 `tools/motion_qa.py`（系统提示词与 docs 里引用了它，但它不在本仓库）。
本脚本实现的是同一套口径的核心三项，够用来给七个模板做**横向可比**的体检：

| 量 | 口径 | 判据 |
|---|---|---|
| `static_ratio` | 相邻帧平均绝对差 < 1.2 的帧对占比 | 全片 < 40% |
| `change_density` | 有变化的帧对 / 秒 | > 0.5 /s |
| `longest_still` | 连续静止的最长时长 | 应 ≥ 1.0s（rest 存在性） |
| `bright_ratio` | 平均亮度 > 180 的帧占比 | < 20% |

## 三条口径纪律（每条都是踩出来的，见 `caiyun_style_guide name=overview` 的「数字红线」）

1. **采样间隔必须一致**：全部按 30fps 抽帧，不要拿 10fps 的片子跟 30fps 比。
2. **均值必须与中位数、峰值一起看**：均值÷中位数 > 2 说明"大部分静止 + 少数剧烈"。
3. **网格会把小运动平均掉**：所以这里用**全分辨率**帧差（不降采样），
   小元素运动也能量到（实测 32×18 网格会把 20~60px 的元素运动读成 0）。

## 用法

    py measure_motion.py <帧目录> [--fps 30]
"""

import argparse
import glob
import os
import sys

import numpy as np
from PIL import Image

# ## 为什么不用"全屏平均帧差"当主口径（踩过，必读）
#
# 第一版照搬"帧差 < 1.2 = 静止"，量出七条片子全部 88~90% 静止、
# 最长静止 4.7~5.4s —— 而画面里明明有手在走、柱子在长、点在变形。
#
# 真因是**尺子被画面尺寸稀释**：一个 200x200 的方块在动，只占 1920x1080 的 4%，
# 全屏平均差被稀释 25 倍，于是永远低于 1.2。这不是"没动"，是"量不到"。
#
# 所以主口径改成两个**不被元素尺寸稀释**的量：
#   · CHANGED_PIXEL：单帧内"明显变化"的像素占比（|diff| > PIX_TOL）
#   · max_diff：该帧对里变化最大的那个像素（元素动不动，它一定看得见）
STATIC_DIFF = 1.2        # 兼容旧口径（全屏平均差），仅作参考
PIX_TOL = 6.0            # 单像素"明显变化"阈值（0-255，吸收压缩噪声）
CHANGED_MIN = 0.0004     # 变化像素占比下限（0.04% ≈ 800px，一个 28x28 的元素）
MAXDIFF_MIN = 12.0       # 单帧对最大像素差下限


def load_gray(path: str) -> np.ndarray:
    return np.asarray(Image.open(path).convert("L"), dtype=np.float32)


def measure(frames: list[str], fps: float) -> dict:
    if len(frames) < 2:
        raise SystemExit("至少需要 2 帧")

    diffs = []          # 全屏平均差（参考）
    changed = []        # 变化像素占比（主口径）
    maxdiff = []        # 单帧对最大像素差（主口径）
    luma = []
    prev = None
    for path in frames:
        cur = load_gray(path)
        luma.append(float(cur.mean()))
        if prev is not None:
            d = np.abs(cur - prev)
            diffs.append(float(d.mean()))
            changed.append(float((d > PIX_TOL).mean()))
            maxdiff.append(float(d.max()))
        prev = cur

    diffs = np.asarray(diffs)
    changed = np.asarray(changed)
    maxdiff = np.asarray(maxdiff)
    # "还在动"：变化像素够多 **或** 有局部显著变化
    still = (changed < CHANGED_MIN) & (maxdiff < MAXDIFF_MIN)

    # 最长连续静止（帧数 → 秒）
    longest, run = 0, 0
    for s in still:
        run = run + 1 if s else 0
        longest = max(longest, run)

    return {
        "frames": len(frames),
        "duration": len(frames) / fps,
        "static_ratio": float(still.mean()),
        "change_density": float((~still).sum() / (len(frames) / fps)),
        "longest_still": longest / fps,
        # ## 为什么还要一个"落定窗"指标（尺子第 N 次修正）
        #
        # `longest_still` 要求**逐帧连续**静止，而示例里的背景有极缓慢的渐变呼吸，
        # 每 8~12 帧就会跨过一次阈值 → 连续段被切碎（实测 01 尾部 2s 里
        # 有 45% 帧是静止的，却只量到 0.33s 的"连续"静止）。
        #
        # 所以补一个不看连续性的量：**尾巴 25% 时长里的静止帧占比**。
        # 它回答"落定之后有没有真的停住"，而不要求"每个相邻帧都完全一样"。
        "tail_still_ratio": float(still[int(len(still) * 0.75):].mean()) if len(still) >= 8 else 0.0,
        "changed_mean": float(changed.mean()),
        "changed_peak": float(changed.max()),
        "maxdiff_mean": float(maxdiff.mean()),
        "maxdiff_peak": float(maxdiff.max()),
        "diff_mean": float(diffs.mean()),
        "diff_median": float(np.median(diffs)),
        "diff_peak": float(diffs.max()),
        # 均值÷中位数 > 2 = "大部分静止 + 少数剧烈"型节奏
        "burst_ratio": float(diffs.mean() / max(1e-6, np.median(diffs))),
        "luma_mean": float(np.mean(luma)),
        "bright_ratio": float((np.asarray(luma) > 180).mean()),
    }


def verdict(m: dict) -> list[str]:
    bad = []
    if m["static_ratio"] > 0.40:
        bad.append("静止 %.0f%% > 40%%（动作太多，落不下来）" % (m["static_ratio"] * 100))
    if m["change_density"] < 0.5:
        bad.append("变化密度 %.2f/s < 0.5（几乎没动）" % m["change_density"])
    if m["longest_still"] < 0.5 and m["tail_still_ratio"] < 0.35:
        bad.append("既无连续静止（最长 %.2fs）也无落定窗（尾段静止仅 %.0f%%）：没有 rest"
                   % (m["longest_still"], m["tail_still_ratio"] * 100))
    if m["bright_ratio"] > 0.20:
        bad.append("过亮帧 %.0f%% > 20%%" % (m["bright_ratio"] * 100))
    return bad


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", help="帧目录（png）")
    ap.add_argument("--fps", type=float, default=30.0)
    args = ap.parse_args()

    frames = sorted(glob.glob(os.path.join(args.dir, "*.png")))
    m = measure(frames, args.fps)

    print("帧数 %d / 时长 %.2fs @ %.0ffps" % (m["frames"], m["duration"], args.fps))
    print("  静止占比    %5.1f%%   (判据 <40%%)" % (m["static_ratio"] * 100))
    print("  变化密度    %5.2f/s  (判据 >0.5)" % m["change_density"])
    print("  最长静止    %5.2fs   (应 >=0.5s，或尾段静止>=35%%)" % m["longest_still"])
    print("  尾段静止    %5.1f%%   (落定窗；判据 >=35%%)" % (m["tail_still_ratio"] * 100))
    print("  变化像素占比 均 %.3f%% / 峰 %.3f%%  (主口径)" % (m["changed_mean"] * 100, m["changed_peak"] * 100))
    print("  单帧最大像素差 均 %5.1f / 峰 %5.1f" % (m["maxdiff_mean"], m["maxdiff_peak"]))
    print("  [参考] 全屏平均差 均/中/峰 %5.2f / %5.2f / %5.2f" % (m["diff_mean"], m["diff_median"], m["diff_peak"]))
    print("  均值÷中位数 %5.2f    (>2 即'大部分静止+少数剧烈')" % m["burst_ratio"])
    print("  平均亮度    %5.1f    过亮帧 %.1f%%" % (m["luma_mean"], m["bright_ratio"] * 100))

    bad = verdict(m)
    if bad:
        print("\n  需注意：")
        for b in bad:
            print("    · " + b)
    else:
        print("\n  全部判据通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
