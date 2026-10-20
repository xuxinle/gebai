#!/usr/bin/env python3
"""窗口**真的上屏了没有**——软件呈现路径的回归探针（Windows）。

为什么需要它：2026-10-20 的缺陷里，界面**内部画布完全正常**，窗口可见、命中测试正常、
按钮点击有回调，`metrics` 里 `present_ms` 有值、`frames` 在涨——但窗口一个像素都没上屏
（窗口 DC 里全是 `0x00000000`，屏幕上看到的是它下面的其它窗口）。

根因（`src/shell/platform_win32.cpp`）：
  `resize_buffers_in_place()`（拖动/DPI 变化/启动自适应走的快路径）会 `release_dib()`
  把 DIB 惰性丢掉，而 `present()` 那时写着 `if (dib_ == nullptr) return;` ——
  早退让 `blit()`（它自己第一步就是 `ensure_dib()`，重建责任本在它身上）永不执行，
  于是 DIB 永不重建，窗口从此一片空白。

为何**必须**在真实窗口上测、且**必须**强制 `--renderer software`：
  - 无头后端不“上屏”，这条路径根本不存在（单元测试碰不到）；
  - GPU 路径走 `presenter_->present()` 直接 `return`，**到不了**那段代码。
  也就是说：**渲染器 autu 选型的机器差异决定了缺陷是否发作**——本机 GPU 是
  NVIDIA L20-6Q 虚拟化切分卡，`auto` 实测选了软件路径，于是必现；
  别的机器选 GPU 就一切正常。不强制指定渲染器，这个探针在某些机器上会**恒绿**。

判据（三条同时成立才算通过）：
  ① `metrics.renderer == "software"`（确认真的在测目标路径）；
  ② 窗口 DC 的像素**不是全 0 / 不是全同色**（真的画了东西）；
  ③ 窗口 DC 与屏幕同位置像素**一致**（像素真的送到了屏幕，而不是只写进了离屏缓冲）。

用法（Windows）：
    python tools/present_software_verify.py                 # 用 build/dev/bin/gbcode.exe
    python tools/present_software_verify.py --exe PATH      # 指定应用
    python tools/present_software_verify.py --keep-shot DIR # 保留截图

退出码：0 = 通过；1 = 失败（附诊断）；2 = 环境不具备（非 Windows / 应用不存在）。
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wintypes
import json
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_EXE = ROOT / "build/dev/bin/gbcode.exe"
WORK_DIR = ROOT / "build/present-verify"


# ── Win32 绑定（只读：取窗口 DC / 屏幕像素）────────────────────────────────────
user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
user32.GetClientRect.restype = wintypes.BOOL
user32.GetDC.argtypes = [wintypes.HWND]
user32.GetDC.restype = wintypes.HDC
user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.IsWindowVisible.restype = wintypes.BOOL
# ⚠ `HWND` 是**指针宽度**：不声明 argtypes 时 ctypes 按 `c_int` 传参，64 位下句柄高位
# 被截断，`EnumWindows` 回调里拿到的 `hwnd` 与后续 `GetDC/GetPixel` 用的不是同一个值
# （实测：窗口明明在，却一直“等不到主窗口”）。
ENUM_PROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.EnumWindows.argtypes = [ENUM_PROC, wintypes.LPARAM]
user32.EnumWindows.restype = wintypes.BOOL
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetWindowThreadProcessId.restype = wintypes.DWORD
gdi32.GetPixel.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
gdi32.GetPixel.restype = wintypes.COLORREF


def log(message: str) -> None:
    # Windows 控制台常是 GBK：直接 print `✓/✗` 会 UnicodeEncodeError 把探针自己搞崩
    # （实测被这个掩盖过一次真结果）。统一用 ASCII 标记。
    print(message.encode("ascii", "replace").decode("ascii"), flush=True)


def find_main_window(pid: int, timeout: float = 60.0) -> int | None:
    """等进程出现主窗口，返回 HWND（0 = 没等到）。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        handles = (wintypes.HWND * 1)()
        # MainWindowHandle 由 .NET/psutil 拿不到这里，改用 EnumWindows 太啰嗦：
        # 直接问 Win32 —— 遍历该 pid 的顶层窗口里第一个可见的。
        found: list[int] = []

        def enum_proc(hwnd, _lparam):
            owner = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value == pid and user32.IsWindowVisible(hwnd):
                found.append(hwnd)
            return True

        user32.EnumWindows(ENUM_PROC(enum_proc), 0)
        if found:
            return found[0]
        time.sleep(0.5)
    return None


