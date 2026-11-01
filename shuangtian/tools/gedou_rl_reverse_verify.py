#!/usr/bin/env python3
"""格斗 RL 链路的**逆向验证**：把每个关键修复临时回退，确认断言真的变红。

## 为什么这个脚本比 e2e 更重要（在本项目里）

RL 链路失败起来是**静默的**：损失在降、熵在降、曲线在涨，只是机器人很笨。
这类问题没法靠"跑一遍看有没有报错"发现，只能靠"把修复回退掉，断言必须变红"
来证明护栏确实在拦。

本项目实际踩到的、本脚本覆盖的六条：

| 回退什么 | 应当变红的断言 | 不做会怎样 |
|---|---|---|
| 前向矩阵行列互换 | `verify_forward` 中止 | 训练收敛、进游戏不会打 |
| 观测 `cornered` 用绝对像素 | `rl_observation_is_egocentric` | 策略学到与窗口尺寸绑定的量 |
| 奖励 `damage_dealt` 符号取反 | `rl_reward_signs` / `--eval` 胜率 | 训练朝反方向收敛 |
| 攻击冷却去掉 | `rl_attack_cooldown_blocks_repeat` | "一直按踢腿" = 无限连 |
| 观测维度 +1（schema 不同步） | `rl_schema_matches_observation_dim` | 训练按错 stride 切数据 |
| 贴脸前进惩罚去掉 | 训练后动作分布变回 ~70% forward | 机器人贴身硬推、不出招 |

    ⚠ 已知限制（2026-11-01）：标记了 `skip_reason` 的三个案例需要“回退后真重编”，
    但构建系统目前不重编被 `#include` 的 `.cpp`（见 `docs/BACKLOG.md` P1）。
    默认会跳过它们并如实计数，不把“跳过”当成“通过”。

用法：
    py -3 tools/gedou_rl_reverse_verify.py                # 全部（自动跳过不可验的）
    py -3 tools/gedou_rl_reverse_verify.py --only cooldown
    py -3 tools/gedou_rl_reverse_verify.py --include-skipped   # 构建缺陷修好后用
"""

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = Path(__file__).resolve().parent.parent
ST = ROOT / "build" / "bin" / ("st.exe" if sys.platform == "win32" else "st")
# 构建与测试必须用**同一档**：建 dev 跑 debug = 回退的代码根本没被编译
# （CONVENTIONS §7.3 的假绿，实测在验证脚本自己身上又踩了一次）。
PROFILE = "debug"

# 每个案例：文件 / 原文 / 回退文 / 期望变红的用例名 / 说明
CASES = [
    {
        "name": "forward-matrix-transposed",
        "file": "examples/gedou/policy.cpp",
        "old": "      const float* weights = layer.weight.data() + row * layer.in;",
        # 回退文本必须**能编译过**（CONVENTIONS §7.3）：整体替掉那个循环体，
        # 而不是在它前面插一段——插一段会撞 `float sum` 重复声明。
        "new": "      // 逆向验证：按列取权重（行主序实现错误）\n"
               "      float sum = layer.bias[row];\n"
               "      for (std::size_t column = 0; column < layer.in; ++column) {\n"
               "        sum += layer.weight[column * layer.out + row] * current[column];\n"
               "      }\n"
               "      if (layer.tanh) sum = std::tanh(sum);\n"
               "      next[row] = sum;\n"
               "      continue;\n"
               "      const float* weights = layer.weight.data() + row * layer.in;",
        "expect": "policy_single_layer_matches_hand_computation",
        "suite": "rl_",
        "why": "前向按列取权重 → 输出与手算不符（训练/推理分岔的典型形态）",
    },
    {
        "name": "egocentric-uses-absolute-pixels",
        "file": "examples/gedou/rl_env.cpp",
        "old": "  const float edge_band = arena.width() * 0.04f;",
        "new": "  const float edge_band = 40.0f;   // 逆向验证：绝对像素",
        "expect": "rl_observation_is_egocentric",
        "suite": "rl_",
        "skip_reason": "构建缺陷（BACKLOG P1）：改了被 #include 的 .cpp 后 st build 报“重编 0”",
        "why": "墙角判定用绝对像素 → 换场地后观测变化，策略学到与窗口绑定的量",
    },
    {
        "name": "reward-sign-flipped",
        "file": "examples/gedou/rl_env.cpp",
        "old": "  float reward = config_.reward.damage_dealt * (dealt / opp_max) -",
        "new": "  float reward = -config_.reward.damage_dealt * (dealt / opp_max) -   // 逆向验证",
        "expect": "rl_reward_signs",
        "suite": "rl_",
        "skip_reason": "构建缺陷（BACKLOG P1）：改了被 #include 的 .cpp 后 st build 报“重编 0”",
        "why": "造成伤害给负分 → 策略学会不打人",
    },
    {
        "name": "cooldown-removed",
        "file": "examples/gedou/rl_env.cpp",
        "old": "  if (attack_cooldown_left_ > 0) {",
        # 回退文本要**能编译且真改变行为**（`if (false)` 会撞 -Werror=unused 提示，
        # 且语义上等于“永不冷却”）。
        "new": "  if (attack_cooldown_left_ < -99999) {   // 逆向验证：去掉冷却",
        "expect": "rl_attack_cooldown_blocks_repeat",
        "suite": "rl_",
        "skip_reason": "构建缺陷（BACKLOG P1）：改了被 #include 的 .cpp 后 st build 报“重编 0”",
        "why": "没有冷却 → 每帧重按攻击 = 无限连（策略退化的直接原因）",
    },
    {
        "name": "observation-dim-drift",
        "file": "examples/gedou/rl_env.cpp",
        "old": '    {"time_left", "回合剩余时间比（1 = 刚开局，0 = 到时）", 0.0f, 1.0f},',
        "new": '    {"time_left", "回合剩余时间比（1 = 刚开局，0 = 到时）", 0.0f, 1.0f},\n'
               '    {"drift", "逆向验证：多一个分量但 observe 不产出", 0.0f, 1.0f},',
        "expect": "rl_schema_matches_observation_dim",
        "suite": "rl_",
        "why": "schema 与 observe 漂移 → Python 按错 stride 切数据（学到的是乱数）",
    },
]


