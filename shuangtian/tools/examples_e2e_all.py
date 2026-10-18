#!/usr/bin/env python3
"""五例 e2e 一键全跑入口（示例健康巡检机制化）。

把 docs/EXAMPLES_E2E.md 入口表的脚本**串行**跑一遍：单脚本失败不中断，
逐脚本一行结论，最后汇总；退出码 = 失败数（0 = 全绿）。
「表是人读的、脚本是机器跑的」——两者以脚本内 SEQUENCE 清单对齐，
清单与 tools/ 下实际文件的差异由对账自检暴露（见 main 尾部）。

用法：
  py -3 tools/examples_e2e_all.py                 # 默认：18 个断言型脚本
  py -3 tools/examples_e2e_all.py --with-bench    # 追加 bench 对照（无断言）
  py -3 tools/examples_e2e_all.py --with-debug    # 追加 gallery_declarative（需 debug 档）
  py -3 tools/examples_e2e_all.py --timeout 900   # 单脚本超时秒数（默认 900，超时计失败）

为什么串行不并行：并行会抢 CPU 互相拖慢——st test 16 片并行负载误伤的教训
（git_service/scale_gate 类性能敏感断言在抢载下会假红），e2e 里带帧耗时
断言的脚本（keepalive 系）同理。

档位例外（默认跳过并提示，避免「未构建的档位」被误计为脚本失败）：
  - st_visual_check.py      内部跑 dev + san 两档 → 缺 build/san/ 时跳过；
  - gallery_declarative_e2e.py 默认取 build/debug 档 → 默认跳过，--with-debug 才跑。
dev 档二进制缺失只警告不跳过——子脚本自己会以非 0 退出，那正是如实信号。
"""

import subprocess
import sys
import time
from pathlib import Path

# 非 UTF-8 控制台自愈：GBK 代码页下 print 中文/符号（如 '⇄'）会 UnicodeEncodeError 崩整跑；
# 已是 UTF-8 则不动，残余不可编字符以 '?' 兜底（宁可错印不崩）。
if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent

# ── 入口表清单（与 docs/EXAMPLES_E2E.md 对齐；改表须同步改这里） ──────────────
SEQUENCE = [
    "st_visual_check.py",            # dev + san 双档（缺 san 跳过，见 docstring）
    "st_editor_smoke.py",
    "gallery_declarative_e2e.py",    # debug 档例外（默认跳过，--with-debug 才跑）
    "gbcode_e2e.py",                 # 主全量走查，最久的一个
    "gbcode_e2e_session.py",
    "gbcode_compare_e2e.py",
    "gbcode_git_e2e.py",
    "gbcode_history_e2e.py",
    "gbcode_blame_e2e.py",
    "gbcode_hint_e2e.py",
    "gbcode_focus_ring_e2e.py",
    "gbcode_sidebar_keepalive_e2e.py",
    "gbcode_terminal_keepalive_e2e.py",
    "gbcode_terminal_scroll_e2e.py",
    # 真实使用者路径（真磁盘工作区 + 真 git 仓 + 真 shell）：功能面全绿时
    # 用户仍会翻车的那些点——BOM/CRLF 保真、Shift+Tab、终端忙闲语义、
    # 菜单/树行的零坐标驱动（2026-10-18 实测所得）。
    "gbcode_usability_e2e.py",
    "louyue_e2e.py",
    "caiyun_e2e.py",
    "xmlapp_e2e.py",
]
BENCH = ["gbcode_sidebar_keepalive_bench.py"]  # 无断言的对照测量，--with-bench 才跑

# 框架级/能力级 e2e——不在「示例 e2e」口径内（见 EXAMPLES_E2E.md 范围口径节），
# 对账时排除，避免每次全跑都误报「新脚本未入表」。
KNOWN_EXCLUDED = {
    "examples_e2e_all.py",      # 本入口
    "events_e2e.py",            # 控制通道事件流原语（跑 gallery 但断框架能力）
    "framework_gaps_e2e.py",    # ui.create/remove 能力面
    "input_action_e2e.py",      # Input 单行动作面原语
    "partial_patch_e2e.py",     # git 数据面（build_partial_patch 真仓验证）
    "split_view_e2e.py",        # SplitView 拖拽原语
    "theme_file_e2e.py",        # --theme-file 装载接线
    "visual_assert_e2e.py",     # 视觉断言原语（capture.hash/visual.diff）
}

DEV_APPS = ["gallery", "gbcode", "louyue", "caiyun", "xmlapp"]


def bin_path(profile: str, app: str) -> Path:
    suffix = ".exe" if sys.platform == "win32" else ""
    return ROOT / "build" / profile / "bin" / (app + suffix)


