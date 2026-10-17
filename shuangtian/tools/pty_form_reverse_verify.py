#!/usr/bin/env python3
"""逆向验证：`terminal_pty_stop_interrupts_without_killing_the_shell` 的新前置形态。

被测的修复是「长命令整条是一个前台作业」：`/bin/sh -c "echo …; exec sleep 60"`。
本脚本把它回退成**旧形态**，跑该用例多轮，要求：

  · 旧形态 **必须真的变红**（否则交付的修复没有判别力——只是换了个写法）；
  · 新形态 **零红**。

## 旧形态怎么回退（判别力的来源）

旧形态的结构缺陷是「标记由一个前台作业打出，被中断的目标是**另一个**前台作业」，
两者的交接（fork → exec → tcsetpgrp）实测在标记出现之后 **+0.6…2.3 ms**
（`tools/pty_form_probe.cpp` 的测量口径；负载越高越宽）。落进这段窗口的 `Ctrl+C`
由 bash 收下并丢弃，目标命令随后照常启动 ⇒ 用例红。

裸旧写法（`echo …; sleep 60`）的窗口只有 1–2 ms，**低负载下 20 轮零红**——
拿它当判据会把"复现不出"误读成"旧写法没问题"。本脚本因此默认用
**结构等价、窗口固定放大**的旧形态：在标记与目标之间放一个忽略 `SIGINT` 的前台作业，
它自己 `exec` 成 `sleep 60`：

    echo MARK; sh -c 'trap "" INT; sleep 0.3; exec sleep 60'

这段延迟是**结构性**的（同一机制：标记之后还有一次前台作业交接），不是人为拖时间；
实测 8/8 红时目标 `sleep 60` 仍在运行（正是被测语义要排除的状态）。
`--raw` 可跑裸旧写法作对照（结果依赖负载，仅作记录，不作为判据）。

⚠ 两条纪律（都是本仓实测踩过的坑）：
  ① 回退文本必须**能编译过**——否则 `-Werror` 打断编译，跑的是上一份二进制 = 假绿。
  ② 恢复必须**删旧文件再写新内容**（`copy2` 会把旧 mtime 搬回来，增量构建按 mtime
     判定 → 继续用回退版的二进制 = 假绿）。

用法：python3 tools/pty_form_reverse_verify.py [每形态轮数] [--raw] [--load N]
"""

from __future__ import annotations

import os
import re
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_SRC = os.path.join(ROOT, "tests", "ui_terminal_pty_test.cpp")
TEST = "terminal_pty_stop_interrupts_without_killing_the_shell"

NEW = '  const std::string long_cmd = "/bin/sh -c \\"echo \'ru\'\'nning-9271\'; exec sleep 60\\"";'
# 结构等价的旧形态 + 固定放大窗口（见模块 docstring）。
OLD_DET = ('  const std::string long_cmd = "echo \'ru\'\'nning-9271\'; '
           'sh -c \'trap \\"\\" INT; sleep 0.3; exec sleep 60\'";')
# 裸旧形态（1–2 ms 窗口，低负载复现不出；仅 --raw 对照用）。
OLD_RAW = '  const std::string long_cmd = "echo \'ru\'\'nning-9271\'; sleep 60";'

ANSI = re.compile(r"\x1b\[[0-9;]*m")
BURN = "while :; do :; done"


class LoadGenerator:
    """纯 CPU 争用（**不碰构建目录**）。

    为什么不用 `st build --force` 当负载：并发构建会同时写同一个 `build/dev/obj`
    与链接产物（实测 `undefined reference` 一堆，因为别人正在替换 `.o`；
    更糟的是 `~/.shuangtian/cache/objects` 的共享对象会被写成 0 字节）。
    负载生成器必须**只吃 CPU、不共享文件**。

    每个 burner 用 `timeout` 把守：**本进程被 kill 也不会留下孤儿空转进程**
    （实测教训：早退路径漏关，留下 9 个空转把机器压到 load 53）。
    """

    def __init__(self, count: int = 9):
        self.count = count
        self.procs: list[subprocess.Popen] = []

    def __enter__(self):
        for _ in range(self.count):
            self.procs.append(
                subprocess.Popen(
                    ["timeout", "3600", "sh", "-c", BURN],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    start_new_session=True,
                )
            )
        time.sleep(3.0)
        return self

    def __exit__(self, *_exc):
        for proc in self.procs:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except OSError:
                pass
        for proc in self.procs:
            try:
                proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                proc.kill()
        return False


def load1() -> float:
    with open("/proc/loadavg", encoding="utf-8") as handle:
        return float(handle.read().split()[0])


def write_fresh(path: str, text: str) -> None:
    """删除后重写：拿到当前 mtime，避免增量构建命中旧 .o（CONVENTIONS §7.3）。"""
    if os.path.exists(path):
        os.remove(path)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


