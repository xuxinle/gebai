#!/usr/bin/env python3
"""一页一行版：**整幅总墨量**比（不依赖任何裁剪/对齐）。

用法：python tools/size_ink_oneshot.py <meta.json> <浏览器目录> <霜天目录前缀> <gamma...>
      其中霜天侧文件名为 `<前缀><gamma>.png`。
"""

import json
import sys

import numpy as np
from PIL import Image

FG = np.array([0x0F, 0x17, 0x2A], dtype=float)
BG = np.array([255.0, 255.0, 255.0])


def ink(path):
    a = np.asarray(Image.open(path).convert("RGB")).astype(float)
    d = BG - FG
    return float(np.clip(((BG - a) @ d) / float(d @ d), 0.0, 1.0).sum())


def main() -> int:
    meta = json.loads(open(sys.argv[1]).read())
    br_dir = sys.argv[2]
    st_prefix = sys.argv[3]
    gammas = sys.argv[4:]
    print(f"{'逻辑px':>7}{'物理px':>8}  " + "  ".join(f"γ{g:>4}" for g in gammas) + "   理想γ")
    print("-" * 80)
    for row in meta:
        size = row["size"]
        br = ink(f"{br_dir}/{size}.png")
        rs = []
        for g in gammas:
            rs.append(ink(f"{st_prefix}{g}.{size}.png") / br if br > 0 else float("nan"))
        gv = [float(g) for g in gammas]
        ideal = None
        for k in range(len(rs) - 1):
            if (rs[k] - 1.0) * (rs[k + 1] - 1.0) <= 0.0 and rs[k] != rs[k + 1]:
                t = (1.0 - rs[k]) / (rs[k + 1] - rs[k])
                ideal = gv[k] + t * (gv[k + 1] - gv[k])
                break
        print(f"{size:>7}{size * 1.5:>8.1f}  " + "  ".join(f"{r:>6.3f}" for r in rs)
              + f"   {('%.3f' % ideal) if ideal is not None else '  —  '}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
