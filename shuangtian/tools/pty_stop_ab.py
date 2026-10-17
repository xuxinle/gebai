#!/usr/bin/env python3
"""量 `terminal_pty_stop_interrupts_without_killing_the_shell` 的失败形态。

判定要回答的问题只有两个：
  ① 失败时是「等得不够久」还是「Ctrl+C 被整条吞掉（再发一次立刻恢复）」？
  ② 红率是否与机器负载相关？

因此每轮采集三件事：
  · 首提示符 → 标记出现 → 发 Ctrl+C → 第二个提示符，各自的墙钟耗时；
  · 失败轮里**再发一次** Ctrl+C 是否恢复、用时多少（恢复 = 被吞，不是僵住）；
  · 起止 load average（结论必须带负载条件）。

用法：python3 tools/pty_stop_ab.py <轮数> [--label 名字]
"""

from __future__ import annotations

import argparse
import os
import re
import statistics
import subprocess
import sys

PROBE = os.environ.get("PTY_PROBE", "build/dev/bin/pty_stop_probe")

ROUND_RE = re.compile(r"════ 轮次 (\d+) ════")
BACK_RE = re.compile(r"back_to_prompt=(是|\*\*否\*\*)（(\d+) ms）")
AFTER_RE = re.compile(r"after-stop 可见=(是|\*\*否\*\*)")
RECOVER_RE = re.compile(r"再发一次 Ctrl\+C → 恢复=(是[^·]*)（(\d+) ms）")
MARKER_RE = re.compile(r"已开始执行=(是|\*\*否\*\*)")


def load() -> str:
    with open("/proc/loadavg", encoding="utf-8") as handle:
        return handle.read().split()[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("rounds", type=int)
    parser.add_argument("--label", default="")
    parser.add_argument("--settle", type=int, default=0)
    args = parser.parse_args()

    before = load()
    proc = subprocess.run(
        [PROBE, str(args.rounds), str(args.settle)],
        capture_output=True,
        text=True,
        timeout=60 * 30,
    )
    after = load()
    out = proc.stdout

    rounds: list[dict] = []
    current: dict | None = None
    for line in out.splitlines():
        m = ROUND_RE.search(line)
        if m:
            if current is not None:
                rounds.append(current)
            current = {"n": int(m.group(1)), "back": None, "ms": None, "after": None,
                       "marker": None, "recover_ms": None, "recover": None}
            continue
        if current is None:
            continue
        if (m := MARKER_RE.search(line)) is not None:
            current["marker"] = m.group(1) == "是"
        if (m := BACK_RE.search(line)) is not None:
            current["back"] = m.group(1) == "是"
            current["ms"] = int(m.group(2))
        if (m := AFTER_RE.search(line)) is not None:
            current["after"] = m.group(1) == "是"
        if (m := RECOVER_RE.search(line)) is not None:
            current["recover"] = True
            current["recover_ms"] = int(m.group(2))
    if current is not None:
        rounds.append(current)

    red = [r for r in rounds if r["back"] is False]
    green = [r for r in rounds if r["back"] is True]
    green_ms = [r["ms"] for r in green if r["ms"] is not None]

    label = args.label or "run"
    print(f"\n===== {label} =====")
    print(f"负载：起 {before} → 止 {after} · {len(rounds)} 轮")
    print(f"红 {len(red)} / {len(rounds)}（{100.0 * len(red) / max(1, len(rounds)):.0f}%）")
    if green_ms:
        green_ms_sorted = sorted(green_ms)
        print(f"绿轮恢复耗时：min {min(green_ms)} · 中位 {statistics.median(green_ms):.0f} · "
              f"max {max(green_ms)} · p90 {green_ms_sorted[int(len(green_ms_sorted) * 0.9)]} ms")
    swallowed = [r for r in red if r["recover"]]
    print(f"红轮里「再发一次 Ctrl+C 立刻恢复」（= 被吞，不是僵住）：{len(swallowed)} / {len(red)}")
    if swallowed:
        print("  恢复耗时：" + ", ".join(f"{r['recover_ms']}ms" for r in swallowed))
    stranded = [r["n"] for r in red if not r["recover"]]
    if stranded:
        print(f"⚠ 真正僵住（再发也不恢复）的轮次：{stranded}")
    weak = [r["n"] for r in red if r["after"]]
    if weak:
        print(f"⚠ 「after-stop 可见」在失败轮里**也判绿**的轮次：{weak}"
              f"（该判据被输入回显满足——弱判据）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