def run_once() -> tuple[str, str]:
    """跑一轮单用例。返回 ('pass'|'fail'|'compile-error', 输出)。"""
    proc = subprocess.run(
        [os.path.join(ROOT, "build", "bin", "st"), "test", "--profile", "dev", TEST],
        cwd=ROOT,
        capture_output=True,
        timeout=1800,
    )
    out = (proc.stdout or b"").decode("utf-8", "replace") + (
        proc.stderr or b""
    ).decode("utf-8", "replace")
    out = ANSI.sub("", out)
    # ⚠ 判「编译成功」不能用退出码：测试红了也是非 0，而那正是我们要的结果。
    if "编译失败" in out or " error: " in out:
        return "compile-error", out
    return ("pass" if f"PASS {TEST}" in out else "fail"), out


def run_form(label: str, rounds: int) -> tuple[int, list[str], str | None]:
    """跑一个形态；返回（红数, 逐轮负载, 编译错误输出或 None）。"""
    red = 0
    loads: list[str] = []
    for i in range(rounds):
        loads.append(f"{load1():.2f}")
        state, out = run_once()
        if state == "compile-error":
            return red, loads, out
        if state == "fail":
            red += 1
            print(f"   {label} 轮 {i + 1}: 红", flush=True)
    return red, loads, None


def main() -> int:
    argv = sys.argv[1:]
    flags: list[str] = []
    numbers: list[str] = []
    burners = 0
    i = 0
    while i < len(argv):
        if argv[i] == "--load":
            burners = int(argv[i + 1])
            i += 2
            continue
        if argv[i].startswith("--"):
            flags.append(argv[i])
        else:
            numbers.append(argv[i])
        i += 1
    rounds = int(numbers[0]) if numbers else 10

    original = open(TEST_SRC, encoding="utf-8").read()
    if NEW not in original:
        print("未找到新写法（源码已改动？）：\n" + NEW)
        return 2

    print(f"负载起点 {load1():.2f} · 每形态 {rounds} 轮 · 加压 {burners} 路\n")
    results: dict[str, tuple[int, list[str]]] = {}
    burn = LoadGenerator(burners) if burners > 0 else None
    try:
        if burn is not None:
            burn.__enter__()
            print(f"加压后负载 {load1():.2f}\n")

        # ① 旧形态（结构等价 + 固定放大窗口）：必须红
        print("① 旧形态（标记与目标分属两个前台作业，交接窗口放大到 300ms）")
        write_fresh(TEST_SRC, original.replace(NEW, OLD_DET, 1))
        old_red, old_loads, err = run_form("旧", rounds)
        if err is not None:
            print(err[-2500:])
            print("⚠ 回退文本编译不过 —— 这会产生假绿，必须修回退写法")
            return 1
        print(f"   → 红 {old_red}/{rounds}（负载 {old_loads[0]} → {old_loads[-1]}）\n")
        results["old_det"] = (old_red, old_loads)

        # ② 新形态（当前工作树）：必须零红
        print("② 新形态（`/bin/sh -c \"…; exec sleep 60\"`，整条一个前台作业）")
        write_fresh(TEST_SRC, original)
        if NEW not in open(TEST_SRC, encoding="utf-8").read():
            print("⚠ 恢复校验失败：请手工从 git 恢复 " + TEST_SRC)
            return 1
        new_red, new_loads, err = run_form("新", rounds)
        if err is not None:
            print(err[-2500:])
            return 1
        print(f"   → 红 {new_red}/{rounds}（负载 {new_loads[0]} → {new_loads[-1]}）\n")
        results["new"] = (new_red, new_loads)

        # ③ 可选：裸旧写法对照（依赖负载，不作判据）
        if "--raw" in flags:
            print("③ 裸旧写法（`echo …; sleep 60`，1–2ms 窗口；仅记录，不作判据）")
            write_fresh(TEST_SRC, original.replace(NEW, OLD_RAW, 1))
            raw_red, raw_loads, err = run_form("裸", rounds)
            if err is not None:
                print(err[-2500:])
                return 1
            print(f"   → 红 {raw_red}/{rounds}（负载 {raw_loads[0]} → {raw_loads[-1]}）\n")
            results["old_raw"] = (raw_red, raw_loads)
    finally:
        write_fresh(TEST_SRC, original)
        ok = NEW in open(TEST_SRC, encoding="utf-8").read()
        print(f"源码已恢复新写法：{ok}")
        if not ok:
            print("⚠ 恢复失败：请从 git 恢复 " + TEST_SRC)
        if burn is not None:
            burn.__exit__()

    old_red, new_red = results["old_det"][0], results["new"][0]
    print()
    if new_red > 0:
        print(f"✗ 新写法仍然红 {new_red} 轮 —— 修复不成立")
        return 1
    if old_red == 0:
        print(f"✗ 旧形态在 {rounds} 轮里零红 —— 本环境复现不出，等于没有判别力（不要当结论）")
        return 1
    print(f"✓ 逆向验证通过：旧形态 {old_red}/{rounds} 红 · 新形态 0/{rounds} 红")
    return 0


if __name__ == "__main__":
    sys.exit(main())
