#!/usr/bin/env python3
"""gedou 示例的**逆向验证**：把每个修复临时回退，确认 e2e 变红，再恢复。

## 为什么必须做

新写的回归测试有两种常见假绿（`CONVENTIONS.md` §7）：

1. **回退是空操作**——改的那一行根本不改变行为，于是"回退后仍绿"看起来像
   "测试无效"，实际是回退没生效；
2. **回退版编译不过**——编译失败后跑的是**上一份二进制**，于是恒绿。

本脚本对每个案例做三件事，缺一不可：
  ① 回退源码 → ② **确认真的重编成功**（构建输出必须有"完成"，且产物的修改时间在回退之后）
  → ③ 跑 e2e，期望**非 0** 退出；再恢复并确认 e2e 回绿。

用法：
    py -3 tools/gedou_reverse_verify.py [--only 案例名]
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = Path(__file__).resolve().parent.parent
ST = ROOT / "build" / "bin" / "st.exe"
E2E = ROOT / "tools" / "gedou_e2e.py"
BIN = ROOT / "build" / "dev" / "bin" / "gedou.exe"

# 每个案例：文件、原文、回退文、这条回退应当让哪一步断言变红、说明。
# 判据必须落在**行为**上（“回退后 e2e 必须失败”），而不是“文件改了”。
#
# `suite` 为空 = 跑 e2e；`suite == "gedou"` = 跑单测（把 `case_name` 作为过滤）。
# 为何要分两种：单位错不会让 AI 失效（继续靠近总能打到），e2e 的行为断言
# 会被 AI 自己的走位掩盖，只有“直接对拍阈值数字”的单测能钉住它。
CASES = [
    {
        "name": "hitbox-only-in-active-window",
        "file": "examples/gedou/fight.cpp",
        "old": "  return fighter.move_frame >= fighter.timing.startup &&\n"
               "         fighter.move_frame < fighter.timing.startup + fighter.timing.active;",
        "new": "  // 逆向验证：删掉帧窗口约束（起手帧也给判定）\n"
               "  (void)fighter.timing;\n"
               "  return true;",
        "expect_msg": "轻拳应有起手帧",
        "why": "判定盒应只在判定帧出现；改成恒真后“起手帧”计数会归零",
    },
    {
        "name": "facing-mirror",
        "file": "examples/gedou/fight.cpp",
        "old": "  const float sign = fighter.facing_right ? 1.0f : -1.0f;\n"
               "  const float near_edge = fighter.x + sign * spec.reach_near * kBodyScale;\n"
               "  const float far_edge = fighter.x + sign * spec.reach_far * kBodyScale;",
        # 回退文本必须**能编译过**：留一行 `(void)sign` 安抚 -Werror=unused-variable，
        # 否则构建失败、e2e 跑的是上一份二进制（典型假绿）。
        "new": "  const float sign = fighter.facing_right ? 1.0f : -1.0f;\n"
               "  (void)sign;   // 逆向验证：忽略朝向（永远朝右出拳）\n"
               "  const float near_edge = fighter.x + spec.reach_near * kBodyScale;\n"
               "  const float far_edge = fighter.x + spec.reach_far * kBodyScale;",
        "expect_msg": "判定盒必须落在它的",
        "why": "右侧角色的判定盒必须落在其左侧；忽略朝向之后会落在右侧",
    },
    {
        "name": "ko-not-ending-round",
        "file": "examples/gedou/fight.cpp",
        "old": "    phase_ = RoundPhase::Over;\n    end_timer_ = 120;\n  }\n  return true;\n}",
        "new": "    // 逆向验证：血空但回合不结束\n"
               "  }\n  return true;\n}",
        "expect_msg": "血空后应进入 over",
        "why": "血空之后必须进入 over 并给出胜者；去掉这一句则仍是 fight",
    },
    {
        "name": "victory-pose-overwritten",
        "file": "examples/gedou/fight.cpp",
        "old": "      if (me.hp > 0.0f && other.hp <= 0.0f && me.pose_lock == 0 &&\n"
               "          me.move != MoveId::Victory && me.move != MoveId::KnockDown) {",
        "new": "      // 逆向验证：不再给胜者切胜利姿态\n"
               "      if (me.hp > 0.0f && other.hp <= 0.0f && me.pose_lock == 0 && me.hp < -1.0f &&\n"
               "          me.move != MoveId::Victory && me.move != MoveId::KnockDown) {",
        "expect_msg": "胜方应进入胜利姿态",
        "why": "胜者应进 victory；去掉之后胜者停在 idle",
    },
    {
        "name": "ai-not-reset-on-restart",
        "file": "examples/gedou/game_view.cpp",
        "old": "  reset_ai();\n  player_intent_ = Intent{};",
        "new": "  // 逆向验证：重开一局不重置 AI\n  player_intent_ = Intent{};",
        "expect_msg": "同种子 AI 跑 120 帧应完全可复现",
        "why": "同种子两跑应完全一致；不重置 AI 时第二跑会带着上一局的决策",
    },
    {
        "name": "damage-from-wrong-table",
        "file": "examples/gedou/fighter.cpp",
        "old": "    /* Light  */ {42.0f, 18.0f, 0.0f, 58.0f, 96.0f, 12, 12.0f, false, 0.0f, false, 8.0f},",
        "new": "    /* Light  */ {43.0f, 18.0f, 0.0f, 58.0f, 96.0f, 12, 12.0f, false, 0.0f, false, 8.0f},",
        "expect_msg": "",
        "why": "伤害必须与招式表一致；改表之后 e2e 读的是新表（期望值同源，理论上仍应通过）"
               "——本案例反向证明“期望值来自进程而非脚本副本”这条约定的作用：它**应当保持绿**",
        "expect_green": True,
    },
    {
        "name": "reach-not-from-pose",
        "file": "examples/gedou/fighter.cpp",
        "old": "  if (is_attack(move)) spec.reach_far = strike_reach(move);",
        "new": "  (void)strike_reach;   // 逆向验证：距离手填、不从姿态反算",
        "expect_msg": "gedou_attack_reach_comes_from_pose",
        "suite": "gedou",
        "case_name": "gedou_attack_reach_comes_from_pose",
        "why": "招式距离必须由命中帧姿态反算；断了之后表与姿态分岔（盒与手脱节）",
    },
    {
        "name": "unit_ai_reach_scale",
        "file": "examples/gedou/ai.cpp",
        "old": "  return local * kBodyScale;",
        "new": "  (void)kBodyScale;\n  return local;   // 逆向验证：局部单位直接当世界用",
        "expect_msg": "gedou_ai_has_no_unit_mismatch",
        "suite": "gedou",
        "case_name": "gedou_ai_has_no_unit_mismatch",
        "why": "AI 的进攻阈值必须与招式表同单位；去掉 kBodyScale 后阈值偏小 40%",
    },
]


def run_check(case):
    """按案例类型跑 e2e 或单测，返回 (returncode, 输出)。"""
    if case.get("suite"):
        proc = subprocess.run(
            [str(ST), "test", case["case_name"], "--profile", "debug"], capture_output=True,
            text=True, encoding="utf-8", errors="replace", timeout=900)
        return proc.returncode, (proc.stdout or "") + (proc.stderr or "")
    proc = subprocess.run([sys.executable, str(E2E)], capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=600)
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def build():
    proc = subprocess.run([str(ST), "build", "gedou", "--profile", "dev"], capture_output=True,
                          text=True, encoding="utf-8", errors="replace", timeout=900)
    text = (proc.stdout or "") + (proc.stderr or "")
    return proc.returncode, text


def binary_fresh(since):
    return BIN.exists() and BIN.stat().st_mtime >= since - 1.0


def apply_case(case):
    path = ROOT / case["file"]
    original = path.read_text(encoding="utf-8")
    if case["old"] not in original:
        raise AssertionError(f"回退锚点未命中：{case['file']}（锚点已过期，请更新本脚本）")
    patched = original.replace(case["old"], case["new"], 1)
    if patched == original:
        raise AssertionError("回退是空操作（内容未变）")
    path.write_text(patched, encoding="utf-8")
    return path, original


def restore(path, original):
    # 恢复必须"删旧再写"：部分工具的 copy 会把旧 mtime 一起搬回来，
    # 让增量构建认为产物是新的（CONVENTIONS §7.3）。
    path.unlink()
    path.write_text(original, encoding="utf-8")


def main():
    only = None
    if "--only" in sys.argv:
        only = sys.argv[sys.argv.index("--only") + 1]
    cases = [c for c in CASES if only is None or c["name"] == only]
    if not cases:
        print(f"没有匹配的案例：{only}")
        return 2

    failures = 0
    for case in cases:
        print(f"\n=== {case['name']} —— {case['why']}")
        path, original = apply_case(case)
        try:
            stamp = time.time()
            code, log = build()
            if code != 0 or "完成" not in log:
                print(f"  [ABORT] 回退版编译失败（回退文本不可编译 ⇒ 跑的是旧二进制，结论无效）")
                print("  " + log.strip().replace("\n", "\n  ")[:1200])
                failures += 1
                continue
            if not binary_fresh(stamp):
                print("  [ABORT] 构建报成功但产物时间戳没更新（增量判定异常）")
                failures += 1
                continue
            rc, out = run_check(case)
            expect_green = case.get("expect_green", False)
            label = "单测" if case.get("suite") else "e2e"
            if expect_green:
                if rc == 0:
                    print("  [OK] 回退后仍绿（符合预期：期望值与实现同源，改表两边一起变）")
                else:
                    print(f"  [FAIL] 回退后变红（不符合预期）—— 期望值被抄成了脚本副本？")
                    print("  " + out.strip().replace("\n", "\n  ")[-1200:])
                    failures += 1
            else:
                if rc == 0:
                    print(f"  [FAIL] 回退后{label}仍然全绿 ⇒ 该断言抓不住这个缺陷")
                    failures += 1
                elif case["expect_msg"] not in out:
                    print(f"  [FAIL] {label}变红了，但不是预期的那条断言（期望：{case['expect_msg']}）")
                    print("  " + out.strip().replace("\n", "\n  ")[-1200:])
                    failures += 1
                else:
                    line = [l for l in out.splitlines() if "FAIL" in l]
                    print(f"  [OK] 回退后断言变红：{line[0].strip() if line else case['expect_msg']}")
        finally:
            restore(path, original)
            code, log = build()
            if code != 0:
                print("  !!! 恢复后编译失败（工作树已被破坏，请手工检查）")
                failures += 1
                continue
            rc, _ = run_check(case)
            if rc != 0:
                print("  !!! 恢复后仍然红（工作树不干净）")
                failures += 1
            else:
                print("  [OK] 恢复后回绿")

    print(f"\n{'全部案例通过' if failures == 0 else f'{failures} 个案例有问题'}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
