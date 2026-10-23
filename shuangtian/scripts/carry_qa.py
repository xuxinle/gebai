#!/usr/bin/env python3
"""carry_qa.py — 连贯度验收：跨镜头边界有没有东西"活下来并变成下一个"。

## 这份工具解决什么

成熟动效片判定"像 PPT"的准确形态是：**后一个画面把前一个替掉了**，
两者之间没有任何东西连续。这件事在像素上表现为"画面变了"，
在**元素身份**上表现为"旧元素消失、新元素出现"。

裁云的架构是"每幕独立渲染 + 交叉淡入"，从结构上就属于后者。
本工具用裁云特有的**元素轨迹真值**（`--track-elements` 导出的
`trajectory.tsv`）判定：每个边界前后，有没有**同一个名字**的元素还在，
并且它在这段里移动 ≥ 对角线的 2% 或缩放 ≥ 10%。

## 与 onetake 的口径对应（不照抄，但可比）

| 概念 | onetake `probe.py` | 这里 |
|---|---|---|
| 边界 | 0.4 s 窗口内画面身份大面积改变 | 相邻两幕的**场景边界**（时间轴上已知） |
| carried | 有身份幸存并移动 ≥2% 对角线 或缩放 ≥10% | 同（用轨迹矩形的精确值，不是从 DOM 猜） |
| anchored | 唯一幸存者是大面积且没动 | 同 |
| bare | 什么都没有幸存 | 同 |
| score | (carried + anchored/2) / 边界数 | 同 |

**为什么我们的口径可以更准**：onetake 要从 DOM/canvas 调用里**推断**元素身份
（"image source + draw order"），而我们是渲染器自己记的**图层名字与矩形**。
所以同一套判据在我们这里的噪声更小——这也是我们不必照搬它的原因。

## 用法

    # 先出片并导出轨迹
    caiyun --headless --script film.json --render out/film.mp4 --track-elements
    # 再验收
    python scripts/carry_qa.py out/trajectory.tsv --duration 15

    # 与参考阈值对照（onetake 的红线：<0.5 FAIL，<0.7 WARN）
    python scripts/carry_qa.py out/trajectory.tsv --duration 15 --strict

退出码：0 = 通过（≥0.5），1 = 不合格，2 = 输入有问题。
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path

# 判据常量（与 onetake 的 probe.py 同口径，便于横向比较）
MOVE_RATIO = 0.02      # 移动 ≥ 画面对角线的 2% → 视作"动了"
SCALE_RATIO = 0.10     # 或缩放 ≥ 10%
ANCHOR_AREA = 0.03     # 未动但面积 ≥ 画面 3% → anchored
BOUNDARY_WINDOW = 0.25  # 边界前后各取 0.25 s 的样本（比 0.4 s 窗口更严格：
                        # 我们已知边界的确切时刻，不需要靠画面变化去猜）
FAIL_BELOW = 0.5
WARN_BELOW = 0.7


def load_tracks(path: Path) -> dict[int, dict[str, dict[str, float]]]:
    """读 trajectory.tsv → {frame: {name: {x,y,w,h,alpha}}}。"""
    frames: dict[int, dict[str, dict[str, float]]] = {}
    with path.open(encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle, delimiter="\t")
        required = {"frame", "name", "x", "y", "w", "h"}
        if not required.issubset(reader.fieldnames or []):
            raise ValueError(
                f"trajectory.tsv 缺少列：{sorted(required - set(reader.fieldnames or []))}"
                f"（实际列：{reader.fieldnames}）"
            )
        for row in reader:
            try:
                frame = int(row["frame"])
                item = {
                    "x": float(row["x"]),
                    "y": float(row["y"]),
                    "w": float(row["w"]),
                    "h": float(row["h"]),
                    "alpha": float(row.get("alpha") or 1.0),
                }
            except (TypeError, ValueError):
                continue
            frames.setdefault(frame, {})[row["name"]] = item
    return frames


def classify(before: dict[str, dict[str, float]], after: dict[str, dict[str, float]],
             width: float, height: float) -> tuple[str, list[str], list[str], float]:
    """判定一个边界：返回 (kind, carriers, anchors, min_jaccard)。"""
    diag = math.hypot(width, height)
    area = width * height
    shared = set(before) & set(after)
    strong: list[tuple[float, str]] = []
    weak: list[tuple[float, str]] = []
    for name in shared:
        a, b = before[name], after[name]
        if min(a["alpha"], b["alpha"]) < 0.15:
            continue
        move = math.hypot(b["x"] - a["x"], b["y"] - a["y"]) / diag
        area_a = max(a["w"] * a["h"], 1.0)
        area_b = max(b["w"] * b["h"], 1.0)
        scale = abs(math.log(math.sqrt(area_b) / math.sqrt(area_a)))
        big = min(area_a, area_b) / area
        if move >= MOVE_RATIO or scale >= SCALE_RATIO:
            strong.append((max(move, scale), name))
        elif big >= ANCHOR_AREA:
            weak.append((big, name))
    strong.sort(reverse=True)
    weak.sort(reverse=True)
    if strong:
        return "carried", [n for _, n in strong[:3]], [], 0.0
    if weak:
        return "anchored", [], [n for _, n in weak[:2]], 0.0
    return "bare", [], [], 0.0


def classify_window(before_frames: list[dict[str, dict[str, float]]],
                    after_frames: list[dict[str, dict[str, float]]],
                    width: float, height: float) -> tuple[str, list[str], list[str]]:
    """边界前后各一组样本 → (kind, carriers, anchors)。

    ## 为何要在窗口内取极值（而不是比较两个单帧）

    实测踩过：承接图元在边界处已经长得接近满屏，于是“边界前最近一帧 vs 边界后最近一帧”
    算出的缩放变化不足 10%，被误判为 anchored（未动）。而它在整个窗口里从 578px
    长到了 1919px。承接的本质是“**跨过**这段窗口的变化”，所以：

    · 对每个两侧都出现的名字，拼出它的完整状态序列（前后两组按时间拼接）；
    · 取该序列里**最早的**状态与**最晚的**状态做比较（而不是两侧各自的端点）。

    这样“先缩后长”这类两阶段运动也能被抓住（两端差异大），符合观感。
    """
    diag = math.hypot(width, height)
    area = width * height
    names = set()
    for group in before_frames:
        names |= set(group)
    names &= set().union(*after_frames) if after_frames else set()

    strong: list[tuple[float, str]] = []
    weak: list[tuple[float, str]] = []
    for name in names:
        series: list[dict[str, float]] = []
        for group in before_frames + after_frames:
            item = group.get(name)
            if item is not None and item["alpha"] >= 0.15:
                series.append(item)
        if len(series) < 2:
            continue
        first, last = series[0], series[-1]
        move = math.hypot(last["x"] - first["x"], last["y"] - first["y"]) / diag
        area_first = max(first["w"] * first["h"], 1.0)
        area_last = max(last["w"] * last["h"], 1.0)
        scale = abs(math.log(math.sqrt(area_last) / math.sqrt(area_first)))
        big = min(area_first, area_last) / area
        if move >= MOVE_RATIO or scale >= SCALE_RATIO:
            strong.append((max(move, scale), name))
        elif big >= ANCHOR_AREA:
            weak.append((big, name))
    strong.sort(reverse=True)
    weak.sort(reverse=True)
    if strong:
        return "carried", [n for _, n in strong[:3]], []
    if weak:
        return "anchored", [], [n for _, n in weak[:2]]
    return "bare", [], []


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("trajectory", help="trajectory.tsv（caiyun --track-elements 产出）")
    parser.add_argument("--bounds", default=None,
                        help="场景边界时刻，逗号分隔（秒）；省略则从 --duration 与 --scenes 推")
    parser.add_argument("--scenes", default=None,
                        help="各幕时长，逗号分隔（秒）；工具据此算边界")
    parser.add_argument("--duration", type=float, default=None, help="全片时长（秒）")
    parser.add_argument("--fps", type=float, default=30.0)
    parser.add_argument("--width", type=float, default=1920.0)
    parser.add_argument("--height", type=float, default=1080.0)
    parser.add_argument("--strict", action="store_true", help="用 onetake 的红线判（<0.5 FAIL）")
    parser.add_argument("--quiet", action="store_true", help="只打印结论")
    args = parser.parse_args()

    path = Path(args.trajectory)
    if not path.exists():
        print(f"找不到轨迹文件：{path}", file=sys.stderr)
        print("先出片时加 --track-elements（它会写出 <输出目录>/trajectory.tsv）", file=sys.stderr)
        return 2

    try:
        frames = load_tracks(path)
    except ValueError as problem:
        print(f"轨迹文件格式不对：{problem}", file=sys.stderr)
        return 2
    if not frames:
        print("轨迹是空的——没有任何元素被记录（检查图层是否有 name）", file=sys.stderr)
        return 2

    order = sorted(frames)
    fps = args.fps

    # —— 求边界时刻 ——
    if args.bounds:
        bounds = [float(x) for x in args.bounds.split(",") if x.strip()]
    elif args.scenes:
        lengths = [float(x) for x in args.scenes.split(",") if x.strip()]
        total = sum(lengths)
        if args.duration and abs(total - args.duration) > 0.05:
            print(f"提示：按 --scenes 求和 {total:.2f}s，与 --duration {args.duration:.2f}s 不一致"
                  f"（用 scenes 的）", file=sys.stderr)
        bounds, acc = [], 0.0
        for length in lengths[:-1]:
            acc += length
            bounds.append(round(acc, 3))
    else:
        print("需要 --bounds 或 --scenes 才能知道边界在哪（这是本工具唯一的外部知识）",
              file=sys.stderr)
        return 2

    window = max(1, int(round(BOUNDARY_WINDOW * fps)))
    events = []
    for at in bounds:
        centre = int(round(at * fps))
        # 边界前/后的样本集（各 window 帧）
        before_frames = [f for f in order if centre - window <= f < centre]
        after_frames = [f for f in order if centre <= f <= centre + window]
        if not before_frames or not after_frames:
            continue

        # ## 为何不能只取“离边界最近的一帧”
        #
        # 实测踩过：承接图元在边界处已经长得接近满屏，于是“最近一帧 vs 最近一帧”
        # 算出的 scale 变化不足 10%，被判成 anchored（没动）——
        # 而它确实从 578px 长到了 1919px（看轨迹一目了然）。
        #
        # 承接的本质是“**跨过**这段窗口的变化”，所以要在窗口内取**极值**：
        # 对每个在两侧都出现的名字，取它在前窗口最“小”与后窗口最“大”的状态
        # 分别算位移与缩放。这也能抓住“先缩后长”这类两阶段运动。
        kind, carriers, anchors = classify_window(
            [frames[f] for f in before_frames],
            [frames[f] for f in after_frames],
            args.width, args.height)
        events.append({"t": at, "kind": kind, "carriers": carriers, "anchors": anchors,
                       "before": before_frames[-1], "after": after_frames[0]})

    if not events:
        print("没有可判定的边界（轨迹覆盖不到边界附近）", file=sys.stderr)
        return 2

    carried = sum(1 for e in events if e["kind"] == "carried")
    anchored = sum(1 for e in events if e["kind"] == "anchored")
    bare = [e for e in events if e["kind"] == "bare"]
    score = (carried + anchored * 0.5) / len(events)

    print(f"carry score {score:.2f} — {len(events)} 个边界："
          f"{carried} carried · {anchored} anchored · {len(bare)} bare")

    if not args.quiet:
        print()
        for event in events:
            via = ", ".join(event["carriers"] or event["anchors"]) or "—"
            print(f"  t={event['t']:6.2f}  {event['kind']:9} via {via}")

    # —— 判据 ——
    if score < FAIL_BELOW:
        verdict = "FAIL"
        hint = ("每个边界都要有东西活下来：给场景加 carry_out（见 docs/script_design.md），"
                "或者在边界处放同名图层")
    elif score < WARN_BELOW:
        verdict = "WARN"
        hint = "已过红线，但仍有 bare 边界会读作换页；把 bare 那几处改成承接"
    else:
        verdict = "PASS"
        hint = ""

    print(f"\nVERDICT: {verdict}")
    if hint:
        print(f"  {hint}")
    if bare and not args.quiet:
        spots = ", ".join(f"{e['t']:.2f}s" for e in bare)
        print(f"  bare 边界：{spots}")

    if args.strict and verdict == "FAIL":
        return 1
    # 默认也按 onetake 红线返回（便于 CI 直接用退出码）
    return 0 if score >= FAIL_BELOW else 1


if __name__ == "__main__":
    sys.exit(main())
