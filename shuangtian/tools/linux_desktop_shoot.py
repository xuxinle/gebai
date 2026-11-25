#!/usr/bin/env python3
"""Linux 真实桌面截图取证：把应用**真的开在桌面会话里**，再从屏幕抓像素。

## 为什么要这个脚本（而不是控制通道的 `capture`）

`capture` 拿的是**进程内帧缓冲**——它证明"我画对了"，但不证明"这些像素到了屏幕上"。
两者之间还有整条链路：X11 MIT-SHM 上屏 / Wayland 提交 / 窗口管理器合成 / 显示缩放。
这条链路上任何一环坏掉，`capture` 依然全绿（历史教训：`present` 漏提交 → 窗口全黑，
而应用内截图正常）。所以**最终判据只能是桌面截图**。

## 与无头截图的区别

| | 无头 `--shots` | 本脚本 |
|---|---|---|
| 渲染路径 | 软件光栅离屏 | 与用户看到的**同一条** |
| 窗口管理器 | 无 | 真实（去装饰/最大化/Z 序全生效）|
| DPI | 自报 | 系统真实 |
| 能发现 | 绘制缺陷 | 绘制 + 上屏 + 合成 + 窗口语义缺陷 |

## 用法

    python3 tools/linux_desktop_shoot.py                 # 全部应用
    python3 tools/linux_desktop_shoot.py gallery gbcode  # 指定应用
    python3 tools/linux_desktop_shoot.py --list

退出码 0 = 全部应用都起了且截到了**非空**窗口区域。

## 依赖

`DISPLAY`（或 `WAYLAND_DISPLAY`）+ 一个截图工具。优先 `gnome-screenshot`，
其次 `import`（ImageMagick），最后 `xwd`+PIL。都不在则**如实失败**而不是静默跳过。
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import signal
import struct
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build/dev/bin"
OUT = ROOT / "build/linux-desktop"

# 应用清单：名字 → (等待秒数, 额外参数)。
# 等待时间按"首帧+启动卡退场"实测给；加太多会把总时长拖长，少了会截到启动卡。
APPS: dict[str, tuple[float, list[str]]] = {
    "gallery": (4.0, []),
    "gbcode": (5.0, []),
    "louyue": (5.0, []),
    "caiyun": (6.0, []),
    "jianmu": (5.0, []),
    "xiangqi": (4.0, []),
    "cefu": (4.0, []),
    "gedou": (5.0, []),
    "xiaobai": (5.0, []),
}


def find_screenshot_tool() -> tuple[str, list[str]] | None:
    """挑一个能抓**整屏**的命令。返回 (名字, 参数模板)，`{}` 是输出路径占位。"""
    if shutil.which("gnome-screenshot"):
        return "gnome-screenshot", ["gnome-screenshot", "-f", "{}"]
    if shutil.which("import"):
        return "import", ["import", "-window", "root", "{}"]
    if shutil.which("grim") and os.environ.get("WAYLAND_DISPLAY"):
        return "grim", ["grim", "{}"]
    return None


def shoot(tool: tuple[str, list[str]], path: pathlib.Path) -> bool:
    name, template = tool
    cmd = [path.as_posix() if arg == "{}" else arg for arg in template]
    try:
        done = subprocess.run(cmd, capture_output=True, timeout=25, check=False)
    except subprocess.TimeoutExpired:
        print(f"  截图超时（{name}）")
        return False
    if done.returncode != 0 or not path.exists():
        detail = done.stderr.decode("utf-8", "replace").strip()[:200]
        print(f"  截图失败（{name} rc={done.returncode}）：{detail}")
        return False
    return True


def window_rect_for_pid(pid: int) -> tuple[int, int, int, int] | None:
    """按 `_NET_WM_PID` 找窗口的屏幕矩形（x, y, w, h）。

    为什么不按标题找：标题是 UTF-8 的 `_NET_WM_NAME`，而 `XFetchName` 只给 Latin-1，
    中文标题必然解不出来。`_NET_WM_PID` 是我们自己建窗时写的，唯一且无编码问题。
    """
    try:
        out = subprocess.run(
            ["xwininfo", "-root", "-children"], capture_output=True, timeout=10, check=False
        ).stdout.decode("utf-8", "replace")
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return None
    best: tuple[int, int, int, int] | None = None
    for line in out.splitlines():
        if "0x" not in line or "x" not in line:
            continue
        head = line.strip().split()[0]
        if not head.startswith("0x"):
            continue
        # 形如：0xe00001 "标题": ("cls" "Cls")  1280x720+0+32  +0+32
        parts = line.split()
        for token in parts:
            if "x" in token and "+" in token:
                try:
                    size, x, y = token.split("+")[0], token.split("+")[1], token.split("+")[2]
                    width, height = size.split("x")
                    rect = (int(x), int(y), int(width), int(height))
                except (ValueError, IndexError):
                    continue
                try:
                    prop = subprocess.run(
                        ["xprop", "-id", head, "_NET_WM_PID"],
                        capture_output=True, timeout=10, check=False
                    ).stdout.decode("utf-8", "replace")
                except (subprocess.TimeoutExpired, FileNotFoundError):
                    continue
                if str(pid) in prop:
                    if rect[2] > 100 and rect[3] > 100:
                        best = rect if best is None else best
                break
    return best


def crop(path: pathlib.Path, rect: tuple[int, int, int, int], dest: pathlib.Path) -> bool:
    try:
        from PIL import Image  # noqa: PLC0415
    except ImportError:
        return False
    try:
        with Image.open(path) as image:
            x, y, w, h = rect
            image.crop((x, y, x + w, y + h)).save(dest)
        return True
    except OSError:
        return False


def mean_brightness(path: pathlib.Path) -> float:
    """平均亮度：全黑窗口（上屏断了）会给出接近 0 的值——**这是本脚本的核心断言**。

    为什么不比指纹/不比像素：主题与字体在不同机器上必然不同，比像素只会天天红。
    而“窗口里有没有东西”是跨机器都成立的。

    ## 阈值为何是 20（而不是 6）

    第一版写的 6，结果 **jianmu 全黑（实测 7.14）也被判“OK”**——那个 7.14 全部来自
    盖在窗口上的系统通知卡（白色），而窗口本身是纯黑。也就是说 6 这个阈值
    只能拦住“完全零输入”，拦不住“只画了一部分/只剩背景”。

    20 的依据：本仓所有真实界面实测亮度最低的是 gedou（深色游戏界面，34.6）
    与 gallery（深色主题，32.2），而真实缺陷态是 7.1。20 落在两者之间且
    距离两边的余量都足够大（差 12 以上）。
    """
    try:
        from PIL import Image, ImageStat  # noqa: PLC0415
    except ImportError:
        return -1.0
    try:
        with Image.open(path) as image:
            return float(ImageStat.Stat(image.convert("L")).mean[0])
    except OSError:
        return -1.0


# 窗口内容亮度下限：低于它就算“没上屏/只剩背景”（依据见 `mean_brightness` 注释）。
MIN_WINDOW_MEAN_L = 20.0


def run_one(
    app: str, tool: tuple[str, list[str]], backend: str | None, settle: float
) -> dict:
    exe = BIN / app
    if not exe.exists():
        return {"app": app, "ok": False, "error": "可执行文件不存在（先 st build all）"}
    cmd = [str(exe)]
    if backend:
        cmd += ["--backend", backend]
    # 给足寿命：截完图我们再主动收掉，应用自己不需要知道时长。
    cmd += ["--ms", str(int((settle + 4.0) * 1000))]

    log_path = OUT / f"{app}{'-' + backend if backend else ''}.log"
    with log_path.open("wb") as log:
        proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
    result: dict = {"app": app, "backend": backend or "auto", "pid": proc.pid}
    try:
        time.sleep(settle)
        if proc.poll() is not None:
            text = log_path.read_text("utf-8", "replace")
            result |= {"ok": False, "error": f"进程已退出（rc={proc.returncode}）", "log": text[-1200:]}
            return result
        rect = window_rect_for_pid(proc.pid)
        shot = OUT / f"{app}{'-' + backend if backend else ''}-desktop.png"
        if not shoot(tool, shot):
            return result | {"ok": False, "error": "截图失败"}
        result["shot"] = str(shot)
        brightness = mean_brightness(shot)
        result["desktop_mean_L"] = round(brightness, 2)
        if rect:
            result["window"] = {"x": rect[0], "y": rect[1], "w": rect[2], "h": rect[3]}
            cropped = OUT / f"{app}{'-' + backend if backend else ''}-window.png"
            if crop(shot, rect, cropped):
                result["window_shot"] = str(cropped)
                window_l = round(mean_brightness(cropped), 2)
                result["window_mean_L"] = window_l
                # 判据：窗口内平均亮度 ≥ 20（深色主题实测 32~35，缺陷态 7）。
                result["ok"] = bool(window_l >= MIN_WINDOW_MEAN_L)
                if not result["ok"]:
                    result["error"] = (
                        f"窗口内容亮度 {window_l} < {MIN_WINDOW_MEAN_L}——"
                        "界面没上屏或只画了背景")
                if not sidecar_ok(log_path):
                    result["ok"] = False
                    result["error"] = "日志里有已知致命标志"
        else:
            # 找不到窗口：X11 下这是缺陷（窗口没映射）；Wayland 也应有 toplevel。
            result |= {"ok": False, "error": "未找到该进程的窗口（窗口未映射？）"}
        return result
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()


FATAL_MARKERS = (
    "段错误",
    "Segmentation",
    "terminate called",
    "Assertion",
    "std::bad_alloc",
    "X Error of failed request",
)


def sidecar_ok(log_path: pathlib.Path) -> bool:
    text = log_path.read_text("utf-8", "replace")
    return not any(marker in text for marker in FATAL_MARKERS)


def main() -> int:
    parser = argparse.ArgumentParser(description="Linux 真实桌面截图取证")
    parser.add_argument("apps", nargs="*", help="要跑的应用（默认全部）")
    parser.add_argument("--backend", default=None, help="显式后端（x11 / wayland）；缺省由应用自己探测")
    parser.add_argument("--settle", type=float, default=None, help="覆盖每应用的等待秒数")
    parser.add_argument("--list", action="store_true", help="只列应用清单")
    args = parser.parse_args()

    if args.list:
        for name, (settle, _) in APPS.items():
            print(f"{name}\t{settle}s")
        return 0

    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        print("失败：没有 DISPLAY / WAYLAND_DISPLAY——本脚本要求真实桌面会话。")
        print("（无头验证请用应用自己的 --shots，那是另一条通路。）")
        return 2
    tool = find_screenshot_tool()
    if tool is None:
        print("失败：找不到截图工具（gnome-screenshot / import / grim 一个都没有）。")
        return 2

    OUT.mkdir(parents=True, exist_ok=True)
    names = args.apps or list(APPS)
    unknown = [n for n in names if n not in APPS]
    if unknown:
        print(f"失败：未知应用 {unknown}（用 --list 看清单）")
        return 2

    print(f"截图工具：{tool[0]} · 输出：{OUT}")
    results = []
    for index, app in enumerate(names, 1):
        settle = args.settle if args.settle is not None else APPS[app][0]
        print(f"[{index}/{len(names)}] {app} … 等待 {settle:.1f}s", flush=True)
        record = run_one(app, tool, args.backend, settle)
        results.append(record)
        status = "OK" if record.get("ok") else "FAIL"
        detail = record.get("error", "")
        lum = record.get("window_mean_L")
        lum_text = f" 窗口亮度 {lum}" if lum is not None else ""
        print(f"    {status}{lum_text}{('  ' + detail) if detail else ''}", flush=True)

    report = OUT / "report.json"
    report.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    passed = sum(1 for r in results if r.get("ok"))
    print(f"\n{passed}/{len(results)} 通过 · 报告 {report}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
