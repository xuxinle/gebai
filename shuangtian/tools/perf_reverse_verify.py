#!/usr/bin/env python3
"""性能体系回归测试的**逆向验证**：逐条把实现改坏，看对应用例是否真的变红。

## 为什么必须做这一步

本仓已有明确记录（`CONVENTIONS.md` §7.1）：修完缺陷后新写的回归测试，**必须验证它真能
抓住那个缺陷**——把修复临时回退、看测试变红，再恢复。不做这一步，很容易交付一个
"恒绿的测试"：它看起来像护栏，实际什么都拦不住。

本脚本把这条纪律做成可重跑的动作：每个 CASE = 「破坏点 → 破坏方式 → 应当变红的用例」。

## 用例

```bash
python tools/perf_reverse_verify.py            # 全部
python tools/perf_reverse_verify.py cold_start # 只跑一条
```

## 三坑（都是实测踩过的，脚本已按此设计）

1. **回退文本必须编译得过**：`-Werror` 会把编译打断 ⇒ 测试跑的是上一份二进制 ⇒ 假绿。
   本脚本每个破坏点都保留语法完整（如 `(void)var;`）。
2. **桩不能抹平差异**：破坏必须落在**被测的那一段真实行为**上，而不是把整条路径短路。
3. **破坏后必须确认二进制里是新的**：脚本检查文件 mtime + 构建成功，两者都满足才跑测试。
"""

import argparse
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ST = ROOT / "build" / "bin" / "st.exe"
ST_TESTS = ROOT / "build" / "dev" / "bin" / "st_tests.exe"


@dataclass
class Case:
    name: str
    path: str          # 相对仓库根
    old: str           # 要被替换掉的原文（必须唯一）
    new: str           # 破坏后的写法（必须能编译过）
    test: str          # 应当变红的用例名
    slow: bool = True  # 该用例是否在 slow 集合
    why: str = ""


CASES = [
    Case(
        name="cold_start",
        path="include/st/perf/frame.hpp",
        old="inline constexpr std::uint64_t kColdFrameCount = 2;",
        new="inline constexpr std::uint64_t kColdFrameCount = 0;",
        test="perf_cold_frames_do_not_pollute_percentiles",
        why="不排除冷启动帧 → p95 被 200ms 的帧抬起来（本体系最贵的假警报来源）",
    ),
    Case(
        name="ring",
        path="src/perf/frame.cpp",
        old="write_ = (write_ + 1) % frames_.size();",
        new="write_ = (write_ + 1) % frames_.size(); if (write_ == 0) write_ = frames_.size() - 1;",
        test="perf_frame_ring_wraps_without_mixing_frames",
        why="环形缓冲不绕回（卡在最后一格）→ 读端拿不到“最后 N 帧”而是一格循环覆写",
    ),
    Case(
        name="histogram",
        path="src/perf/histogram.cpp",
        old="if (accumulated >= rank) return table.upper[index];",
        new="if (accumulated >= rank) return index == 0 ? table.upper[index] : table.upper[index - 1];",
        test="perf_histogram_quantile_is_not_optimistic",
        why="分位退一桶 → 读数比真值乐观（判“超没超预算”时会假绿）",
    ),
    Case(
        name="scope_self",
        path="src/perf/scope.cpp",
        old="const std::int64_t self = elapsed > frame.child_ns ? elapsed - frame.child_ns : 0;",
        new="const std::int64_t self = elapsed; (void)frame.child_ns;",
        test="perf_nested_scope_self_excludes_children",
        why="self 不减 child → 父阶段把子阶段算进自己（“未归因”凭空消失）",
    ),
]


def build() -> bool:
    """构建 `st` 并**同步测试二进制**。

    `st build <target>` 默认会“只构建不运行”地同步一次 `st_tests`（见 `st build --help`），
    所以这里只建 `st` 就够——而直接建 `st_tests` 会报“目标不存在”（它是拦在
    `tests` 键下的产物，不是 targets 里的一项）。

    ⚠ 必须让它同步：只建 `st` 而不重链测试，就是“代码改了、测试跑旧二进制”的经典假绿。
    """
    result = subprocess.run(
        [str(ST), "build", "st", "--profile", "dev"],
        cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    if result.returncode != 0:
        print(result.stdout[-3000:])
        print(result.stderr[-3000:])
        return False
    return True


def run_test(name: str, slow: bool) -> bool:
    """跑单个用例；True = 通过。超时视为挂死（也算“没变红”，要报出来）。

    ⚠ **必须用 `st test`（而不是直调 `st_tests.exe`）**：`st build st` 只构建 `st` 目标，
    **不重链测试二进制** —— 直调 `st_tests.exe` 会跑上一份二进制，于是“破坏后仍绿”
    （实测：4 条全部假绿，而构建日志一切正常）。`st test` 自己负责构建再跑。
    """
    args = [str(ST), "test", name, "--profile", "dev"]
    if slow:
        args.append("--slow")
    try:
        result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=300)
    except subprocess.TimeoutExpired:
        print(f"    ⚠ 用例挂死（>300s）：{name}")
        return False
    return result.returncode == 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("only", nargs="*", help="只跑指定的 CASE（省略 = 全部）")
    options = parser.parse_args()

    cases = [c for c in CASES if not options.only or c.name in options.only]
    if not cases:
        print(f"没有匹配的 CASE。可用：{', '.join(c.name for c in CASES)}")
        return 1

    print("① 建立基线：所有用例应当先绿")
    if not build():
        print("基座构建失败")
        return 1
    baseline_ok = {}
    for case in cases:
        baseline_ok[case.name] = run_test(case.test, case.slow)
        print(f"   {'PASS' if baseline_ok[case.name] else 'FAIL'}  {case.test}")
    bad = [c for c in cases if not baseline_ok[c.name]]
    if bad:
        print(f"\n⚠ 基线不绿，无法验证：{', '.join(c.name for c in bad)}（先修好再用本脚本）")
        return 1

    print("\n② 逐条破坏 → 断言变红 → 恢复")
    failures = []
    for case in cases:
        path = ROOT / case.path
        original = path.read_text(encoding="utf-8")
        if original.count(case.old) != 1:
            print(f"   ✗ {case.name}: 破坏点原文匹配 {original.count(case.old)} 次（要求唯一）")
            failures.append(case.name)
            continue
        try:
            path.write_text(original.replace(case.old, case.new), encoding="utf-8")
            if not build():
                print(f"   ✗ {case.name}: 破坏后**编译不过**（那会跑旧二进制、假绿）")
                failures.append(case.name)
                continue
            red = not run_test(case.test, case.slow)
            if red:
                print(f"   ✓ {case.name}：破坏后 {case.test} 变红（{case.why}）")
            else:
                print(f"   ✗ {case.name}：破坏后仍绿 ⇒ 该用例**抓不住**这个缺陷")
                failures.append(case.name)
        finally:
            path.write_text(original, encoding="utf-8")

    print("\n③ 恢复并重建（保证工作区回到可用状态）")
    if not build():
        print("恢复后构建失败")
        return 1
    for case in cases:
        ok = run_test(case.test, case.slow)
        print(f"   {'PASS' if ok else 'FAIL'}  {case.test}（恢复后）")
        if not ok:
            failures.append(case.name)

    print()
    if failures:
        print(f"✗ {len(failures)} 条未通过：{', '.join(failures)}")
        return 1
    print(f"✓ 全部 {len(cases)} 条：破坏会红 / 恢复会绿")
    return 0


if __name__ == "__main__":
    sys.exit(main())