def skip_reason(name: str, with_debug: bool) -> str | None:
    """已知「档位例外」：返回跳过原因；None = 照跑。"""
    if name == "st_visual_check.py":
        # 内部跑 dev + san 两档，缺 san 会中途崩——那不是脚本失败，是档没建。
        if not (bin_path("san", "gallery").exists() and bin_path("san", "gbcode").exists()):
            return "需先 st build --profile=san（内部跑 dev + san 双档）"
    if name == "st_editor_smoke.py":
        # 与 gallery_declarative 同族：默认取 debug 档（ST_PROFILE 环境变量可改，
        # 但入口不注入——避免与子脚本自己的档位选择打架）。缺档时 0.2s 启动即死，
        # 那不是脚本失败是档没建；--with-debug 才真跑，缺档由脚本自己如实失败。
        if not with_debug:
            return "默认跳过（取 debug 档）——加 --with-debug 跑；需先 st build --profile=debug"
    if name == "gallery_declarative_e2e.py" and not with_debug:
        # 无条件默认跳过（debug 档恰好存在但过期时跑了会假红）；--with-debug 才真跑，
        # 那时缺档由脚本自己非 0 退出，是如实信号。
        return "默认跳过（取 debug 档）——加 --with-debug 跑；需先 st build --profile=debug"
    return None


def run_one(name: str, timeout: float) -> tuple[int | None, float, str | None]:
    """跑单个脚本，透传其输出；返回 (退出码, 耗时秒, 失败原因或 None)。"""
    start = time.monotonic()
    try:
        proc = subprocess.run([sys.executable, str(TOOLS / name)], timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, time.monotonic() - start, f"timeout（>{timeout:.0f}s）"
    return proc.returncode, time.monotonic() - start, None


def main() -> int:
    args = sys.argv[1:]
    with_bench = "--with-bench" in args
    with_debug = "--with-debug" in args
    timeout = 900.0
    if "--timeout" in args:
        timeout = float(args[args.index("--timeout") + 1])

    # dev 档预检：只警告不跳过——缺二进制时子脚本自己非 0 退出，正是如实信号。
    missing_dev = [a for a in DEV_APPS if not bin_path("dev", a).exists()]
    if missing_dev:
        print(f"[warn] dev 档二进制缺失：{'、'.join(missing_dev)}"
              f"——先跑 st build <例>，相关脚本将如实失败\n")

    plan = list(SEQUENCE) + (BENCH if with_bench else [])

    results: list[tuple[str, str, str]] = []  # (脚本, 状态行, 备注)
    print(f"=== 示例 e2e 全跑（串行 {len(plan)} 个，单脚本超时 {timeout:.0f}s）===\n")
    for index, name in enumerate(plan, 1):
        reason = skip_reason(name, with_debug)
        if reason:
            print(f"[{index}/{len(plan)}] [skip] {name} —— {reason}\n")
            results.append((name, "skip", reason))
            continue
        print(f"[{index}/{len(plan)}] {name}")
        code, seconds, note = run_one(name, timeout)
        if code == 0:
            print(f"  → 通过 ({seconds:.1f}s)\n")
            results.append((name, "pass", f"{seconds:.1f}s"))
        else:
            why = note if note else f"exit {code}"
            print(f"  → 失败（{why}，{seconds:.1f}s）\n")
            results.append((name, "fail", why))

    # ── 对账自检：清单外的 *_e2e.py = 表与本入口可能已腐化 ──────────────────────
    known = set(SEQUENCE) | set(BENCH) | KNOWN_EXCLUDED
    strays = sorted(p.name for p in TOOLS.glob("*_e2e.py") if p.name not in known)
    if strays:
        print("[对账] 发现清单外的新 e2e 脚本（请补入 docs/EXAMPLES_E2E.md 与本入口 SEQUENCE，"
              "或明确加入 KNOWN_EXCLUDED 并注明口径）：")
        for name in strays:
            print(f"  - {name}")
        print()

    # ── 汇总 ─────────────────────────────────────────────────────────────────
    passed = [r for r in results if r[1] == "pass"]
    failed = [r for r in results if r[1] == "fail"]
    skipped = [r for r in results if r[1] == "skip"]
    print("=== 汇总 ===")
    print(f"通过 {len(passed)} / 失败 {len(failed)} / 跳过 {len(skipped)}"
          + (f"（另有 {len(strays)} 个清单外脚本待对账）" if strays else ""))
    if failed:
        print("失败清单：")
        for name, _, why in failed:
            print(f"  - {name}（{why}）")
    return len(failed)


if __name__ == "__main__":
    sys.exit(main())
