"""比较各动作骨架的"纵向跨度"，找出为什么某些动作切换时人物会缩小。

## 机理

`game_view.cpp` 里立绘高度的算法：

```cpp
const float top = pose.head.y - pose.head_radius;
const float height = std::max(40.0f, lowest - top);
```

`lowest` 是**脚**的 y（最低点）。所以：
- **站立动作**（脚在 y=0~4）：跨度 ≈ head.y - 0 ≈ 全部身高
- **躺地/跪地动作**（脚也不在 0 附近，或者整体压低）：跨度小得多

滚动到这类动作时，立绘按较小的 `height` 缩放，人物就"缩小"了。

这是**设计上的取舍**：躺地动作本来就不该占满全高（躺下时人当然比站着矮）。
但如果观众觉得突兀，可以改成"按骨架的标准身高缩放，躺地时允许超出画布"。
"""
import argparse
import json
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--poses", default="build/char-gen/poses/poses.json")
    parser.add_argument("--head-radius", type=float, default=15.0)
    args = parser.parse_args()

    with open(args.poses, encoding="utf-8") as handle:
        library = json.load(handle)

    print(f"{'动作':14s} {'头y':>7s} {'脚y':>7s} {'跨度':>7s} {'相对idle':>9s}  说明")
    baseline = None
    for name, pose in library.items():
        head_y = pose["head"][1]
        feet = [pose[key][1] for key in ("foot_f", "foot_b") if key in pose]
        knees = [pose[key][1] for key in ("knee_f", "knee_b") if key in pose]
        lowest = min(feet + knees) if (feet or knees) else 0.0
        span = head_y - args.head_radius - lowest
        if name == "idle":
            baseline = span
        ratio = span / baseline if baseline else 1.0
        note = ""
        if ratio < 0.7:
            note = "← 明显矮（躺/跪）"
        elif ratio < 0.88:
            note = "← 偏矮"
        print(f"{name:14s} {head_y:7.1f} {lowest:7.1f} {span:7.1f} {ratio:9.3f}  {note}")

    print(f"\n基准（idle）跨度 {baseline:.1f}")
    print("\n提示：跨度小的动作，立绘会按更小的高度缩放 —— 这是"
          "\"躺下的人本来就矮\"的设计取舍，但也可能被看成\"人物缩水\"。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
