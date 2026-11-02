"""把游戏骨架渲染成 ControlNet 能读的**姿势线稿**。

## 为什么走 Canny 而不是 OpenPose

ModelScope 上能直接下的 SDXL ControlNet 是 **Canny** 版（OpenPose 版没找到
可用镜像）。Canny 读的是**边缘图**——本质上"给一张线稿，让它照着画"。

这对我们是**正好够用**的：游戏的 2D 骨架本身就是一套清晰的线条
（头/躯干/四肢），画成黑白线稿交给 Canny，身体姿态就被锁死了。

## 为什么这比"用提示词描述动作"强

实测过两次失败：`"throwing a straight punch"` 画出来是**弯腰**，
`"standing upright, one arm extended"` 也只是"大概站着"。
提示词对**精确姿势**的控制力很弱——它只能影响"语义氛围"。

而线稿是**几何约束**：肩在哪里、手伸到哪个坐标，模型必须照着放。
这是"说给模型听"与"画给模型看"的差别。

## 线稿怎么画

用与游戏里**同一份** `Pose`（2D 骨架关节坐标），所以：
- 生成的立绘姿势 = 游戏里的判定姿势（视觉与判定天然对齐）
- 想加新动作，只要在 `POSE_LIBRARY` 里补一组关节坐标

线稿要**白底黑线、粗细一致、不带任何填充**——Canny 对灰度填充会产生
大量伪边缘，导致画面碎掉。
"""
import argparse
import json
import math
import os
import sys


