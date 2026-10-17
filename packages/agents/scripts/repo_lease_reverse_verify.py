#!/usr/bin/env python3
"""仓级资源租约（跨演进线）的**逆向验证**门：逐条回退 → 断言必须变红 → 字节级恢复。

为什么要单独一份（而不是加进 `reverse_verify.py` 的 CASES）：
  · 那个脚本跑满全套 10 条用例、约 8~10 分钟，而本单只需要证明**两个新机制**真有判别力；
  · 更重要的是**并发安全**：本单施工期间同树还有另一条演进线（歌白根）在改同一个
    `shengsheng.ts`。独立的短脚本能把「回退 → 跑测 → 回填」压到十几秒，并且
    在回填前**检查源码是否被别人改过**（被别人改过就中止并大声报告，绝不覆盖别人的活）。

两条用例（与工单要求一一对应）：
  ① 「仓级资源名」退回「只按项目根」——即 `repoResourceName` 恒等于本地资源名。
     预期变红：同仓 A 占 → B 顺延（名字不再带 git 顶层，B 的仓级表里根本查不到 A 那条）、
     以及资源名层的那条纯函数断言。
  ② 「跨线顺延」退回「照旧开跑」——即 claimLease 里仓级 blocked 分支失效（`req.minutes < 0`
     永不成立）。预期变红：B 根派工被顺延的那条（它会直接开跑，`t.b.sent` 从 0 变 1）。

判据与 `reverse_verify.py` 同口径（CONVENTIONS §7.1/§7.3）：
  · 回退文本必须**能过 tsc**（否则跑到的是旧/坏代码 = 假绿）；
  · 判据是「指定用例变红」+「用例数不为 0」；
  · 恢复必须**逐字节**校验，且只在「源码仍是我回退的那一版」时才覆盖。

用法：python3 packages/agents/scripts/repo_lease_reverse_verify.py [--list]
"""
import hashlib
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[3]          # /home/x/code/gebai
AGENTS = ROOT / "packages" / "agents"
SRC = AGENTS / "src" / "agents" / "shengsheng" / "shengsheng.ts"
TEST = AGENTS / "src" / "agents" / "shengsheng" / "shengsheng.test.ts"
TSC = AGENTS / "node_modules" / ".bin" / "tsc"
BUN = "bun"
ANSI = re.compile(r"\x1b\[[0-9;]*m")


class Case:
    def __init__(self, name, old, new, expect_fail, note):
        self.name = name
        self.old = old
        self.new = new
        self.expect_fail = expect_fail
        self.note = note


CASES = [
    Case(
        "仓级资源名：退回“只按项目根”（名字不再带 git 顶层）",
        # 回退：让仓级资源名退化成本地资源名——即“没有仓级概念”的旧行为。
        # 用 `void top` 而不是删形参：保留签名，`-Werror` 风格的 noUnusedParameters 不会把回退版打红，
        # 于是跑到的确定是回退版而不是“编译失败的上一版”（CONVENTIONS §7.3 的假绿形态）。
        """export function repoResourceName(top: string, resource: string): string {
  return `repo:${rootKey(top)}:${resourceKey(resource)}`
}""",
        """export function repoResourceName(top: string, resource: string): string {
  void top
  return resourceKey(resource)
}""",
        ["仓级租约：资源名带 git 顶层", "仓级租约：git 顶层把"],
        "名字不带顶层时，两条线各自的仓级表键不同 → 互不可见（本单要消灭的那个缺陷原形）",
    ),
    Case(
        "跨线顺延：退回“照旧开跑”（仓级 blocked 分支失效）",
        # 回退点：仓级冲突判定退化为恒不成立。
        # `req.minutes < 0` 永不成立（leaseMinutes 保证 ≥ 1）——于是异线持着也照发租约，
        # 即**后来的持者直接顶掉前一位**：两条线的负载敏感测量同时开跑，正是 196% CPU 那次的原形。
        # 用表达式而不是字面量 `false`：字面量可能触发 unreachable-code 诊断，让回退版编译不过。
        """      const holder = repoLeaseOf(cur, repoName)
      if (holder && !leaseHeldBy(holder, req.sessionId, req.alias)) return { result: { kind: "blocked", holder } }""",
        """      const holder = repoLeaseOf(cur, repoName)
      if (holder && !leaseHeldBy(holder, req.sessionId, req.alias) && req.minutes < 0) return { result: { kind: "blocked", holder } }""",
        ["仓级租约：资源名带 git 顶层", "仓级租约：不同 git 仓"],
        "回退后异线持着也照开跑：同仓两条线的测量同时进入，跨线互斥形同虚设。",
    ),
]


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()[:16]


