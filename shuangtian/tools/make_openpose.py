"""把游戏骨架画成 **OpenPose 骨架图**（ControlNet 的输入格式）。

## 和 Canny 线稿的区别（这是能不能用起来的关键）

Canny 读的是"边缘"，所以给它黑线就行；**OpenPose 读的是"骨架语义"**：
它按 **COCO 18 关键点**的固定顺序与固定颜色来识别人体。

具体必须对上的三件事：

1. **18 个点的顺序不能错**。ControlNet 是靠像素颜色反推"这是左肩还是右肘"的。
2. **每个关节颜色固定**（见 `COCO_COLORS`）。比如左肩必须是 (255,0,0)、
   右肩 (255,85,0)…… 颜色错了，模型会把左手当右手。
3. **四肢要画成有宽度的色带，不是细线**。官方实现里相邻关键点之间用
   该点的颜色画一条粗线（`limbSeq`），关节再叠一个实心圆。

## 为什么这套比 Canny 强

Canny 是"画给你看轮廓"，模型还得自己猜"这条线是胳膊还是背景"；
OpenPose 是"直接告出 18 个关节的坐标和左右归属"，几乎没有歧义。
这是姿势控制能做到"精确"的根本原因。

## COCO 18 关键点顺序

```
0 鼻 1 颈 2 右肩 3 右肘 4 右腕 5 左肩 6 左肘 7 左腕
8 右髋 9 右膝 10 右踝 11 左髋 12 左膝 13 左踝
14 右眼 15 左眼 16 右耳 17 左耳
```
"""
import argparse
import json
import math
import os
import sys

# COCO-18 的标准颜色（OpenPose 官方配色，逐点固定）。
COCO_COLORS = [
    (255, 0, 0), (255, 85, 0), (255, 170, 0), (255, 255, 0), (170, 255, 0),
    (85, 255, 0), (0, 255, 0), (0, 255, 85), (0, 255, 170), (0, 255, 255),
    (0, 170, 255), (0, 85, 255), (0, 0, 255), (85, 0, 255), (170, 0, 255),
    (255, 0, 255), (255, 0, 170), (255, 0, 85),
]

# 官方 limbSeq：相邻关键点对（用它决定"画哪条肢体、用什么颜色"）。
LIMB_SEQ = [
    (1, 2), (1, 5), (2, 3), (3, 4), (5, 6), (6, 7),
    (1, 8), (8, 9), (9, 10), (1, 11), (11, 12), (12, 13),
    (1, 0), (0, 14), (14, 16), (0, 15), (15, 17),
]

# 与游戏 `Pose` 的关节 → COCO-18 索引。
#
# 游戏的骨架没有"眼/耳/鼻"的区分（只有一个 head 点），所以：
# - 颈 = chest
# - 鼻 = head（往朝向那一侧偏一点，让 OpenPose 能判断朝向）
# - 眼/耳 用 head 的左右偏置推出来（OpenPose 允许这些点缺失，不影响主骨架）
JOINT_MAP = {
    # ⚠ 游戏骨架里**没有 "neck" 这个关节**，颈的位置就是 `chest`
    #（`chest` 是肩线/胸，OpenPose 的颈部点取它最合适）。
    # 早先写成 `"neck": 1` 而数据里没有这个键，于是 1 号点（颈）**从来没被设置**，
    # 导致躯干和手臂全部落空——渲染出来只有两条腿加一个头。
    "chest": 1,
    "head": 0,
    "shoulder_f": 5,   # 前方臂 = 角色左侧（面朝 +x 时）
    "elbow_f": 6,
    "hand_f": 7,
    "shoulder_b": 2,   # 后方臂 = 角色右侧
    "elbow_b": 3,
    "hand_b": 4,
    "knee_f": 12,
    "foot_f": 13,
    "knee_b": 9,
    "foot_b": 10,
}

# 画布
CANVAS_W, CANVAS_H = 768, 1024
GROUND_Y = 960
CENTER_X = CANVAS_W // 2
SCALE = 6.0
LIMB_WIDTH = 8
JOINT_RADIUS = 5


