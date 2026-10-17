#!/usr/bin/env python3
"""逆向验证：`retire mode=stop` 的**子孙进程树回收**与**跨会话保护**（本单的两条硬约束）。

为什么单独一份、而不是加进 `reverse_verify.py` 的 CASES：
  · `reverse_verify.py` 的 `run_tests()` 只跑 `shengsheng.test.ts` 一个文件，而本单的用例在
    `shengsheng-retire-tree.test.ts`（与实现同粒度：进程判据/干跑/真进程三层）；
  · 那个文件此刻正由另一条演进线改动（`reverse_verify.py` 亦在其未提交改动里），
    把本单的用例插进去会与它互相覆盖——而"两个门禁抢同一份源码"正是本仓已实证过的假红来源。

判据与 `reverse_verify.py` 同口径（CONVENTIONS §7.1/§7.3）：
  ① 回退必须是**能通过 tsc** 的最小改动（否则跑的是坏代码，红绿都不可信）；
  ② 恢复用**字节快照回填 + 回读逐字节校验**（不是"把备份拷回去"就完事）；
  ③ 判据是「指定用例变红」+「用例数不为 0」。

两条用例，对应本单的两条硬约束（缺任一条都会有假绿）：
  A. **退回「只杀会话」**（不回收子进程树）→ 真进程用例必须红。
     这条防的是「机制写了但没接上」——桩层若把 reap 结果抹平成常量，真进程层仍会红。
  B. **退回「按命令行猜归因」** → 跨会话用例必须红。
     这条防的是「把全机同名的都杀了」：不按环境标记认人时，别人的长跑进程会被一起回收。

用法：
    python3 reverse_verify_retire_tree.py            # 两条都跑
    python3 reverse_verify_retire_tree.py --list      # 只列用例
    python3 reverse_verify_retire_tree.py --force     # 越过并发检测（'bun test' 提示）
"""
from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "src" / "agents" / "shengsheng"))
sys.path.insert(0, str(HERE))  # 同目录的 verify_guard

import verify_guard  # noqa: E402

ROOT = HERE.parents[2]  # packages/agents/scripts → packages/agents → packages → 仓库根
SRC_DIR = ROOT / "packages/agents/src/agents/shengsheng"
SRC = SRC_DIR / "shengsheng.ts"
PROC = SRC_DIR / "proc-tree.ts"
TEST = SRC_DIR / "shengsheng-retire-tree.test.ts"
TSC = ROOT / "packages/agents/node_modules/.bin/tsc"
ROLLBACK_SUFFIX = ".revbak"
ANSI = re.compile("\x1b\\[[0-9;]*m")


class Case:
    def __init__(self, name: str, path: pathlib.Path, old: str, new: str, expect_fail: list[str], note: str):
        self.name = name
        self.path = path
        self.old = old
        self.new = new
        self.expect_fail = expect_fail
        self.note = note


CASES = [
    Case(
        "只杀会话：不回收子孙进程树",
        SRC,
        "        const reap: ReapResult | undefined = located.plan ? await reapProcesses(located.plan) : undefined",
        '        const reap = located.plan ? ((void reapProcesses, { outcome: "nothing", targets: [], survivors: [], log: [] } as ReapResult)) : undefined',
        ["真场景：会话的子进程 + 孙进程被回收"],
        "退回「只 engine.cancel」时，被终止 worker 的子进程会在会话死后继续改同一棵树（本单要消灭的那个缺陷）",
    ),
    Case(
        "去掉跨会话保护：别人的进程一并杀掉",
        PROC,
        "    if (marker && marker !== input.sessionId) {",
        "    if (false && marker && marker !== input.sessionId) {",
        ["标记指向别的会话的进程"],
        "丢掉「标记属别的会话就不碰」这一条，就等于「把全机同名的都杀了」——反向用例必须把它拦住（跨线祸及比不杀更坏）",
    ),
]


