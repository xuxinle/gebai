#!/usr/bin/env python3
"""量「标记出现」与「前台进程组易主」的先后关系——即 `Ctrl+C` 被吞的窗口有多宽。

被测用例 `terminal_pty_stop_interrupts_without_killing_the_shell` 的前置条件是
「长命令真的开始执行了」（屏幕上出现标记）。但标记出现 ≠ 目标命令已接管终端：
标记若是**前一条**命令打出的，此后 shell 还要 fork → exec → `tcsetpgrp` 把终端交给
下一条命令，这段间隔里发来的 `0x03` 由 shell 收下并丢弃（它在等前台作业时不处理 SIGINT），
而目标命令随后照常启动 ⇒ 用例红。

本脚本逐轮采样两个时刻：
  t_marker —— 标记字节从 pty 读到的时刻
  t_fg     —— 前台进程组从 shell 变成子作业的时刻（`tcgetpgrp` 与 shell pid 不同）

输出 `t_fg - t_marker`：**正值 = 标记先出现、交接在后（窗口存在）**，负值 = 先交接后标记。

用法: python3 tools/pty_timeline.py [每形态轮数]
"""

from __future__ import annotations

import os
import pty
import select
import signal
import sys
import time

PROMPT = "st-test#"
MARK = "running-9271"

ARMS = {
    # 用例修前写法：标记由 bash 内建 echo 打出，目标是后一条命令
    "A(修前)": "echo 'ru''nning-9271'; sleep 60",
    # 用例现行写法：整条一个前台作业，sh 打标记后 exec 成 sleep
    "B(现行)": "/bin/sh -c \"echo 'ru''nning-9271'; exec sleep 60\"",
}


class Shell:
    """最小交互 shell（固定提示符，与用例的 `test_shell()` 同口径）。"""

    def __init__(self) -> None:
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            env = dict(os.environ, PS1=f"{PROMPT} ", TERM="xterm")
            os.execvpe("/bin/bash", ["bash", "--noprofile", "--norc", "-i"], env)
        self.buf = ""

    def pump(self, seconds: float) -> None:
        end = time.time() + seconds
        while True:
            left = end - time.time()
            if left <= 0:
                return
            ready, _, _ = select.select([self.fd], [], [], left)
            if not ready:
                return
            try:
                data = os.read(self.fd, 65536)
            except OSError:
                return
            if not data:
                return
            self.buf += data.decode("utf-8", "replace")

    def wait_for(self, needle: str, timeout: float = 5.0) -> bool:
        end = time.time() + timeout
        while time.time() < end:
            if needle in self.buf:
                return True
            self.pump(0.002)
        return False

    def close(self) -> None:
        try:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        except OSError:
            pass
        try:
            os.close(self.fd)
        except OSError:
            pass


def measure(cmd: str) -> float | None:
    """返回 `t_fg - t_marker`（微秒）；未测到返回 None。"""
    shell = Shell()
    try:
        if not shell.wait_for(f"{PROMPT} "):
            return None
        shell.buf = ""
        os.write(shell.fd, (cmd + "\r").encode())
        t_marker = None
        t_fg = None
        end = time.time() + 3.0
        while time.time() < end:
            ready, _, _ = select.select([shell.fd], [], [], 0.0005)
            if ready:
                try:
                    data = os.read(shell.fd, 65536)
                except OSError:
                    break
                if not data:
                    break
                chunk = data.decode("utf-8", "replace")
                shell.buf += chunk
                if t_marker is None and MARK in chunk:
                    t_marker = time.perf_counter()
            try:
                fg = os.tcgetpgrp(shell.fd)
            except OSError:
                continue
            if t_fg is None and fg != shell.pid:
                t_fg = time.perf_counter()
            if t_marker is not None and t_fg is not None:
                break
        if t_marker is None or t_fg is None:
            return None
        return round((t_fg - t_marker) * 1e6, 1)
    finally:
        shell.close()


def main() -> int:
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    with open("/proc/loadavg", encoding="utf-8") as handle:
        print(f"起始负载 {handle.read().split()[0]} · 每形态 {rounds} 轮")
    for name, cmd in ARMS.items():
        values = [v for v in (measure(cmd) for _ in range(rounds)) if v is not None]
        values.sort()
        if values:
            print(f"  {name:9s} t_fg - t_marker (us): min={values[0]} "
                  f"p50={values[len(values) // 2]} max={values[-1]} n={len(values)}")
        else:
            print(f"  {name:9s} 未测到")
    with open("/proc/loadavg", encoding="utf-8") as handle:
        print(f"末负载 {handle.read().split()[0]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
