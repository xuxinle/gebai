"""判据自完整性回归测试 —— 防「尺子失效时静默通过」

## 为什么要有这份测试

同一形态的缺陷在这套对标流水线里出现了三次，前两次都是靠事后发现的：

1. 并行渲染的静止占比**分母恒为 0**，而摘要毫无异常（已在 C++ 侧修复+测试）；
2. 用「变化峰值」当强度判据 —— motion-web 的峰值 234 来自**一帧硬切黑场**；
3. `judge()` 里每条相对判据在参考值缺失时**静默跳过** ⇒ violations 为空 ⇒ 脚本返回 0 ⇒
   调用方看到绿色 PASS，而实际上**一条判据都没跑**。

第 3 类最危险：它不会让结果变难看，只会让结果变得**看起来正常**。
所以这里把「没真正判」也当成失败态来测。

运行：py scripts/test_ot_bench_judge.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ot_bench import REQUIRED_REF_KEYS, judge  # noqa: E402

FAILS = []


def check(name, cond, detail=""):
    print(f"{'✓' if cond else '✗'} {name}{'' if cond else '  ← ' + detail}")
    if not cond:
        FAILS.append(name)


BASE_Q = {"still_ratio": 50.0, "bright_ratio": 10.0, "pairs_unreliable": False}
BASE_M = {"change_mean": 1.0, "change_median": 1.0, "change_p95": 5.0}
FULL_REF = {"still_ratio": 50.0, "bright_ratio": 10.0,
            "change_median": 1.0, "change_p95": 5.0}


def legs(*a, **kw):
    return [v["leg"] for v in judge(*a, **kw)]


def main():
    # Windows 控制台默认 GBK，输出 ✓/✗ 会抛 UnicodeEncodeError（实测）
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    # ① 基线：参考齐全且各项都达标 → 不报任何东西
    check("完整基线 + 全部达标 → PASS", legs(BASE_Q, BASE_M, FULL_REF) == [],
          str(legs(BASE_Q, BASE_M, FULL_REF)))

    # ② 参考少一个字段 → 必须报 incomplete（而不是当那条通过）
    for key in REQUIRED_REF_KEYS:
        crippled = {k: v for k, v in FULL_REF.items() if k != key}
        got = legs(BASE_Q, BASE_M, crippled)
        check(f"参考缺 {key} → incomplete", "incomplete" in got, str(got))

    # ③ 完全没给参考片 → 必须报 undecided（这是最危险的一条：
    #    原来的实现在这种情况下 violations 为空、退出码 0）
    got = legs(BASE_Q, BASE_M, None, ["未给 --ref-baseline"])
    check("没给参考片 → undecided", "undecided" in got, str(got))
    got = legs(BASE_Q, BASE_M, None)
    check("没给参考片且没留说明 → 仍 PASS（无相对判据可判，由调用方负责说明）",
          got == [], str(got))

    # ④ 过亮必须**相对**判：与参考同调性的高调片不能被判红
    hi_q = dict(BASE_Q, bright_ratio=100.0)
    hi_ref = dict(FULL_REF, bright_ratio=100.0)
    check("我们过亮 100% / 参考也 100% → 放行（相对口径）",
          "bright" not in legs(hi_q, BASE_M, hi_ref), str(legs(hi_q, BASE_M, hi_ref)))
    lo_ref = dict(FULL_REF, bright_ratio=0.0)
    check("我们过亮 100% / 参考 0% → 判红",
          "bright" in legs(hi_q, BASE_M, lo_ref), str(legs(hi_q, BASE_M, lo_ref)))
    # 参考 90% 我们 96% → +6pp，在 +10pp 容差内，放行
    check("相对高 6pp → 放行", "bright" not in legs(
        dict(BASE_Q, bright_ratio=96.0), BASE_M, dict(FULL_REF, bright_ratio=90.0)))

    # ⑤ 强度必须用 p95 而不是峰值：给一个 p95 低但峰值极高的情形
    spike_m = {"change_mean": 3.0, "change_median": 1.0, "change_p95": 1.0,
               "change_peak": 999.0}
    check("峰值 999 但 p95 低 → 仍判 intensity（不被一次硬切骗过）",
          "intensity" in legs(BASE_Q, spike_m, FULL_REF), str(legs(BASE_Q, spike_m, FULL_REF)))

    # ⑥ 静止占比必须与参考比，不是绝对阈值
    check("我们静止 60 / 参考 55 → 放行（+5pp）", "rest" not in legs(
        dict(BASE_Q, still_ratio=60.0), BASE_M, dict(FULL_REF, still_ratio=55.0)))
    check("我们静止 80 / 参考 55 → 判红（+25pp）", "rest" in legs(
        dict(BASE_Q, still_ratio=80.0), BASE_M, dict(FULL_REF, still_ratio=55.0)))

    # ⑦ 分母为 0 时必须报（静止占比不可信）
    q = dict(BASE_Q, pairs_unreliable=True, frame_pairs=0)
    check("有效帧对为 0 → 报 still 不可信", "still" in legs(q, BASE_M, FULL_REF))

    print()
    if FAILS:
        print(f"✗ {len(FAILS)} 项失败：{FAILS}")
        return 1
    print("全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