# 每个动作的骨架关节坐标（局部空间：原点在脚下地面，x 向正面，y 向上）。
#
# 数值抄自 `examples/gedou/pose.cpp` 的关键帧（缩放前）。
# 单位是"骨架局部单位"，画线稿时统一乘以 `SCALE` 变成像素。
#
# 关节顺序：hip, chest, head, shoulder_f, elbow_f, hand_f,
#           shoulder_b, elbow_b, hand_b, knee_f, foot_f, knee_b, foot_b
POSE_LIBRARY: dict[str, dict[str, tuple[float, float]]] = {
    "idle": {
        "hip": (0, 62), "chest": (0, 112), "head": (0, 140),
        "shoulder_f": (10, 106), "elbow_f": (16, 82), "hand_f": (18, 60),
        "shoulder_b": (-10, 106), "elbow_b": (-16, 82), "hand_b": (-18, 60),
        "knee_f": (10, 32), "foot_f": (14, 2), "knee_b": (-10, 32), "foot_b": (-14, 2),
    },
    "walk": {
        "hip": (0, 62), "chest": (2, 112), "head": (2, 140),
        "shoulder_f": (12, 106), "elbow_f": (26, 88), "hand_f": (34, 70),
        "shoulder_b": (-8, 106), "elbow_b": (-20, 86), "hand_b": (-26, 68),
        "knee_f": (22, 34), "foot_f": (34, 4), "knee_b": (-16, 32), "foot_b": (-28, 2),
    },
    "guard": {
        "hip": (0, 60), "chest": (-2, 110), "head": (-2, 138),
        "shoulder_f": (10, 104), "elbow_f": (22, 118), "hand_f": (26, 138),
        "shoulder_b": (-10, 104), "elbow_b": (6, 120), "hand_b": (12, 140),
        "knee_f": (12, 30), "foot_f": (16, 2), "knee_b": (-12, 30), "foot_b": (-16, 2),
    },
    # 出拳：手臂必须**完全伸直且明显超出身体轮廓**。
    # 早先 hand_f 只到 x=74（肩在 18），横向仅 56 单位，画出来像"手往旁边指"，
    # ControlNet 也照这个弱信号生成，结果是"手垂在身侧"。
    # 改到 x=105、y 略低于肩，形成一条明确的前伸直线。
    "light": {
        "hip": (0, 62), "chest": (10, 112), "head": (12, 140),
        "shoulder_f": (20, 106), "elbow_f": (62, 104), "hand_f": (105, 102),
        "shoulder_b": (-8, 106), "elbow_b": (-26, 92), "hand_b": (-34, 76),
        "knee_f": (20, 32), "foot_f": (28, 2), "knee_b": (-18, 32), "foot_b": (-26, 2),
    },
    "heavy": {
        "hip": (0, 62), "chest": (-6, 112), "head": (-8, 140),
        "shoulder_f": (-16, 108), "elbow_f": (10, 116), "hand_f": (44, 116),
        "shoulder_b": (14, 106), "elbow_b": (-8, 96), "hand_b": (-26, 88),
        "knee_f": (18, 30), "foot_f": (24, 2), "knee_b": (-16, 30), "foot_b": (-22, 2),
    },
    "kick": {
        "hip": (0, 62), "chest": (-8, 112), "head": (-10, 140),
        "shoulder_f": (14, 108), "elbow_f": (30, 122), "hand_f": (44, 128),
        "shoulder_b": (-14, 106), "elbow_b": (-30, 92), "hand_b": (-38, 76),
        "knee_f": (42, 80), "foot_f": (95, 88), "knee_b": (-8, 32), "foot_b": (-12, 2),
    },
    "special": {
        "hip": (0, 60), "chest": (0, 110), "head": (0, 138),
        "shoulder_f": (16, 108), "elbow_f": (40, 128), "hand_f": (58, 146),
        "shoulder_b": (-16, 108), "elbow_b": (-40, 128), "hand_b": (-58, 146),
        "knee_f": (16, 28), "foot_f": (22, 2), "knee_b": (-16, 28), "foot_b": (-22, 2),
    },
    "donut_toss": {
        "hip": (0, 62), "chest": (6, 112), "head": (6, 140),
        "shoulder_f": (14, 108), "elbow_f": (40, 104), "hand_f": (68, 100),
        "shoulder_b": (-10, 106), "elbow_b": (-16, 84), "hand_b": (-14, 66),
        "knee_f": (14, 32), "foot_f": (20, 2), "knee_b": (-14, 32), "foot_b": (-20, 2),
    },
    # 泡芙（双手前推喷射）。
    #
    # ⚠ 早先两条手臂的**起点都在躯干附近**（shoulder_f=14、shoulder_b=-8，
    # elbow/hand 却在 +42/+70 和 +20/+48），画到骨架图上就是**两条线从胸口交叉穿过**。
    # ControlNet 忠实照做，生成出来是"缩肩抱胸"的姿势——
    # 实测在游戏里看着像**人物变小了一截**（其实是弯腰缩肩）。
    #
    # 修法：让两条手臂都从**同一侧的肩部**出发、平行前伸（像真的双手推出去），
    # 而不是一条从左一条从右斜插到胸前。
    "puff_spray": {
        "hip": (0, 62), "chest": (6, 112), "head": (6, 140),
        "shoulder_f": (16, 106), "elbow_f": (44, 104), "hand_f": (72, 102),
        "shoulder_b": (8, 104), "elbow_b": (36, 116), "hand_b": (64, 122),
        "knee_f": (18, 32), "foot_f": (26, 2), "knee_b": (-16, 32), "foot_b": (-24, 2),
    },
    "waist_grab": {
        "hip": (0, 62), "chest": (10, 112), "head": (10, 140),
        "shoulder_f": (18, 108), "elbow_f": (46, 104), "hand_f": (72, 100),
        "shoulder_b": (-4, 106), "elbow_b": (24, 102), "hand_b": (50, 98),
        "knee_f": (20, 30), "foot_f": (28, 2), "knee_b": (-16, 30), "foot_b": (-24, 2),
    },
    "apple_drop": {
        "hip": (0, 62), "chest": (0, 112), "head": (-2, 140),
        "shoulder_f": (12, 108), "elbow_f": (14, 142), "hand_f": (14, 176),
        "shoulder_b": (-12, 106), "elbow_b": (-18, 82), "hand_b": (-20, 60),
        "knee_f": (10, 32), "foot_f": (14, 2), "knee_b": (-10, 32), "foot_b": (-14, 2),
    },
    "apple_guard": {
        "hip": (0, 60), "chest": (0, 110), "head": (0, 138),
        "shoulder_f": (10, 106), "elbow_f": (16, 118), "hand_f": (-4, 122),
        "shoulder_b": (-10, 106), "elbow_b": (-16, 118), "hand_b": (4, 122),
        "knee_f": (12, 30), "foot_f": (16, 2), "knee_b": (-12, 30), "foot_b": (-16, 2),
    },
    "apple_life": {
        "hip": (0, 60), "chest": (0, 110), "head": (0, 138),
        "shoulder_f": (16, 108), "elbow_f": (42, 122), "hand_f": (62, 136),
        "shoulder_b": (-16, 108), "elbow_b": (-42, 122), "hand_b": (-62, 136),
        "knee_f": (14, 30), "foot_f": (18, 2), "knee_b": (-14, 30), "foot_b": (-18, 2),
    },
    "hurt": {
        "hip": (-4, 62), "chest": (-14, 112), "head": (-22, 138),
        "shoulder_f": (0, 108), "elbow_f": (14, 126), "hand_f": (18, 146),
        "shoulder_b": (-24, 104), "elbow_b": (-38, 88), "hand_b": (-46, 72),
        "knee_f": (8, 32), "foot_f": (12, 2), "knee_b": (-20, 30), "foot_b": (-28, 2),
    },
    "knockdown": {
        "hip": (-30, 24), "chest": (-68, 26), "head": (-100, 28),
        "shoulder_f": (-64, 34), "elbow_f": (-46, 20), "hand_f": (-30, 8),
        "shoulder_b": (-66, 18), "elbow_b": (-48, 6), "hand_b": (-32, 2),
        "knee_f": (-2, 18), "foot_f": (18, 6), "knee_b": (-6, 8), "foot_b": (10, 2),
    },
    "victory": {
        "hip": (0, 62), "chest": (0, 112), "head": (0, 140),
        "shoulder_f": (12, 108), "elbow_f": (16, 142), "hand_f": (18, 178),
        "shoulder_b": (-12, 106), "elbow_b": (-18, 82), "hand_b": (-20, 60),
        "knee_f": (10, 32), "foot_f": (14, 2), "knee_b": (-10, 32), "foot_b": (-14, 2),
    },
    "defeat": {
        "hip": (0, 26), "chest": (-4, 66), "head": (-8, 92),
        "shoulder_f": (8, 62), "elbow_f": (10, 40), "hand_f": (8, 20),
        "shoulder_b": (-14, 60), "elbow_b": (-18, 38), "hand_b": (-20, 18),
        "knee_f": (16, 24), "foot_f": (22, 2), "knee_b": (-10, 24), "foot_b": (-16, 2),
    },
}

