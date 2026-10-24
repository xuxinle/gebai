"""逆向验证：逐条回退 ot_bench.judge() 的修复，确认回归测试真的会变红。

## 为什么必须做这一步（CONVENTIONS §7.1）

不做逆向验证的回归测试很容易变成「恒绿测试」——它看起来像护栏，实际什么都拦不住。
本轮的三处修复里，有两处（`incomplete` / `undecided`）如果不做这一步，
根本无法确认「把修复删掉之后测试会不会红」，因为**原始实现的输出恰好也是合法的**。

做法：把 `ot_bench.py` 备份 → 用文本替换注入回退 → 跑测试 → 恢复 → 再跑一次确认全绿。
每一条都单独回退（一条假设一条实验），不是一次性全删。

运行：py scripts/verify_judge_reverse.py
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TARGET = os.path.join(HERE, "ot_bench.py")
TEST = os.path.join(HERE, "test_ot_bench_judge.py")
BACKUP = os.path.join(HERE, "_ot_bench_backup.py")

# (名字, 原文, 回退成, 期望变红的测试关键词)
CASES = [
    (
        "跳过相对判据完整性（incomplete 检查）",
        '        missing = [k for k in REQUIRED_REF_KEYS if (ref or {}).get(k) is None]',
        '        missing = []  # 回退：不再检查参考字段完整性',
        "incomplete",
    ),
    (
        "把「没真正判」当通过（undecided 检查）",
        '    if notes:\n        bad.append({"leg": "undecided"',
        '    if False:\n        bad.append({"leg": "undecided"',
        "undecided",
    ),
    (
        "过亮改回绝对阈值",
        '        if ours_bright > ref_bright + 10:',
        '        if ours_bright > 20:  # 回退：绝对阈值',
        "过亮 100% / 参考也 100%",
    ),
    (
        "强度改回峰值",
        '            if ref_p95 and ours_p95 is not None and ours_p95 < ref_p95 * 0.5:',
        '            if ref.get("change_peak") and (motion.get("change_peak") or 0) < ref["change_peak"] * 0.35:',
        "峰值 999",
    ),
]


def run_test():
    r = subprocess.run([sys.executable, TEST], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if not os.path.exists(TARGET):
        print(f"✗ 找不到 {TARGET}")
        return 1

    code, out = run_test()
    if code != 0:
        print("✗ 基线状态下测试就没过，先修好再做逆向验证")
        print(out)
        return 1
    print("基线：测试全绿 ✓\n")

    shutil.copy2(TARGET, BACKUP)
    src = open(TARGET, encoding="utf-8").read()
    failures = []
    try:
        for name, old, new, expect in CASES:
            if old not in src:
                print(f"⚠ 回退点未命中，跳过：{name}")
                failures.append(name)
                continue
            open(TARGET, "w", encoding="utf-8").write(src.replace(old, new, 1))
            code, out = run_test()
            red = code != 0
            hit = expect in out
            ok = red and hit
            print(f"{'✓' if ok else '✗'} 回退「{name}」→ 测试{'变红' if red else '仍绿'}"
                  f"{'' if hit else '（没命中预期用例）'}")
            if not ok:
                failures.append(name)
    finally:
        shutil.copy2(BACKUP, TARGET)
        os.remove(BACKUP)

    code, out = run_test()
    print(f"\n恢复后：{'全绿 ✓' if code == 0 else '仍红 ✗'}")
    if code != 0:
        print(out)
        return 1
    if failures:
        print(f"\n✗ {len(failures)} 条回退未被测试拦住：{failures}")
        return 1
    print("全部回退都被拦住了——测试是真护栏，不是恒绿摆设")
    return 0


if __name__ == "__main__":
    sys.exit(main())
