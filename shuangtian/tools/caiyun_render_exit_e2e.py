"""`caiyun --render` 命令行收尾的回归：**进程必须自己退出**。

## 这条用例防的是什么

2026-10-26 实测：mp4 已完整写出（ffprobe 读得出全部 10 帧 / 0.33s），
进程却永不退出——CPU 累计只有 0.6~3.7s，所有线程停在 `Wait:UserRequest`。

根因（已修）：`Renderer::mutex_` 是**非递归**锁，而有三处在持有它时调用了
会再次加锁的函数——`log_line()` 自己加同一把锁，`on_update` 回调里会
读 `status()`（也加同一把锁）。自死锁发生在**收尾**，所以产物已经写好，
只有"退出"这一步永远走不到。

从外面看，这完全是"渲染很慢"：每次 `--render` 都白等数分钟。
真值 10 帧只要约 1.0s、39.2 秒的全片只要 22s。

## 为什么断言"墙钟上限"而不是只看产物存在

产物存在恰恰是死锁时的状态（写完 mp4 才卡住），拿它当判据会**恒绿**。
必须有**时间上界**：修好之前，同一命令远远超过这个上限。

## 用法

    py tools/caiyun_render_exit_e2e.py [caiyun.exe]

默认找 `build/dev/bin/caiyun.exe`。
"""
import json
import os
import subprocess
import sys
import tempfile
import time

sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BIN = os.path.join(ROOT, "build", "dev", "bin", "caiyun.exe")

#: 10 帧的最小脚本，正常应在 ~1s 完成；给 60s 上界吸收冷启动与杀毒扫描
EXIT_DEADLINE_S = 60.0
MINI_FRAMES = 10
MINI_DURATION = MINI_FRAMES / 30.0


def check(condition, message):
    if not condition:
        raise SystemExit("检查失败：" + message)
    print("  ok  " + message)


def mini_script():
    """最小脚本：一帧不变色块 + 关音频（避免配乐阶段干扰本用例的判据）。"""
    return {
        "name": "exit-probe", "fps": 30, "size": [640, 360],
        "transition": 0.0, "fade_in": 0.0, "fade_out": 0.0,
        "audio": {"enabled": False},
        "scenes": [{
            "kind": "custom", "duration": MINI_DURATION,
            "background": ["#080B12", "#0E1521"], "accent": "#4B8CF5",
            "wrap": False,
            "layers": [{"kind": "rect", "x": 40, "y": 40, "w": 200, "h": 120,
                        "fill": "#1B2A3E", "enter": 0}],
        }],
    }


def run_render(binary, work, tag, extra=None):
    """渲一次，返回 (退出码, 墙钟秒, mp4 路径)。用 PIPE 读尽输出，避免写满阻塞。"""
    script = os.path.join(work, tag + ".json")
    with open(script, "w", encoding="utf-8") as f:
        json.dump(mini_script(), f, ensure_ascii=False)
    out = os.path.join(work, tag + ".mp4")
    args = [binary, "--headless", "--script", script, "--render", out]
    if extra:
        args += list(extra)
    started = time.time()
    # 输出必须读尽：caiyun 每帧打日志，不读会把子进程堵在写管道上，
    # 于是"日志太多"会被误诊成"渲染挂死"。
    completed = subprocess.run(args, capture_output=True, timeout=EXIT_DEADLINE_S)
    return completed.returncode, time.time() - started, out


def main() -> int:
    binary = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    check(os.path.exists(binary), "找到裁云可执行文件：" + binary)

    # 残留实例会占着 exe 导致构建失败，也会让本用例的墙钟失真
    if sys.platform == "win32":
        subprocess.run(["taskkill", "/F", "/IM", "caiyun.exe"],
                       capture_output=True)

    with tempfile.TemporaryDirectory(prefix="caiyun-exit-") as work:
        for tag, extra in (("pipe", None), ("keepframes", ["--keep-frames"])):
            label = "管道直送 ffmpeg" if extra is None else "--keep-frames（PNG 路径）"
            code, seconds, out = run_render(binary, work, tag, extra)
            check(code == 0, f"{label}：退出码为 0（实际 {code}）")
            check(seconds < EXIT_DEADLINE_S,
                  f"{label}：{seconds:.2f}s 内自行退出（上界 {EXIT_DEADLINE_S:.0f}s）")
            check(os.path.exists(out) and os.path.getsize(out) > 0,
                  f"{label}：产出非空 mp4")

    print("\n全部通过：命令行渲染在收尾阶段不会自死锁。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