def sample_pixels(hwnd: int, points: list[tuple[int, int]]) -> list[tuple[int, int, int]]:
    """取窗口 DC 与屏幕 DC 在同一点的像素（各返回 0xRRGGBB）。"""
    window_dc = user32.GetDC(hwnd)
    screen_dc = user32.GetDC(None)
    results: list[tuple[int, int, int]] = []
    try:
        for x, y in points:
            in_window = gdi32.GetPixel(window_dc, x, y)
            on_screen = gdi32.GetPixel(screen_dc, x, y)
            results.append((in_window, on_screen, (in_window == on_screen)))
    finally:
        user32.ReleaseDC(hwnd, window_dc)
        user32.ReleaseDC(None, screen_dc)
    return results


def channel_delta(a: int, b: int) -> int:
    """两个 0xRRGGBB 在 RGB 三通道上的**最大**差值（0~255）。"""
    return max(abs(((a >> shift) & 0xFF) - ((b >> shift) & 0xFF)) for shift in (0, 8, 16))


# 「上屏了」与「没上屏」的差值量级相差一个数量级，用容差把两者分开：
#
# - 半透明像素的**正常**差异 4~8：`GetDC(hwnd)` 读到的是画布里的**预乘 alpha** RGB，
#   而屏幕上是 DWM 把它与桌面合成后的值。实测：标签条半透明区稳定差 4~5（不是噪声，
#   也不是缺陷——但逐位相等的判据会把它误判成“没上屏”）。
# - 真缺陷的差异 22~32：窗口 DC 是 `0x00000000`，屏幕是下层窗口的深灰（`0x00191717`）。
#
# 取 12：把合成差异全部容下，仍留 10 以上的余量挡真缺陷。
PIXEL_TOLERANCE = 12


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=str(DEFAULT_EXE))
    parser.add_argument("--keep-shot", default=None, help="保留窗口截图到该目录")
    parser.add_argument("--timeout", type=float, default=90.0, help="等控制通道就绪的秒数")
    args = parser.parse_args()

    if sys.platform != "win32":
        log("跳过：本探针需要在 Windows 上跑（窗口呈现路径是 Win32 专属）。")
        return 2

    exe = pathlib.Path(args.exe)
    if not exe.exists():
        log(f"跳过：可执行文件不存在 {exe}（先 `st build gbcode --profile dev`）。")
        return 2

    WORK_DIR.mkdir(parents=True, exist_ok=True)
    control_file = WORK_DIR / "control.json"
    control_file.unlink(missing_ok=True)

    # `--renderer software`：**必须显式指定**，否则 `auto` 在 GPU 更快的机器上会走
    # swapchain 路径、绕过被测代码 —— 探针会恒绿（见文件头说明）。
    command = [
        str(exe),
        "--renderer", "software",
        "--control-port", "0",
        "--control-file", str(control_file),
    ]
    log("启动：" + " ".join(command))
    process = subprocess.Popen(
        command,
        cwd=str(exe.parent),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    failures: list[str] = []
    try:
        hwnd = find_main_window(process.pid, args.timeout)
        if hwnd is None:
            log(f"FAIL: no main window (pid={process.pid}, {args.timeout}s)")
            return 1
        log(f"window handle {hwnd}")

        # 等控制通道写出端口（应用已经进主循环 = 已经 present 过多帧）
        deadline = time.time() + args.timeout
        info: dict = {}
        while time.time() < deadline:
            if control_file.exists():
                try:
                    info = json.loads(control_file.read_text(encoding="utf-8"))
                    break
                except (json.JSONDecodeError, OSError):
                    pass
            time.sleep(0.3)
        if not info:
            log("FAIL: control channel not ready (control file missing)")
            return 1

        # 给窗口足够时间走完“自适应改尺寸 → 快路径丢 DIB → 后续 present”这条路
        time.sleep(2.5)

        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
        import st_client_lib  # noqa: PLC0415  （同目录工具库，延迟导入避免影响 --help）

        metrics_reply = st_client_lib.call_with_token(
            int(info["port"]), "metrics", {}, info.get("token", ""), timeout=20
        )
        # 返回的是**完整协议帧** `{id, result}`，指标在 `result` 下（直接当 dict 取字段会全部
        # 得到 None——实测踩过：像素全绿、却因为读不到 renderer 而根根判失败）。
        metrics = metrics_reply.get("result", metrics_reply)
        renderer = str(metrics.get("renderer", "?"))
        log(f"renderer={renderer} · physical={metrics.get('physical_width')}x"
            f"{metrics.get('physical_height')} · frames={metrics.get('frames')} · "
            f"present_skipped={metrics.get('present_skipped')}")

        # ① 确实在测软件路径
        if renderer != "software":
            failures.append(f"渲染器不是 software（是 {renderer}）——本探针没测到目标路径")

        # 新增观测项：调了 present 但没上屏的帧数必须为 0
        skipped = int(metrics.get("present_skipped") or 0)
        if skipped > 0:
            failures.append(f"present_skipped = {skipped}（有帧本该上屏却没上屏）")

        # ②③ 窗口 DC 真的画了东西，且与屏幕一致
        rect = wintypes.RECT()
        user32.GetClientRect(hwnd, ctypes.byref(rect))
        width, height = rect.right, rect.bottom
        log(f"客户区 {width}x{height}")
        if width <= 0 or height <= 0:
            failures.append("客户区尺寸为 0（窗口没成形）")
        else:
            # 采样点只落在**静态底色**区域：避开编辑区中心（那里有闪烁的光标与当前行高亮）。
            # 实测踩过：中心点抽到 `0x00D5EFFD`（光标/选中色）而屏幕是 `0xFFFFFF`——
            # `GetDC` 读的是画布**当前**内容，屏幕是**上一次合成**的结果，两者差一个闪烁相位，
            # 与“没上屏”毫无关系。
            points = [
                (width // 20, height // 2),        # 侧栏
                (width // 3, height // 20),        # 标签条下的空白
                (width * 3 // 4, height * 3 // 4), # 右下部
                (width // 2, height // 8),         # 上部
                (width // 8, height * 7 // 8),     # 左下
                (width * 9 // 10, height // 10),   # 右上角
            ]
            # **多次采样 + 持续不一致才判失败**：闪烁光标、悬停高亮、动画都会让单次采样
            # 出现相位差。真缺陷（没上屏）是**持续**的，不会自己好。
            attempts = 3
            mismatched_all: list[tuple[int, int]] | None = None
            samples: list[tuple[int, int, bool]] = []
            for attempt in range(attempts):
                samples = sample_pixels(hwnd, points)
                current = [
                    (x, y)
                    for (x, y), (in_window, on_screen, _same) in zip(points, samples)
                    if channel_delta(in_window, on_screen) > PIXEL_TOLERANCE
                ]
                if mismatched_all is None:
                    mismatched_all = current
                else:
                    # 取交集：只有前几次都不同的点才算“持续不一致”
                    mismatched_all = [pt for pt in mismatched_all if pt in current]
                if not mismatched_all:
                    break
                if attempt + 1 < attempts:
                    log(f"  采样第 {attempt + 1} 轮有 {len(current)} 点瞬时不一致，重试…")
                    time.sleep(0.25)
            for (x, y), (in_window, on_screen, same) in zip(points, samples):
                delta = channel_delta(in_window, on_screen)
                log(f"  ({x},{y}) window=0x{in_window:08X} screen=0x{on_screen:08X} "
                    f"delta={delta} {'ok' if delta <= PIXEL_TOLERANCE else 'OVER'}")

            window_pixels = {value for value, _screen, _same in samples}
            # 全 0 = DIB 从没被写过（本次缺陷的典型形态：GetDC 拿到的是一块没内容的表面）
            if all(value == 0 for value in window_pixels):
                failures.append("窗口 DC 全是 0x00000000（一个像素都没上屏）——正是本探针要抓的缺陷")
            elif len(window_pixels) == 1:
                failures.append(f"窗口 DC 只有一个颜色 0x{next(iter(window_pixels)):08X}"
                                "（像是整块没画内容）")

            # 用**多轮交集**：只有持续不一致才算“像素没送到屏幕”（见上面的闪烁相位说明）。
            if mismatched_all:
                failures.append(
                    f"{len(mismatched_all)}/{len(points)} 个采样点窗口与屏幕像素**持续**不同 "
                    f"（{attempts} 轮重试均不一致，像素没送到屏幕）：{mismatched_all[:3]}"
                )

        if args.keep_shot:
            shot_dir = pathlib.Path(args.keep_shot)
            shot_dir.mkdir(parents=True, exist_ok=True)
            subprocess.run(
                ["powershell", "-ExecutionPolicy", "Bypass", "-File",
                 str(ROOT / "tools" / "grab_window.ps1"),
                 "-ProcessId", str(process.pid),
                 "-Out", str((shot_dir / "present-verify.png").resolve())],
                check=False,
            )
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()

    if failures:
        log("")
        log("FAIL:")
        for item in failures:
            log("  - " + item)
        return 1
    log("")
    log("PASS: 窗口像素真的上屏（软件路径，窗口 DC 与屏幕一致）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
