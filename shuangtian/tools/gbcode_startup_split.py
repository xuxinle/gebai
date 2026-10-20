"""gbcode 启动分段归因：谁占了那 250 ms。

## 背景

上一轮的 `startup_latency_probe.py` 报 660 ms，但那是**测量设置造成的**：
它用 `cwd=exe.parent`（`build/dev/bin`，485 MB），而 gbcode 无 `--workspace` 时
拿当前目录当工作区。改正后真实值是 **~250 ms**（见 `startup_cwd_probe.py`）。

## 这个探针做什么

用**应用自己的日志时间戳**（而非外部轮询）切段，避免把“外部观测延迟”算进启动成本：

| 段 | 含义 |
|---|---|
| A 进程启动 → 日志落盘 | Windows 加载 8.5 MB 映像 + 全局构造 + 日志系统初始化 |
| B 日志落盘 → 会话恢复完成 | 读 session.json + 打开标签 |
| C 会话恢复 → DSL 首帧构建完成 | 建窗口 + 构建整棵组件树 |
| D DSL → 字体加载完成 | 读 6 个 face + 3 个等宽 face（TTC/TTF 解析） |
| E 字体 → 控制通道就绪 | 建监听 + 握手 |

**为什么要分开**：只有先知道“哪一段最大”，才谈得上该不该优化。
之前我直接猜“会话恢复是瓶颈”，实测 1/4/8 个标签全是 ~300 ms——猜错了。

用法：
    py -3 tools/gbcode_startup_split.py
    py -3 tools/gbcode_startup_split.py --rounds 5
"""

from __future__ import annotations

import argparse
import pathlib
import re
import statistics
import subprocess
import sys
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "dev" / "bin" / "gbcode.exe"

# 日志行里带 ISO 时间戳；用它算段间耗时，避开外部轮询的抖动。
STAMP = re.compile(r"\[(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3})Z\]")


def parse_stamp(line: str) -> float | None:
    found = STAMP.search(line)
    if not found:
        return None
    text = found.group(1)
    # 只取时间部分（同一天内足够），转成秒。
    clock = text.split("T")[1]
    hours, minutes, rest = clock.split(":")
    seconds = float(rest)
    return int(hours) * 3600.0 + int(minutes) * 60.0 + seconds


def one_run() -> dict[str, float]:
    started = time.perf_counter()
    process = subprocess.Popen(
        [str(EXE), "--headless", "--control-port", "0"],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    lines: list[tuple[float, str]] = []

    def reader() -> None:
        assert process.stdout is not None
        for line in process.stdout:
            lines.append(((time.perf_counter() - started) * 1000.0, line.rstrip()))

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()
    time.sleep(1.5)
    process.kill()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        pass

    marks: dict[str, float] = {}
    for elapsed_ms, line in lines:
        if "日志落盘" in line and "log_init" not in marks:
            marks["log_init"] = elapsed_ms
        if "已恢复上次会话" in line and "session" not in marks:
            marks["session"] = elapsed_ms
        if "[dsl]" in line and "dsl" not in marks:
            marks["dsl"] = elapsed_ms
        if "字体已加载" in line and "fonts" not in marks:
            marks["fonts"] = elapsed_ms
        if "控制通道已启动" in line and "control" not in marks:
            marks["control"] = elapsed_ms
        if "listening control on" in line and "control" not in marks:
            marks["control"] = elapsed_ms
    return marks


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rounds", type=int, default=3)
    args = parser.parse_args()

    if not EXE.exists():
        print(f"跳过：先 st build gbcode --profile dev（找不到 {EXE}）")
        return 0

    keys = ["log_init", "session", "dsl", "fonts", "control"]
    labels = {
        "log_init": "A 进程启动 → 日志落盘",
        "session": "B → 会话恢复完成",
        "dsl": "C → DSL 首帧构建完成",
        "fonts": "D → 字体加载完成",
        "control": "E → 控制通道就绪",
    }
    samples: dict[str, list[float]] = {key: [] for key in keys}
    for _ in range(args.rounds):
        marks = one_run()
        for key in keys:
            if key in marks:
                samples[key].append(marks[key])

    print(f"gbcode 启动分段（{args.rounds} 轮，取中位；绝对时刻，单位 ms）\n")
    print(f"{'段':<26}{'到达时刻':>12}{'本段耗时':>12}")
    previous = 0.0
    for key in keys:
        if not samples[key]:
            print(f"{labels[key]:<26}{'n/a':>12}{'':>12}")
            continue
        at = statistics.median(samples[key])
        print(f"{labels[key]:<26}{at:>10.0f} ms{at - previous:>10.0f} ms")
        previous = at
    print("\n注：A 段是 Windows 加载 8.5 MB 静态映像 + 全局构造，应用侧动不了；")
    print("    其余段才是 gbcode 自己的成本。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
