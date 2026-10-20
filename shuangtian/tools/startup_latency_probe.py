"""gbcode 启动耗时的分阶段测量——**测的是真窗口，不是 headless**。

## 口径（读数字前必看）

| 启动模式 | 控制通道就绪 |
|---|---|
| headless | **271 ms** |
| **本探针（真窗口）** | **644 ms** |

两者差 **373 ms**，全部是窗口创建（其中 D3D11 建设备 **357.8 ms**）。
两个数都对——错的是**拿本探针的数去当 headless 场景的成本**。需求 headless
数字时用 `tools/startup_cwd_probe.py`。

⚠ 已澄清的两个**不成立**的怀疑（别再重测）：
- 工作区大小（485 MB 的 bin 目录）：**无影响**（266~294 ms）。目录树惰性扫描 + 按层缓存。
- 会话标签数（1/4/8/16）：**无影响**（297~313 ms）。

## 原来量什么

以**窗口出现**为界，量三段：
  ① 窗口创建 + 首次上屏（headless 没有）；
  ② 会话恢复（4 个标签 × 读文件 + 语法高亮）；
  ③ 首帧绘制（含字体加载、字形光栅化）。

做法：以**窗口出现**为界，量三段：
  t0 = 进程启动
  t1 = 控制通道可用（说明已进入事件循环之前/附近）
  t2 = 首帧完成（`paint_ms` 有值且 `frames >= 1`）

用法：
    py -3 tools/startup_latency_probe.py            # 复用已有会话（4 个标签）
    py -3 tools/startup_latency_probe.py --cold     # 临时 HOME，空会话（对照）
    py -3 tools/startup_latency_probe.py --no-window
"""

from __future__ import annotations

import json
import os
import pathlib
import statistics
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
WORK = ROOT / "build" / "startup"
sys.path.insert(0, str(ROOT / "tools"))
import st_client_lib  # noqa: E402

# : 隔离的 ST_HOME，避免读真实用户会话（否则会把“恢复真仓库”算进启动成本）。
STARTUP_HOME = WORK / "probe-home"


def one_run(rounds: int = 3, cold: bool = False) -> dict:
    exe = ROOT / "build" / "dev" / "bin" / "gbcode.exe"
    if not exe.exists():
        print("跳过：先 st build gbcode --profile dev")
        return {}

    results = {"control_ready": [], "first_frame": [], "total": []}
    for index in range(rounds):
        WORK.mkdir(parents=True, exist_ok=True)
        control = WORK / f"control-{index}.json"
        control.unlink(missing_ok=True)
        log = WORK / f"log-{index}.txt"
        log.unlink(missing_ok=True)

        env = dict(os.environ)
        if cold:
            # 空 HOME → 没有 session.json → 不恢复标签（对照组）
            cold_home = WORK / f"home-{index}"
            cold_home.mkdir(parents=True, exist_ok=True)
            env["ST_HOME"] = str(cold_home)
        else:
            # 默认也用隔离 HOME：否则会读真实用户会话，把“恢复真仓库/真 git 状态”
            # 算进启动成本（实测这一条曾把 250 ms 抬成 660 ms 的一部分）。
            STARTUP_HOME.mkdir(parents=True, exist_ok=True)
            env["ST_HOME"] = str(STARTUP_HOME)

        t0 = time.perf_counter()
        with log.open("wb") as err:
            process = subprocess.Popen(
                [str(exe), "--control-port", "0", "--control-file", str(control)],
                cwd=str(ROOT),
                stdout=subprocess.DEVNULL,
                stderr=err,
                env=env,
            )
        try:
            # t1：控制通道就绪（控制文件出现且能握手）
            info = {}
            deadline = time.time() + 90
            while time.time() < deadline:
                if control.exists():
                    try:
                        info = json.loads(control.read_text(encoding="utf-8"))
                        break
                    except (json.JSONDecodeError, OSError):
                        pass
                time.sleep(0.01)
            t1 = time.perf_counter() - t0
            if not info:
                print(f"  第 {index + 1} 轮：控制通道未就绪")
                continue

            # t2：首帧完成（frames >= 1 且 paint_ms > 0）
            port, token = int(info["port"]), info["token"]
            t2 = None
            while time.time() < deadline:
                try:
                    metrics = st_client_lib.call_with_token(port, "metrics", {}, token,
                                                            timeout=10)["result"]
                except Exception:  # noqa: BLE001 - 启动期握手可能失败，重试即可
                    time.sleep(0.01)
                    continue
                if int(metrics.get("frames", 0)) >= 1 and float(metrics.get("paint_ms", 0)) > 0:
                    t2 = time.perf_counter() - t0
                    break
                time.sleep(0.01)

            results["control_ready"].append(t1)
            if t2 is not None:
                results["first_frame"].append(t2)
            results["total"].append(time.perf_counter() - t0)
            label = f"{t2:.0f}ms" if t2 is not None else "n/a"
            print(f"  第 {index + 1} 轮：控制通道 {t1 * 1000:.0f}ms · 首帧 {label}")
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
            time.sleep(0.5)
    return results


def report(name: str, values: list[float]) -> None:
    if not values:
        print(f"{name}: 无数据")
        return
    ordered = sorted(values)
    print(f"{name}: 中位 {statistics.median(ordered) * 1000:.0f}ms "
          f"· 最小 {ordered[0] * 1000:.0f}ms · 最大 {ordered[-1] * 1000:.0f}ms "
          f"（{len(ordered)} 轮）")


def main() -> int:
    cold = "--cold" in sys.argv
    rounds = 3
    print(f"gbcode 启动耗时（{'冷启动/空会话' if cold else '复用已有会话'}）")
    print()
    data = one_run(rounds=rounds, cold=cold)
    if not data:
        return 2
    print()
    report("控制通道就绪", data["control_ready"])
    report("首帧完成", data["first_frame"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
