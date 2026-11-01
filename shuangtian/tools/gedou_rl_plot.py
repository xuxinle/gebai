#!/usr/bin/env python3
"""把训练报告画成曲线（PNG，无外部依赖：纯 Pillow 手绘）。

## 为什么单独一个脚本

训练脚本的职责是"训练"，出图是"汇报"。混在一起会让训练脚本为了美观
多背一堆绘图代码；而报告 JSON 已经存了全部数据，画图随时可重画。

## 画什么（以及为什么不画什么）

三段并排：**胜率曲线**（策略 vs 两条基线）、**熵**（探索度；塌到 0 说明
提前锁死在单招）、**回报**。胜率曲线必须带基线——只画"训练中的胜率"
在对手随课程变化时完全无法解读（本项目实测过：曲线在 0%↔100% 之间跳，
而真正变化的是对手强度）。
"""

import argparse
import json
import sys
from pathlib import Path

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    raise SystemExit("需要 Pillow：py -3 -m pip install pillow")


W, H = 1160, 420
PAD_L = 70
PAD_R = 30
PAD_T = 46
PAD_B = 54
PANEL_GAP = 34


def font(size):
    for name in ("msyh.ttc", "simhei.ttf", "DejaVuSans.ttf", "arial.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def draw_panel(draw, box, title, series, y_max, y_fmt, colors):
    x0, y0, x1, y1 = box
    draw.rectangle([x0, y0, x1, y1], outline=(210, 214, 220), width=1)
    draw.text((x0 + 8, y0 - 26), title, fill=(40, 44, 52), font=font(15))
    # 网格（4 条水平线）
    for step in range(5):
        y = y1 - (y1 - y0) * step / 4
        draw.line([x0, y, x1, y], fill=(238, 240, 244), width=1)
        draw.text((x0 - 46, y - 8), y_fmt(y_max * step / 4), fill=(120, 126, 136), font=font(11))
    for name, points, color in series:
        if len(points) < 2:
            continue
        coords = []
        for index, value in enumerate(points):
            px = x0 + (x1 - x0) * (index + 1) / max(1, len(points))
            py = y1 - (y1 - y0) * min(1.0, max(0.0, value / y_max if y_max else 0.0))
            coords.append((px, py))
        draw.line(coords, fill=color, width=2)
        for px, py in coords:
            draw.ellipse([px - 2.5, py - 2.5, px + 2.5, py + 2.5], fill=color)
    return colors


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", default="build/e2e-gedou-rl/train-report.json")
    parser.add_argument("--out", default="build/e2e-gedou-rl/train-curve.png")
    args = parser.parse_args()

    report = json.loads(Path(args.report).read_text(encoding="utf-8"))
    history = [h for h in report["history"] if h.get("policy_winrate") is not None]
    if not history:
        raise SystemExit("报告里没有策略胜率数据")

    rounds = [h["round"] for h in history]
    policy = [h["policy_winrate"] for h in history]
    entropy = [h.get("entropy", 0.0) for h in history]
    ret = [h.get("return", 0.0) for h in history]
    baselines = report["baselines"]
    random_base = baselines["random"]["winrate"]
    script_base = baselines["script_ai"]["winrate"]

    image = Image.new("RGB", (W, H), (252, 252, 254))
    draw = ImageDraw.Draw(image)
    draw.text((PAD_L, 14), "格斗机器人 · 强化学习训练曲线", fill=(24, 28, 36), font=font(20))

    panel_w = (W - PAD_L - PAD_R - PANEL_GAP * 2) / 3
    top, bottom = PAD_T + 20, H - PAD_B

    def box(index):
        x0 = PAD_L + index * (panel_w + PANEL_GAP)
        return (int(x0), top, int(x0 + panel_w), bottom)

    y_max = max(0.5, max(policy) * 1.15, script_base * 1.6, random_base * 1.6)
    draw_panel(draw, box(0), "胜率（每轮评测）",
               [("训练策略", policy, (36, 120, 220)),
                ("手写 AI 基线", [script_base] * len(policy), (230, 130, 40)),
                ("随机基线", [random_base] * len(policy), (150, 155, 165))],
               y_max, lambda v: f"{v:.0%}",
               [(36, 120, 220), (230, 130, 40), (150, 155, 165)])
    draw_panel(draw, box(1), "策略熵（探索度）", [("entropy", entropy, (120, 80, 200))],
               max(0.6, max(entropy) * 1.1), lambda v: f"{v:.1f}", [(120, 80, 200)])
    draw_panel(draw, box(2), "平均回报", [("return", ret, (40, 160, 120))],
               max(1.0, max(ret) * 1.15, abs(min(ret)) * 1.15), lambda v: f"{v:.1f}",
               [(40, 160, 120)])

    # 图例（行距固定，不超出画布）
    ly = H - 42
    lx = PAD_L
    for label, color in (("训练策略", (36, 120, 220)), ("手写 AI 基线", (230, 130, 40)),
                         ("随机基线", (150, 155, 165)), ("熵", (120, 80, 200)),
                         ("回报", (40, 160, 120))):
        draw.line([lx, ly + 7, lx + 22, ly + 7], fill=color, width=3)
        draw.text((lx + 28, ly), label, fill=(70, 76, 86), font=font(12))
        lx += 132

    draw.text((PAD_L, H - 20),
              f"共 {len(history)} 轮 · 最佳胜率 {report['best_winrate']:.1%} · "
              f"最终复核 {report['final_eval']['winrate']:.1%} · "
              f"手写 {script_base:.1%} · 随机 {random_base:.1%} · "
              f"{report['seconds']:.0f}s",
              fill=(110, 116, 126), font=font(12))

    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    image.save(args.out)
    print(f"曲线已保存：{args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
