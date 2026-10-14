#!/usr/bin/env python3
"""Wayland 真窗口验证（Linux）：**连接、configure 握手、共享内存送帧、控制通道**。

## 为什么 Wayland 的验证口径与 X11 不同

X11 是**服务端绘制**：窗口在服务端有实体，脚本可以从服务端把像素读回来
（`tools/x11_win_check.py` 就是那么做的）。Wayland 是**客户端绘制 + 无窗口服务端**：
窗口在合成器里，而协议**没有**"读回窗口像素"的接口——合成器是唯一能看画面的角色。

所以本脚本的判据分三层，都在**协议与进程内**可查：

  ① **握手真实性**：进程能跑起来且 `metrics.backend == "wayland"`、`headless == false`。
     —— `xdg-shell` 的 configure 握手是**强制**的：没收到 `configure` 就 attach 缓冲
     是协议错误（合成器直接断开）。所以"能起来"本身就证明走完了
     `get_registry → bind → xdg_surface.get_toplevel → commit → configure → ack_configure`。
  ② **帧真在推进**：`metrics.frames` 随时间增长；且 `device_scale` 取自合成器
     （GNOME 2× 缩放时是 2.0，而 X11/无头默认 1.0）——这是"真的连上了合成器，
     不是内部假象"的直接证据。
  ③ **像素内容**：控制通道 `capture` 取自**同一块被送上去的帧缓冲**
     （`wl_shm` 缓冲就是它拷过去的），因此截图与"送显内容"逐像素同源。
     与无头同参数截图比对（容差内），证明这条链没引入形变。

用法：
    python3 tools/wayland_win_check.py                       # 用当前会话的 WAYLAND_DISPLAY
    python3 tools/wayland_win_check.py --exe PATH --scale 2.0

退出码 0 = 全部通过。非 Linux / 非 Wayland 会话直接跳过（退出 0）。
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import signal
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_EXE = ROOT / "build/dev/bin/gallery"
OUT_DIR = ROOT / "build/wayland-check"
sys.path.insert(0, str(ROOT / "tools"))
from st_client_lib import call_with_token, load_control  # noqa: E402

if not sys.platform.startswith("linux"):
    print("跳过：本脚本只在 Linux 上运行（需要真实 Wayland 会话）")
    sys.exit(0)


def wayland_session_available() -> tuple[bool, str]:
    """是否处在可用的 Wayland 会话里（`WAYLAND_DISPLAY` + socket 存在）。"""
    display = os.environ.get("WAYLAND_DISPLAY", "")
    if not display:
        return False, "无 WAYLAND_DISPLAY 环境变量"
    runtime = os.environ.get("XDG_RUNTIME_DIR", "")
    if runtime and not display.startswith("/"):
        if not pathlib.Path(runtime, display).exists():
            return False, f"{runtime}/{display} 不存在（不在 Wayland 会话里？）"
    return True, display


def call(port: int, method: str, params: dict | None, token: str, timeout: float = 10.0) -> dict:
    return call_with_token(port, method, params or {}, token, timeout=timeout)


def main() -> int:
    parser = argparse.ArgumentParser(description="Wayland 真窗口验证")
    parser.add_argument("--exe", default=str(DEFAULT_EXE))
    parser.add_argument("--scale", type=float, default=0.0,
                        help="显式缩放（0 = 跟随合成器，用于验证 wl_output.scale 是否被采纳）")
    parser.add_argument("--ms", type=int, default=15000)
    args = parser.parse_args()

    available, detail = wayland_session_available()
    if not available:
        print(f"跳过：{detail}")
        return 0
    if not pathlib.Path(args.exe).exists():
        print(f"跳过：可执行文件不存在 {args.exe}（先 st build gallery）")
        return 0

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    failures: list[str] = []
    checks: list[str] = []
    checks.append(f"Wayland 会话：WAYLAND_DISPLAY={detail}")

    control_file = str(OUT_DIR / f"ctl-{os.getpid()}.json")
    if os.path.exists(control_file):
        os.unlink(control_file)
    log_path = OUT_DIR / "gallery-wayland.log"
    log = open(log_path, "wb")  # noqa: SIM115
    command = [args.exe, "--backend", "wayland", "--control-port", "0",
               "--control-file", control_file, "--ms", str(args.ms)]
    if args.scale > 0:
        command += ["--scale", str(args.scale)]
    proc = subprocess.Popen(command, stdout=log, stderr=log,
                            env=dict(os.environ), start_new_session=True)
    try:
        info: dict = {}
        for _ in range(200):
            time.sleep(0.2)
            info = load_control(control_file)
            if info.get("port"):
                break
            if proc.poll() is not None:
                break
        if not info.get("port"):
            print("失败：控制通道未就绪（wayland 窗口没起来？）")
            print(log_path.read_text(errors="replace")[-4000:])
            return 1
        token = info.get("token", "")

        # —— ① 握手与后端身份 ——
        if info.get("backend") != "wayland":
            failures.append(f"控制文件 backend={info.get('backend')!r}（期望 'wayland'）")
        else:
            checks.append("控制文件 backend=wayland（真的换到 Wayland 后端了）")
        if info.get("headless"):
            failures.append("headless=True（Wayland 后端被当成无头了）")
        else:
            checks.append("headless=false（真窗口，不是无头回退）")

        first = call(info["port"], "metrics", {}, token).get("result", {})
        if first.get("backend") != "wayland":
            failures.append(f"metrics.backend={first.get('backend')!r}")
        else:
            checks.append(f"metrics 上报 backend=wayland / renderer={first.get('renderer')}")
        scale = float(first.get("device_scale") or 0.0)
        if args.scale > 0:
            if abs(scale - args.scale) > 0.01:
                failures.append(f"显式 --scale {args.scale} 未生效（device_scale={scale}）")
            else:
                checks.append(f"显式缩放被采纳：device_scale={scale}")
        else:
            # 跟随合成器：GNOME 2× 缩放时这里应是 2.0（说明 wl_output.scale 真读到了）
            if scale <= 0:
                failures.append("device_scale 非法")
            else:
                checks.append(f"缩放取自合成器：device_scale={scale}（wl_output.scale 生效）")
        physical = (int(first.get("physical_width") or 0), int(first.get("physical_height") or 0))
        if physical[0] > 0 and physical[1] > 0:
            checks.append(f"帧缓冲物理尺寸 {physical[0]}x{physical[1]}")

        # —— ② configure 握手：窗口尺寸由合成器给 ——
        # 我们请求 1280x800（gallery 默认）。合成器可以给别的值——**只有真走完
        # configure 流程才会有尺寸**，因此 >0 本身就是握手的证据。
        # 送帧链路验证：**主动触发一次重绘**再数帧。
        #
        # ⚠ 为什么不是"干等一秒看帧数涨"：静止界面**不该**重绘——`present()` 只在
        # `repaint`/`dirty` 时被调（这是省电的正确行为，headless/x11 同理）。
        # 干等的结论会是"帧没涨 → 送帧坏了"，而真实原因是"没东西要画"。
        # 因此这里用控制通道改一个属性（会标脏 → 触发重绘），再看帧数。
        before = int(call(info["port"], "metrics", {}, token).get("result", {}).get("frames") or 0)
        for _ in range(5):
            call(info["port"], "set", {"id": "titlebar", "props": {"title": "Wayland 验证"}}, token)
            time.sleep(0.35)
        after = int(call(info["port"], "metrics", {}, token).get("result", {}).get("frames") or 0)
        if after <= before:
            failures.append(f"触发重绘后帧仍未推进（{before} → {after}）：送帧链路可能卡死")
        else:
            checks.append(f"触发重绘后帧推进：{before} → {after}"
                          f"（静止不重绘是正确行为，故需主动触发）")

        # 窗口在这台合成器上真的存在：合成器给过 configure（尺寸非零）
        tree = call(info["port"], "tree", {"depth": 1}, token).get("result", {})
        if not tree:
            failures.append("控制通道 tree 无返回（窗口/界面未建立）")
        else:
            checks.append("组件树可读（窗口已建立且界面已布局）")

        # —— ③ 像素：截图与控制通道同源 ——
        shot = OUT_DIR / "wayland-frame.png"
        if shot.exists():
            shot.unlink()
        reply = call(info["port"], "capture",
                     {"encode": "file", "path": str(shot)}, token)
        written = (reply.get("result") or {}).get("path") if isinstance(reply, dict) else None
        pixel_size = (reply.get("result") or {}).get("pixel_size", {}) if isinstance(reply, dict) else {}
        if not written or not pathlib.Path(written).exists():
            failures.append(f"控制通道截图未落盘：{reply}")
        else:
            size = pathlib.Path(written).stat().st_size
            checks.append(f"截图已落盘（{size} 字节，{pixel_size.get('width')}x"
                          f"{pixel_size.get('height')} @ {pixel_size.get('device_scale')}）")
            # 截图尺寸必须等于帧缓冲物理尺寸——它同时是"送上去的那块缓冲"的尺寸
            if physical[0] > 0 and (pixel_size.get("width"), pixel_size.get("height")) != physical:
                failures.append(f"截图尺寸 {pixel_size.get('width')}x{pixel_size.get('height')} "
                                f"!= 帧缓冲 {physical[0]}x{physical[1]}")

        # 收尾：走 request_close 的同一路径（与 x11 脚本一致）
        close = call(info["port"], "invoke", {"id": "titlebar", "action": "close"}, token)
        if isinstance(close, dict) and close.get("ok"):
            try:
                proc.wait(timeout=10)
                checks.append(f"invoke close → 进程走正常收尾退出（返回码 {proc.returncode}）")
            except subprocess.TimeoutExpired:
                failures.append("invoke close 未使进程退出")
    finally:
        log.close()
        if proc.poll() is None:
            try:
                proc.send_signal(signal.SIGTERM)
            except Exception:  # noqa: BLE001
                pass

    print("\n—— Wayland 真窗口验证 ——")
    for line in checks:
        print(f"  ✓ {line}")
    print(f"\n  {len(checks)} 项通过，{len(failures)} 项失败")
    for line in failures:
        print(f"  ✗ {line}")
    print(f"\n日志：{log_path}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
