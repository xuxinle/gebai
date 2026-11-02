"""检查全套立绘的**人物尺寸是否一致**。

## 为什么需要

立绘是按 `MoveId` 逐张切换的。如果某张图里人物比别的小，切换动作时
人物会**突然缩小**——不是"动画效果"，是素材问题。

游戏里立绘高度按骨架跨度缩放：
```
height = lowest - (pose.head.y - pose.head_radius)
```
这意味着**缩放只看骨架，不看图里人物实际占多少**。所以如果生成时
模型把人物画小了（比如双臂张开的动作容易画小，为了塞进画布），
切到那张就会缩水。

## 判据

量每张图 alpha 通道的**内容高度**（不透明像素的 y 跨度），
与全套中位数比较。偏差超过 8% 就该重画。
"""
import glob
import os
import sys

import numpy as np
from PIL import Image


def measure(path: str) -> dict:
    image = Image.open(path).convert("RGBA")
    array = np.array(image)
    mask = array[:, :, 3] > 40
    ys, xs = np.where(mask)
    if len(ys) == 0:
        return {"path": path, "height": 0, "width": 0, "top": 0, "bottom": 0,
                "canvas": array.shape[0]}
    return {
        "path": path,
        "height": int(ys.max() - ys.min()),
        "width": int(xs.max() - xs.min()),
        "top": int(ys.min()),
        "bottom": int(ys.max()),
        "canvas": int(array.shape[0]),
    }


def main() -> int:
    parser = __import__("argparse").ArgumentParser()
    parser.add_argument("--dir", default="build/char-gen/final2-cut")
    parser.add_argument("--pattern", default="donut-*.png")
    parser.add_argument("--tolerance", type=float, default=0.08)
    args = parser.parse_args()

    paths = sorted(glob.glob(os.path.join(args.dir, args.pattern)))
    if not paths:
        print(f"{args.dir}/{args.pattern} 没有文件", file=sys.stderr)
        return 1

    rows = [measure(path) for path in paths]
    rows = [r for r in rows if r["height"] > 0]
    heights = np.array([r["height"] for r in rows])
    median = float(np.median(heights))

    print(f"{'动作':16s} {'内容高':>7s} {'vs中位':>8s} {'内容宽':>7s} {'顶':>5s} {'底':>5s}")
    problems = []
    for row in rows:
        name = os.path.basename(row["path"]).split("-", 1)[1][:-4]
        ratio = row["height"] / median
        flag = ""
        if abs(ratio - 1.0) > args.tolerance:
            flag = "  ← 偏" + ("小" if ratio < 1 else "大")
            problems.append((name, ratio))
        print(f"{name:16s} {row['height']:7d} {ratio:8.3f} {row['width']:7d} "
              f"{row['top']:5d} {row['bottom']:5d}{flag}")

    print(f"\n中位高度 {median:.0f}px，范围 {heights.min()}~{heights.max()}"
          f"（比值 {heights.max() / heights.min():.2f}）")
    if problems:
        print(f"\n{len(problems)} 张超出 ±{args.tolerance:.0%} 容差：")
        for name, ratio in problems:
            print(f"  {name}: {ratio:.3f}")
    else:
        print(f"\n全部在 ±{args.tolerance:.0%} 容差内")
    return 0


if __name__ == "__main__":
    sys.exit(main())