def run_tests() -> tuple[int, int, list[str]]:
    proc = subprocess.run(["bun", "test", str(TEST)], capture_output=True, text=True, timeout=600, cwd=str(ROOT))
    out = ANSI.sub("", proc.stdout + proc.stderr)
    passed = failed = 0
    names: list[str] = []
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("(pass)"):
            passed += 1
        elif line.startswith("(fail)"):
            failed += 1
            names.append(line[7:].strip())
    return passed, failed, names


def typecheck() -> bool:
    if not TSC.exists():
        return True
    return subprocess.run([str(TSC), "--noEmit"], cwd=str(ROOT / "packages/agents"), capture_output=True, timeout=600).returncode == 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--force", action="store_true", help="越过并发检测（别的 bun test 在跑时会误判）")
    args = parser.parse_args()
    if args.list:
        for c in CASES:
            print(f"- {c.name}（期望变红：{c.expect_fail}）")
        return 0

    g = verify_guard.preflight(tree=SRC_DIR, name="reverse_verify_retire_tree.py", force=args.force)
    if g.refused:
        return verify_guard.EXIT_CONCURRENT

    base_pass, base_fail, _ = run_tests()
    if base_pass == 0 or base_fail > 0:
        print(f"基线不绿（{base_pass} passed / {base_fail} failed）——先让基线全绿再逆向验证")
        return 2
    if not typecheck():
        print("基线类型检查失败——先修干净")
        return 2
    print(f"基线：{base_pass} passed / 0 failed\n")

    failures: list[str] = []
    try:
        for case in CASES:
            snapshot = case.path.read_bytes()
            source = snapshot.decode("utf-8")
            if case.old not in source:
                print(f"[跳过] {case.name}：回退锚点未命中（代码已变，请更新脚本）")
                failures.append(case.name)
                continue
            backup = case.path.with_name(case.path.name + ROLLBACK_SUFFIX)
            backup.write_bytes(snapshot)
            try:
                case.path.write_text(source.replace(case.old, case.new, 1), encoding="utf-8")
                if not typecheck():
                    print(f"[无效] {case.name}：回退版本类型检查失败（不许当通过）")
                    failures.append(f"{case.name}（回退版本编译不过）")
                    continue
                passed, failed, names = run_tests()
                if passed == 0 and failed == 0:
                    print(f"[无效] {case.name}：一条用例都没跑起来")
                    failures.append(f"{case.name}（无用例）")
                    continue
                hit = [n for n in names if any(k in n for k in case.expect_fail)]
                if hit:
                    print(f"[通过] {case.name}：回退后 {failed} 条变红，命中期望 {len(hit)} 条 —— 用例真能抓住（{case.note}）")
                else:
                    print(f"[失败] {case.name}：回退后没有期望的用例变红（失败清单：{names[:4]}）——这是一条**恒绿**的用例")
                    failures.append(case.name)
            finally:
                case.path.write_bytes(snapshot)
                backup.unlink(missing_ok=True)
                now = case.path.read_bytes()
                if now != snapshot:
                    print(f"[恢复失败] {case.name}：{case.path} 未回到开跑前（人工修：git checkout -- {case.path.relative_to(ROOT)}）")
                    failures.append(f"{case.name}（源码未恢复）")
    finally:
        # 锁必须由自己释放：`--force` 下 preflight 仍会 `lock.take()`（它只跳过并发判定），
        # 不释放就会在盘上留一把带**当前 pid** 的锁——下一轮要么等 pid 消失、要么误报「有人持锁」。
        g.release()
        g.report_end()

    if failures:
        print(f"\n{len(failures)} 条未通过：{', '.join(failures)}")
        return 1
    passed, failed, _ = run_tests()
    if failed or passed != base_pass:
        print(f"\n恢复后未回到基线（{passed} passed / {failed} failed）")
        return 2
    print(f"\n全部 {len(CASES)} 条逆向验证通过；已恢复基线（{passed} passed / 0 failed）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