def write_atomic(path: pathlib.Path, data: bytes) -> None:
    """同目录临时文件 + os.replace：读者要么看到旧内容、要么看到新内容，没有半个文件。"""
    tmp = path.with_name(path.name + ".rvrepo-tmp")
    with open(tmp, "wb") as fh:
        fh.write(data)
        fh.flush()
        os.fsync(fh.fileno())
    os.replace(tmp, path)
    stamp = time.time()
    os.utime(path, (stamp, stamp))


def run_tests():
    proc = subprocess.run([BUN, "test", str(TEST)], capture_output=True, text=True, timeout=900, cwd=str(ROOT))
    out = ANSI.sub("", proc.stdout + proc.stderr)
    passed = failed = 0
    names = []
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
    proc = subprocess.run([str(TSC), "--noEmit"], cwd=str(AGENTS), capture_output=True, text=True, timeout=600)
    return proc.returncode == 0


def main() -> int:
    if "--list" in sys.argv:
        for c in CASES:
            print(f"{c.name} → 期望红：{c.expect_fail}")
        return 0

    base_pass, base_fail, _ = run_tests()
    if base_fail or not base_pass:
        print(f"基线不绿（{base_pass} passed / {base_fail} failed）——先修好再跑逆向验证")
        return 2
    print(f"基线：{base_pass} passed / 0 failed（类型检查 {'OK' if typecheck() else '失败'}）\n")

    began = SRC.read_bytes()
    bad = []
    for case in CASES:
        snapshot = SRC.read_bytes()                    # 每条开跑前的字节快照 = 恢复的唯一真值
        text = snapshot.decode("utf-8")
        if case.old not in text:
            print(f"[跳过] {case.name}：回退锚点未命中（代码已变，请更新脚本）")
            bad.append(case.name)
            continue
        reverted = text.replace(case.old, case.new, 1).encode("utf-8")
        aborted = False
        try:
            write_atomic(SRC, reverted)
            if not typecheck():
                print(f"[无效] {case.name}：回退版本类型检查失败（不许当通过——那会跑旧/坏代码）")
                bad.append(case.name)
                continue
            passed, failed, names = run_tests()
            if passed == 0 and failed == 0:
                print(f"[无效] {case.name}：一条用例都没跑起来（不算通过）")
                bad.append(case.name)
                continue
            # 同一用例名可能多次出现（bun 会把慢用例同时打进逐条行与失败汇总），去重再报
            uniq = list(dict.fromkeys(names))
            hit = [n for n in uniq if any(k in n for k in case.expect_fail)]
            if hit:
                print(f"[通过] {case.name}：回退后 {failed} 条变红，命中期望 {len(hit)} 条 —— 测试真能抓住（{case.note}）")
                for n in hit:
                    print(f"        · {n}")
            else:
                print(f"[假绿] {case.name}：回退后 {failed} 条红，但**没命中**期望用例：{uniq}")
                bad.append(case.name)
        finally:
            # 回填前**先确认源码仍是我回退的那一版**：若被别人改过（同树有另一条演进线在写），
            # 拿我的快照覆盖就是抹掉别人的活。这种情况必须中止并大声报告，不能“尽力恢复”。
            now = SRC.read_bytes()
            if now != reverted and now != snapshot:
                print(f"\n!!! [中止] {case.name}：回退期间源码被**别的进程**改过（{sha(reverted)} → {sha(now)}）")
                print("!!! 不覆盖（宁可不恢复，也不抹掉别人的修改）。请人工检查 git diff 后重跑。")
                aborted = True
            else:
                write_atomic(SRC, snapshot)
                back = SRC.read_bytes()
                if back != snapshot:
                    print(f"!!! [恢复失败] {case.name}：源码未回到原样（{sha(snapshot)} → {sha(back)}）")
                    print(f"!!! 人工修：cd {ROOT} && git checkout -- {SRC.relative_to(ROOT)}")
                    bad.append(case.name)
                else:
                    print(f"[恢复] {case.name}：源码已回填并逐字节校验通过（{len(snapshot)} 字节，sha {sha(snapshot)}）")
        if aborted:
            return 3

    tail = SRC.read_bytes()
    if tail != began:
        print("\n源码与起跑前不一致——工作树可能已损坏，请检查 git status")
        return 2
    passed, failed, _ = run_tests()
    if failed or passed != base_pass:
        print(f"\n恢复后未回到基线（{passed} passed / {failed} failed）——检查 git status")
        return 2
    if bad:
        print(f"\n{len(bad)} 条未通过逆向验证：{', '.join(bad)}")
        return 1
    print(f"\n全部 {len(CASES)} 条逆向验证通过；已恢复基线（{passed} passed / 0 failed）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
