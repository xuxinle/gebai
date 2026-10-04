"""真窗口浏览器基准的「字号 → 理想 γ」汇总（并给出可分档的平滑映射）。

输入是两批真窗口测量（小字号页 10/11/12/14/16，大字号页 20/24/32）。本脚本把
"逐字号的理想 γ"与"**分档映射**"分开：

  · 逐字号理想 γ 直接读实测（**不平滑**——实测值本身就带噪声，平滑会掩盖真跳变）
  · 分档映射只分**两档**：一个阈值，两侧各一个 γ。理由是实测显示理想 γ 的走向
    只能支撑"小字号要更亮的 γ"这一条结论，更细的档位没有数据支撑（见 DESIGN §4.3.7.16）

用法：python tools/size_map.py <理想γ表.tsv>
      TSV 两列：逻辑px<TAB>理想γ
"""

import sys

import numpy as np


def main() -> int:
    rows = []
    for line in open(sys.argv[1], encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        a, b = line.split()
        rows.append((float(a), float(b)))
    rows.sort()
    print(f"{'逻辑px':>7}{'物理px':>8}{'理想γ':>9}")
    print("-" * 26)
    for size, g in rows:
        print(f"{size:>7}{size * 1.5:>8.1f}{g:>9.3f}")

    xs = np.array([r[0] * 1.5 for r in rows])
    ys = np.array([r[1] for r in rows])
    # 只报两点趋势：小字号端与正文端（不硬拟合曲线）
    small = ys[xs <= 24]
    body = ys[xs >= 30]
    print()
    print(f"小字号端(≤24 物理px) 理想 γ：中位 {np.median(small):.3f}  范围 "
          f"{small.min():.3f}~{small.max():.3f}")
    print(f"正文端(≥30 物理px)   理想 γ：中位 {np.median(body):.3f}  范围 "
          f"{body.min():.3f}~{body.max():.3f}")
    print(f"两端差：{np.median(small) - np.median(body):+.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
