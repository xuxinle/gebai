"""gbcode **真实窗口**启动耗时的分阶段测量。

为什么要专门量：headless 启动日志看只有 ~313ms（从"日志落盘"到"控制通道已启动"），
但用户体感是「卡顿 2 秒」。两者差的正是：
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

        t0 = time.perf_counter()
        with log.open("wb") as err:
            process = subprocess.Popen(
                [str(exe), "--control-port", "0", "--control-file", str(control)],
                cwd=str(exe.parent),
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
