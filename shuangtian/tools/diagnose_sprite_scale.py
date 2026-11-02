"""实测各招式渲染时的**立绘缩放高度**，找出哪几个动作会把人物画小。

## 为什么要单独测

`game_view.cpp` 的立绘缩放：

```cpp
const float lowest = std::max(pose.foot_front.y, pose.foot_back.y);  // 世界坐标最低点
const float top    = pose.head.y - pose.head_radius;                 // 世界坐标头顶
const float height = std::max(40.0f, lowest - top);                  // 贴图高度（世界像素）
```

`height` 由**姿态**决定，不是常数。所以：
- 前倾动作（`puff_spray` 双臂前推时躯干压低）⇒ `head.y` 变大 ⇒ `top` 变大 ⇒ `height` 变小 ⇒ **人物画小**
- 而贴图本身是同一张全身立绘 ⇒ 被压进更小的高度 ⇒ 视觉上缩水

这解释了"泡芙时人物变小"：**不是素材问题，是姿态让缩放变了**。

## 修法

立绘的缩放应该基于**角色的标准身高**（`idle` 的跨度），而不是每帧的
`lowest - top`。这样：
- 站立/出招时高度恒定（人物大小稳定）
- 蹲/躺/跪时仍然按标准身高缩放（人不会因为蹲下就变小，只是重心低了）

代价：躺地动作会超出画布下沿。但躺地时 `lowest` 本来就靠下，
超出部分正好被地面线挡住——视觉上合理。
"""
import argparse
import json
import sys

# 与 pose.cpp 保持一致
K_BODY_SCALE = 2.2      # kBodyScale（若不同请改这里）
HEAD_RADIUS = 15.0      # body::kHeadRadius


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--poses", default="build/char-gen/poses/poses.json")
    parser.add_argument("--scale", type=float, default=K_BODY_SCALE)
    parser.add_argument("--head-radius", type=float, default=HEAD_RADIUS)
    args = parser.parse_args()

    with open(args.poses, encoding="utf-8") as handle:
        library = json.load(handle)

    print(f"{'动作':14s} {'头顶(局部)':>10s} {'最低脚':>8s} {'跨度(局部)':>11s} {'相对idle':>9s}")
    baseline = None
    for name, pose in library.items():
        head_y = pose["head"][1]
        feet = [pose[key][1] for key in ("foot_f", "foot_b") if key in pose]
        knees = [pose[key][1] for key in ("knee_f", "knee_b") if key in pose]
        lowest_local = min(feet + knees) if (feet or knees) else 0.0
        top = head_y - args.head_radius
        span = lowest_local - top
        if name == "idle":
            baseline = span
        ratio = span / baseline if baseline else 1.0
        mark = "  ← 会明显变小" if ratio < 0.85 else ("  ← 偏小" if ratio < 0.95 else "")
        print(f"{name:14s} {top:10.1f} {lowest_local:8.1f} {span:11.1f} {ratio:9.3f}{mark}")

    print(f"\n基准（idle）跨度 {baseline:.1f} 局部单位")
    print("结论：跨度小于 idle 的动作，立绘会被压进更小的高度，看起来像"
          "\"人物缩水\"。修法是把缩放基准固定成 idle 的跨度。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
