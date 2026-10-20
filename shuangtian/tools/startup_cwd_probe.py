"""gbcode 启动耗时与**工作区大小**的关系（结论：无关）。

## 为何要量这个

曾怀疑“工作区大 → 启动慢”：探针从 `build/dev/bin`（485 MB / 27 文件）
启动时报 660 ms。而 gbcode 无 `--workspace` 时拿当前目录当工作区（`code .` 语义）。

**实测否定了这个怀疑**：仓库根 / 小项目 / 485 MB 的 bin，三者都是 **266~294 ms**。
原因是目录树**惰性扫描 + 按层缓存**，启动期只列第一层。
那 660 ms 的真因是**探针没传 `--headless`**（真窗口 = headless + 窗口创建 373 ms）。

## 这个探针量什么

以「控制通道就绪」与「首帧完成」为两段，在三种工作目录下对照，用来**对账**：
只要三者接近，就说明工作区大小不构成启动成本。

用法：
    py -3 tools/startup_cwd_probe.py
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

EXE = ROOT / "build" / "dev" / "bin" / "gbcode.exe"


def one_run(cwd: str, tag: str) -> tuple[float, float | None]:
    control = WORK / f"cwd-{tag}.json"
    control.unlink(missing_ok=True)
    started = time.perf_counter()
    process = subprocess.Popen(
        [str(EXE), "--headless", "--control-port", "0", "--control-file", str(control)],
        cwd=cwd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env={**os.environ, "ST_HOME": str(WORK / "home")},
    )
    info: dict = {}
    while time.perf_counter() - started < 30:
        if control.exists():
            try:
                info = json.loads(control.read_text(encoding="utf-8"))
                break
            except (json.JSONDecodeError, OSError):
                pass
        time.sleep(0.005)
    control_ms = (time.perf_counter() - started) * 1000

    frame_ms: float | None = None
    if info:
        port, token = int(info["port"]), info["token"]
        while time.perf_counter() - started < 30:
            try:
                metrics = st_client_lib.call_with_token(port, "metrics", {}, token, timeout=10)[
                    "result"
                ]
            except Exception:  # noqa: BLE001 - 启动期握手可能失败，重试即可
                time.sleep(0.01)
                continue
            if int(metrics.get("frames", 0)) >= 1 and float(metrics.get("paint_ms", 0)) > 0:
                frame_ms = (time.perf_counter() - started) * 1000
                break
            time.sleep(0.01)

    process.kill()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        pass
    return control_ms, frame_ms


def main() -> int:
    WORK.mkdir(parents=True, exist_ok=True)
    if not EXE.exists():
        print(f"跳过：先 st build gbcode --profile dev（找不到 {EXE}）")
        return 0

    scenarios = [
        (str(ROOT), "仓库根"),
        (str(ROOT / "examples" / "gbcode"), "小项目目录"),
        (str(ROOT / "build" / "dev" / "bin"), "bin 目录（485MB，对账用）"),
    ]
    print(f"{'工作目录':<24}{'控制通道中位':>14}{'首帧中位':>12}")
    for cwd, label in scenarios:
        if not pathlib.Path(cwd).is_dir():
            print(f"{label:<24}  （不存在）")
            continue
        runs = [one_run(cwd, f"{pathlib.Path(cwd).name}-{index}") for index in range(3)]
        controls = [item[0] for item in runs]
        frames = [item[1] for item in runs if item[1] is not None]
        frame_text = f"{statistics.median(frames):.0f} ms" if frames else "n/a"
        print(f"{label:<24}{statistics.median(controls):>12.0f} ms{frame_text:>14}")

    print("\n结论：三者接近 → **工作区大小不影响启动**（目录树惰性扫描 + 按层缓存）。")
    print("    读 660 ms 那类数字时，先确认探针有没有传 `--headless`：")
    print("    headless 271 ms vs 真窗口 644 ms，差的 373 ms 全是窗口创建（D3D11 设备 357.8 ms）。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
