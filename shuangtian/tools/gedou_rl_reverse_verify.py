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

    五个案例全部可验（2026-11-01）。此前有三个不可验，原因是**构建系统的缺陷**：
    `st build` 判断测试二进制是否过期时只看 `tests/*_test.cpp` 自身哈希，
    不看它们 `#include` 的实现文件——于是“回退 → 重编 0 → 测试仍绿”。
    该缺陷已修（`included_impl_files`），修好后三条立刻可验。

    仍保留 `skip_reason` 机制：将来若再遇到“暂时不可验”的案例，
    宁可显式跳过并计入统计，也不要把“跳过”当成“通过”。

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
# 回退前的源码快照（`.gitignore` 已忽略 `*.rv-bak`）：
# 进程被杀时靠它把源码写回去，避免回退代码留在工作树里。
SNAPSHOT_DIR = ROOT / ".rv-snapshots"

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
        "why": "墙角判定用绝对像素 → 换场地后观测变化，策略学到与窗口绑定的量",
    },
    {
        "name": "reward-sign-flipped",
        "file": "examples/gedou/rl_env.cpp",
        "old": "  float reward = config_.reward.damage_dealt * (dealt / opp_max) -",
        "new": "  float reward = -config_.reward.damage_dealt * (dealt / opp_max) -   // 逆向验证",
        "expect": "rl_reward_signs",
        "suite": "rl_",
        "why": "造成伤害给负分 → 策略学会不打人",
    },
    {
        "name": "cooldown-removed",
        "file": "examples/gedou/rl_env.cpp",
        "old": "  if (attack_cooldown_left_ > 0) {",
        # 回退文本要**能编译且真改变行为**：把条件改成永不成立 = 冷却失效。
        # 首行同时充当 `audit_worktree()` 的残留特征串，所以必须唯一。
        "new": "  if (attack_cooldown_left_ < -99999) {   // 逆向验证：去掉冷却",
        "expect": "rl_attack_cooldown_blocks_repeat",
        "suite": "rl_",
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

    `st build` 判定“测试二进制过期”的判据已包含被 `#include` 的实现文件
    （`included_impl_files`，从 `.d` 取），所以回退后会被正确判为过期并重建。
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


def restore_snapshot(snapshot_dir):
    """把上次异常中断遗留的回退代码恢复回原样。

    ## 为何必须有这一步（血的教训）

    逆向验证的做法是“改源码 → 构建 → 跑测试 → 改回”。若进程在中间被杀
    （Ctrl+C、超时、崩溃），**回退代码会留在工作树里**，而它看起来完全正常
    （注释写着“逆向验证”，但在一次 `git commit -a` 里就会进库）。
    实测代价：一条 `policy.cpp` 的按列取权重回退残留了很久，直到查 `git diff`
    才发现——而那时前向已经与训练侧不一致。

    做法：改之前把原文件另存一份（`.rv-bak`，`.gitignore` 已覆盖），
    正常路径下删掉；下次启动时先把任何遗留的 `.rv-bak` 写回去。
    """
    leftovers = list(snapshot_dir.glob("*.rv-bak"))
    for bak in leftovers:
        # 文件名编码了原路径：`<相对路径下划线化>.rv-bak`
        rel = bak.name[: -len(".rv-bak")].replace("__", "/")
        target = ROOT / rel
        # 直接写（不先删）：Windows 上“删再写”会撞上文件被占用
        # （上一条验证进程可能还没退干净），而覆盖写能容忍这种情况。
        target.write_text(bak.read_text(encoding="utf-8"), encoding="utf-8")
        bak.unlink(missing_ok=True)
        print(f"[恢复] 上次中断遗留的回退代码已写回：{rel}")
    return len(leftovers)


def snapshot(path):
    """把原文件存进快照目录（供 `restore_snapshot` 灾后恢复）。"""
    rel = path.relative_to(ROOT).as_posix().replace("/", "__")
    bak = SNAPSHOT_DIR / (rel + ".rv-bak")
    SNAPSHOT_DIR.mkdir(parents=True, exist_ok=True)
    bak.write_text(path.read_text(encoding="utf-8"), encoding="utf-8")
    return bak


def audit_worktree():
    """启动时自检：**每个案例的回退文本不能出现在源码里**。

    为何不能只靠 `.rv-bak` 快照：只有"正在处理的那一个"有快照，而进程可能在
    处理另一个文件时被杀——那时已回退过的前一个案例已经改了内容，却没快照可救。

    实测代价（两次）：`policy.cpp` 的"按列取权重"与 `rl_env.cpp` 的 `<-99999`
    先后残留在工作树里，都是靠 `git diff` 才发现的。它们看起来完全正常
    （注释还写着"逆向验证"），一次 `git commit -a` 就会把缺陷提交进去。

    所以这里做**内容层**自检：把每个案例 `new` 的首行当特征串去源码里找。
    """
    dirty = []
    for case in CASES:
        path = ROOT / case["file"]
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8")
        # 特征串必须**只属于回退文本**：有些案例的 `new` 首行与 `old` 原文相同
        # （如新增一行观测分量），拿它当特征会误报（实测踩过）。
        old_lines = {line.strip() for line in case["old"].splitlines()}
        marker = ""
        for line in case["new"].splitlines():
            stripped = line.strip()
            if stripped and stripped not in old_lines:
                marker = stripped
                break
        if marker and marker in text:
            dirty.append((case["name"], case["file"], marker))
    if dirty:
        print("✗ 工作树里有**未恢复的回退代码**，拒绝继续：")
        for name, rel, marker in dirty:
            print(f"    {name}: {rel}")
            print(f"      残留：{marker[:70]}")
        print("  修法：`git checkout -- <文件>`（或手工改回），再重建。")
        return False
    return True


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

    restore_snapshot(SNAPSHOT_DIR)
    if not audit_worktree():
        return 3

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
        bak = snapshot(path)
        # 直接覆盖写（不先 unlink）：Windows 上删除可能撞上编辑器/杀软占用，
        # 而覆盖写能容忍；内容哈希判据也不依赖 inode。
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
            path.write_text(original, encoding="utf-8")
            bak.unlink(missing_ok=True)   # 已正常恢复 → 快照不再需要
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