def build(target):
    """构建（不带 target = 工程默认：框架库 + 测试目标）。

    为何不能传 `gedou_rl`：测试跑的是 `build/debug/bin/st_tests.exe`，而
    `rl_env.cpp` 是被**测试单元自己** `#include` 进去的。`st build gedou_rl`
    只会重编 `gedou_rl.exe` 里的那份，测试目标里的那份不动。

    ⚠ 但**即使构建测试目标也修不了**（实测）：`st build` 判定“测试二进制过期”
    只看 `tests/*_test.cpp` 自身的内容哈希，不看被 include 的实现文件
    ——见 `docs/BACKLOG.md` P1。所以依赖这种回退的案例都标了 `skip_reason`。
    """
    proc = subprocess.run([str(ST), "build", "--profile", PROFILE], cwd=ROOT,
                          capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=1800)
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def run_tests(suite, case_name):
    proc = subprocess.run([str(ST), "test", case_name, "--profile", PROFILE], cwd=ROOT,
                          capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=1800)
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", default="")
    parser.add_argument("--include-skipped", action="store_true",
                        help="也跑标记了 skip_reason 的案例（构建缺陷修好后用）")
    args = parser.parse_args()
    cases = [c for c in CASES if not args.only or c["name"] == args.only]
    if not cases:
        print(f"没有匹配的案例：{args.only}")
        return 2

    skipped = 0
    failures = 0
    for case in cases:
        if case.get("skip_reason") and not args.include_skipped:
            skipped += 1
            print(f"\n=== {case['name']} —— SKIP：{case['skip_reason']}")
            continue
        path = ROOT / case["file"]
        original = path.read_text(encoding="utf-8")
        print(f"\n=== {case['name']} —— {case['why']}")
        if case["old"] not in original:
            print(f"  [ABORT] 回退锚点未命中（锚点已过期）：{case['file']}")
            failures += 1
            continue
        patched = original.replace(case["old"], case["new"], 1)
        path.unlink()
        path.write_text(patched, encoding="utf-8")
        try:
            stamp = time.time()
            code, log = build(None)
            if code != 0:
                # 回退文本编译不过 ⇒ 跑的是旧二进制 ⇒ 结论无效（CONVENTIONS §7.3）
                print("  [ABORT] 回退版编译失败（跑的是旧二进制，结论无效）")
                print("  " + log.strip().replace("\n", "\n  ")[:800])
                failures += 1
                continue
            rc, out = run_tests(case["suite"], case["expect"])
            if rc == 0:
                print("  [FAIL] 回退后测试仍然全绿 ⇒ 该断言抓不住这个缺陷")
                failures += 1
            elif case["expect"] not in out:
                print(f"  [FAIL] 测试变红了，但不是预期用例（期望 {case['expect']}）")
                print("  " + out.strip().replace("\n", "\n  ")[-800:])
                failures += 1
            else:
                line = [l for l in out.splitlines() if "FAIL" in l]
                print(f"  [OK] 回退后断言变红：{line[0].strip() if line else case['expect']}")
        finally:
            path.unlink()
            path.write_text(original, encoding="utf-8")
            code, _ = build(None)
            if code != 0:
                print("  !!! 恢复后编译失败（工作树被破坏，请手工检查）")
                failures += 1
                continue
            rc, _ = run_tests(case["suite"], case["expect"])
            if rc != 0:
                print("  !!! 恢复后仍然红（工作树不干净）")
                failures += 1
            else:
                print("  [OK] 恢复后回绿")

    print(f"\n{'全部案例通过' if failures == 0 else f'{failures} 个案例有问题'}"
          f"（{skipped} 个因构建缺陷跳过）")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