def build_keypoints(pose: dict[str, tuple[float, float]]) -> dict[int, tuple[float, float]]:
    """把游戏骨架关节翻译成 COCO-18 的像素坐标。"""
    points: dict[int, tuple[float, float]] = {}

    def put(index: int, point: tuple[float, float]) -> None:
        x, y = point
        points[index] = (CENTER_X + x * SCALE, GROUND_Y - y * SCALE)

    for name, index in JOINT_MAP.items():
        if name in pose:
            put(index, pose[name])

    # 左右髋：游戏只有一个 hip 中点，按膝盖方向推出左右
    hip = pose.get("hip")
    if hip is not None:
        knee_f = pose.get("knee_f", hip)
        knee_b = pose.get("knee_b", hip)
        # 左髋偏向 knee_f 那一侧，右髋偏向 knee_b
        left_hip = (hip[0] + (knee_f[0] - hip[0]) * 0.55 + 4.0, hip[1] - 4.0)
        right_hip = (hip[0] + (knee_b[0] - hip[0]) * 0.55 - 4.0, hip[1] - 4.0)
        put(11, left_hip)
        put(8, right_hip)

    # 眼与耳：由 head 推（OpenPose 缺这些点也能用，但补上朝向更明确）
    head = pose.get("head")
    # ── 头部关键点：**脸的朝向全靠它们** ──
    #
    # OpenPose 判断"朝前还是朝后"靠眼睛/耳朵的**左右相对位置**：
    # 正脸时两眼分得开、耳朵更靠外；侧脸时一侧的眼耳会挤压在一起。
    #
    # ⚠ 踩过的坑：眼耳只写到 ±6 / ±10（骨架单位），映射到 768 宽的画布上
    # 就是 4 个几乎重合的点。模型看不出朝向，**随机猜成了背面**——
    # 实测连续两轮生成的甜甜圈全是后背朝镜头，国旗帽和脸的细节完全看不见。
    #
    # 修法：按真实头部比例拉开。头半径约 15 单位（`POSE_LIBRARY` 里 head 到
    # chest 的间距），所以眼间距取 ±5.5、耳取 ±13。
    if head is not None:
        put(15, (head[0] + 5.5, head[1] - 2.0))    # 左眼
        put(14, (head[0] - 5.5, head[1] - 2.0))    # 右眼
        put(17, (head[0] + 13.0, head[1] + 0.5))   # 左耳
        put(16, (head[0] - 13.0, head[1] + 0.5))   # 右耳

    return points


def draw_openpose(points: dict[int, tuple[float, float]], path: str) -> None:
    """按 OpenPose 规范画：黑底 + 彩色肢体 + 彩色关节圆。"""
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("需要 pillow", file=sys.stderr)
        raise

    image = Image.new("RGB", (CANVAS_W, CANVAS_H), (0, 0, 0))
    draw = ImageDraw.Draw(image)

    # 肢体：用**靠近颈部的那个点**的颜色（官方实现就是取 limbSeq 第一个点的色）
    for a, b in LIMB_SEQ:
        if a not in points or b not in points:
            continue
        draw.line([points[a], points[b]], fill=COCO_COLORS[a], width=LIMB_WIDTH)

    # ⚠ 补一条**躯干**：COCO-18 的 limbSeq 里**没有"颈→髋"这一条**
    #（它靠"颈→两髋"两条斜线隐式表达躯干）。只画标准 limbSeq 时，
    # 骨架图上下半身是断开的——实测渲染出来像"两根腿飘在空中"。
    # 补上这两条斜线（颈→左髋、颈→右髋）后，OpenPose 才能正确理解身体朝向。
    if 1 in points:
        for hip_index in (8, 11):
            if hip_index in points:
                draw.line([points[1], points[hip_index]],
                          fill=COCO_COLORS[1], width=LIMB_WIDTH)

    # 关节圆点
    #
    # ⚠ 鼻子（0 号）画**大一圈**：它是 OpenPose 里唯一表示"脸朝哪边"的
    # 定位点，太小的话模型读不出头部朝向，会随机画成背面或侧脸。
    # 其余关节点保持小（大了会被理解成"很粗的肢体"）。
    for index, position in points.items():
        radius = JOINT_RADIUS * 1.9 if index == 0 else JOINT_RADIUS
        draw.ellipse([position[0] - radius, position[1] - radius,
                      position[0] + radius, position[1] + radius],
                     fill=COCO_COLORS[index])

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    image.save(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--poses", default="build/char-gen/poses.json",
                        help="骨架坐标（由 make_poses.py 输出）")
    parser.add_argument("--outdir", default="build/char-gen/openpose")
    args = parser.parse_args()

    if not os.path.exists(args.poses):
        print(f"找不到骨架坐标：{args.poses}", file=sys.stderr)
        return 1

    with open(args.poses, encoding="utf-8") as handle:
        library = json.load(handle)

    os.makedirs(args.outdir, exist_ok=True)
    for name, pose in library.items():
        points = build_keypoints(pose)
        draw_openpose(points, os.path.join(args.outdir, f"openpose-{name}.png"))
        print(f"  openpose-{name}.png  ({len(points)} 个关键点)", flush=True)

    print(f"{len(library)} 张 OpenPose 骨架图 -> {args.outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
