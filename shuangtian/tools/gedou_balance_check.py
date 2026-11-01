#!/usr/bin/env python3
"""格斗平衡体检：**在训练之前**先确认"游戏里有最优解吗"。

## 为什么必须先做这一步

强化学习会把环境里**任何**便宜的策略放大成"机器人很强"。如果游戏本身存在
"只需一直按某一招就必胜"的漏洞，训练出来的机器人只会**更高效地利用漏洞**，
而我们却会把它当成"学会格斗"——这是本项目实际踩到的坑：

- 无条件按蹬腿：100% 胜率、打满 1000 伤害、己方 900 血全程未损
- 手写 AI 自己：只有 ~20% 胜率

两者并存说明问题不在"机器人的水平"，而在**规则与对手**。所以本脚本先跑一遍
固定动作矩阵，把"每个单招的强度"量出来；有压倒性单招时，训练没有意义。

## 判据（写死在这里，避免事后挪动标准）

- 任何一个**固定动作**的胜率 ≥ 90% ⇒ 存在"无脑必胜" ⇒ 平衡不合格，先改规则；
- 手写 AI 对同难度对手的胜率应在 30%~70%（有来有回）；
- 随机策略胜率应 ≤ 15%（否则"什么都不做也有优势"）。

用法：
    py -3 tools/gedou_balance_check.py                    # 默认 100 局/档
    py -3 tools/gedou_balance_check.py --episodes 200 --json out.json
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "dev" / "bin" / ("gedou_rl.exe" if sys.platform == "win32" else "gedou_rl")

ACTIONS = ["idle", "forward", "backward", "guard", "light", "heavy", "kick", "special"]
DIFFICULTIES = ["easy", "normal", "hard"]


def evaluate(extra, episodes, difficulty, seed):
    cmd = [str(EXE), "--eval", "--episodes", str(episodes), "--difficulty", difficulty,
           "--seed", str(seed), "--frame-skip", "1"] + extra
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=1800)
    if proc.returncode != 0:
        raise SystemExit(f"评测失败（{extra}）：{proc.stderr[:400]}")
    for line in proc.stdout.splitlines():
        if line.startswith("RESULT"):
            fields = dict(part.split("=") for part in line.split()[1:])
            return {key: float(value) for key, value in fields.items()}
    raise SystemExit(f"没有 RESULT 行：{proc.stdout[-300:]}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--episodes", type=int, default=100)
    parser.add_argument("--seed", type=int, default=99991)
    parser.add_argument("--json", default="")
    args = parser.parse_args()

    if not EXE.exists():
        raise SystemExit(f"找不到 {EXE}（先 st build gedou_rl）")

    rows = []
    offenders = []

    print(f"每档 {args.episodes} 局 · 决策间隔 1 帧（与手写 AI 同频）\n")
    print(f"{'策略':<12}" + "".join(f"{d:>10}" for d in DIFFICULTIES))
    print("-" * 42)

    for action in ACTIONS:
        values = []
        for difficulty in DIFFICULTIES:
            result = evaluate(["--constant", action], args.episodes, difficulty, args.seed)
            values.append(result["winrate"])
            rows.append({"strategy": f"constant:{action}", "difficulty": difficulty, **result})
        print(f"{action:<12}" + "".join(f"{v:>9.0%} " for v in values))
        if max(values) >= 0.90:
            offenders.append(("constant:" + action, max(values)))

    for label, extra in (("random", ["--random"]), ("script-ai", ["--script-ai"])):
        values = []
        for difficulty in DIFFICULTIES:
            result = evaluate(extra, args.episodes, difficulty, args.seed)
            values.append(result["winrate"])
            rows.append({"strategy": label, "difficulty": difficulty, **result})
        print(f"{label:<12}" + "".join(f"{v:>9.0%} " for v in values))
        if label == "random" and max(values) > 0.15:
            offenders.append(("random", max(values)))

    script_normal = next(r for r in rows
                         if r["strategy"] == "script-ai" and r["difficulty"] == "normal")
    print()
    if offenders:
        print("✗ 平衡不合格——存在“无脑必胜”策略：")
        for name, winrate in offenders:
            print(f"    {name}: 最高胜率 {winrate:.0%}（判据 < 90%）")
        print("  训练之前必须先修规则/对手，否则 RL 只会更高效地利用这个漏洞。")
    else:
        print("✓ 没有压倒性单招（所有固定动作 < 90%）")
    if not (0.30 <= script_normal["winrate"] <= 0.70):
        print(f"△ 手写 AI 对 normal 胜率 {script_normal['winrate']:.0%}，不在有来有回的 30%~70%")
    else:
        print(f"✓ 手写 AI 对 normal 胜率 {script_normal['winrate']:.0%}（有来有回）")

    if args.json:
        Path(args.json).write_text(json.dumps({"rows": rows, "offenders": offenders},
                                              ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"\n报告：{args.json}")

    return 0 if not offenders else 1


if __name__ == "__main__":
    sys.exit(main())