# 骨架单位 → 像素
# 骨架单位 → 像素。
#
# ⚠ 这个值决定"人物在画布上占多大"，而 Canny ControlNet **按边缘的实际像素位置**
# 约束构图。实测 SCALE=5.2 时边缘包围盒只有 520×776（画布 832×1216 的 64%），
# 生成出来的人物就**缩在画面中间、很小**。
#
# 目标：让"头顶到脚底"占画布高的 ~88%。骨架从脚(0)到头(155)共 155 单位，
# 88% × 1216 / 155 ≈ 6.9。
SCALE = 6.9
# 画布（与 SDXL 常用尺寸一致）
CANVAS_W, CANVAS_H = 832, 1216
# 地面线（脚下留白，避免脚尖贴边）
GROUND_Y = 1150
CENTER_X = CANVAS_W // 2

BONES = [
    ("hip", "chest"), ("chest", "head"),
    ("chest", "shoulder_f"), ("shoulder_f", "elbow_f"), ("elbow_f", "hand_f"),
    ("chest", "shoulder_b"), ("shoulder_b", "elbow_b"), ("elbow_b", "hand_b"),
    ("hip", "knee_f"), ("knee_f", "foot_f"),
    ("hip", "knee_b"), ("knee_b", "foot_b"),
]


def to_canvas(point: tuple[float, float]) -> tuple[float, float]:
    """局部骨架坐标 → 画布像素（y 向下）。"""
    x, y = point
    return CENTER_X + x * SCALE, GROUND_Y - y * SCALE


def draw_pose_png(pose: dict[str, tuple[float, float]], path: str, line_width: int = 7) -> None:
    """把骨架画成白底黑线的 PNG（Canny 的输入）。

    ⚠ 线宽要**细**（7px 左右），这是踩了几轮才定下来的：
    - 太粗（22px）时，Canny 沿粗线的**两侧**各提一条边缘，ControlNet 读到的是
      "两条平行线"，它理解成"某个粗物体的轮廓"，而不是"一根肢体"，
      姿势约束很弱（实测手臂只是"张开"，做不到精确位置）；
    - 7px 时 Canny 提出的是**单条细边缘**，正好对应 Canny ControlNet
      训练时看到的形态，姿势跟随明显更准。
    - 关节圆点也要小（0.42 倍线宽），否则同样会被当成"大黑块"。

    为什么关节圆点**要小**：第一版用 0.62 倍线宽的圆点，结果整个人变成一个
    由大黑球串起来的形状，模型把球当成"很粗的肢体"画，出来是圆滚滚的。
    0.42 倍刚好能表达"关节"，又不会盖住肢体走向。
    """
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("需要 pillow", file=sys.stderr)
        raise

    image = Image.new("RGB", (CANVAS_W, CANVAS_H), (255, 255, 255))
    draw = ImageDraw.Draw(image)

    # 四肢
    for a, b in BONES:
        if a not in pose or b not in pose:
            continue
        draw.line([to_canvas(pose[a]), to_canvas(pose[b])], fill=(0, 0, 0), width=line_width)

    # 关节圆点（让连接处明确，模型不会画成断开的两截）
    for name in pose:
        cx, cy = to_canvas(pose[name])
        radius = line_width * 0.42
        draw.ellipse([cx - radius, cy - radius, cx + radius, cy + radius], fill=(0, 0, 0))

    # 头：画一个大圆环（表示头部大小与位置）
    if "head" in pose:
        hx, hy = to_canvas(pose["head"])
        head_radius = 15.0 * SCALE * 0.62
        draw.ellipse([hx - head_radius, hy - head_radius, hx + head_radius, hy + head_radius],
                     fill=(0, 0, 0))

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    image.save(path)
    print(f"  {os.path.basename(path)}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--outdir", default="build/char-gen/poses")
    parser.add_argument("--width", type=int, default=22)
    args = parser.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    for name, pose in POSE_LIBRARY.items():
        draw_pose_png(pose, os.path.join(args.outdir, f"pose-{name}.png"), args.width)

    with open(os.path.join(args.outdir, "poses.json"), "w", encoding="utf-8") as handle:
        json.dump(POSE_LIBRARY, handle, ensure_ascii=False, indent=2)
    print(f"{len(POSE_LIBRARY)} 张姿势线稿 -> {args.outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
