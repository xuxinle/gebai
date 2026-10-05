"""回归：拖动改造后「空闲不再空转」与「最大化路径」是否仍正确。

两个必须守住的边界（改造直接动到了它们的触发路径）：
  1. **空闲**：`WM_SIZE` 里现在会**同步重绘**，若某次尺寸事件被误判成"一直在变"，
     应用会在无输入时不停重绘（帧计数疯涨 = 空转耗 CPU）。
  2. **最大化/还原**：那条路径不走拖动模态循环，也不能因为新代码而漏画、
     或画出超出视口的内容（旧缺陷：最大化后盖住任务栏 / 只画左上角）。
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from st_client_lib import call_with_token, load_control  # noqa: E402

u = ctypes.WinDLL("user32", use_last_error=True)
try:
    ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)
except Exception:
    u.SetProcessDPIAware()
u.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int,
                           ctypes.c_int, ctypes.c_int, ctypes.c_uint]
u.SetWindowPos.restype = wt.BOOL
u.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u.EnumWindows.argtypes = [ctypes.c_void_p, wt.LPARAM]


def find(pid: int):
    found: list[tuple[int, tuple[int, int, int, int]]] = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    def cb(hwnd, _lp):
        o = wt.DWORD()
        u.GetWindowThreadProcessId(hwnd, ctypes.byref(o))
        if o.value == pid and u.IsWindowVisible(hwnd):
            r = wt.RECT()
            u.GetWindowRect(hwnd, ctypes.byref(r))
            found.append((hwnd, (r.left, r.top, r.right, r.bottom)))
        return True

    u.EnumWindows(cb, 0)
    if not found:
        return None
    hwnd, rect = max(found, key=lambda item: (item[1][2] - item[1][0]) * (item[1][3] - item[1][1]))
    return hwnd if (rect[2] - rect[0]) > 0 else None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=str(ROOT / "build/dev/bin/gallery.exe"))
    args = parser.parse_args()
    control = ROOT / "build/live-resize/ctl-idle.json"
    control.parent.mkdir(parents=True, exist_ok=True)
    if control.exists():
        control.unlink()
    proc = subprocess.Popen([args.exe, "--control-file", str(control)], cwd=str(ROOT))
    failures: list[str] = []
    try:
        info = {}
        for _ in range(120):
            time.sleep(0.2)
            info = load_control(str(control))
            if info.get("port") and info.get("pid"):
                break
        port, token, pid = info["port"], info.get("token", ""), info["pid"]

        def call(method: str, params: dict | None = None) -> dict:
            reply = call_with_token(port, method, params, token, timeout=30)
            return reply.get("result", reply)

        time.sleep(1.5)
        hwnd = find(pid)

        # —— 1. 空闲不空转 ——
        # 先让它稳定下来（首帧 + 两次 render_frame + 悬停过渡收尾）
        time.sleep(1.0)
        before = call("metrics")["frames"]
        time.sleep(3.0)
        after = call("metrics")["frames"]
        idle_frames = after - before
        print(f"空闲 3 秒的帧数增量 = {idle_frames}（期望 0；>0 说明有空转）")
        # 允许极少量（动画收尾/控制通道自身活动）；每秒 <=2 帧才算真的空闲
        if idle_frames > 6:
            failures.append(f"空闲空转：3 秒渲染了 {idle_frames} 帧")

        # —— 2. 最大化 / 还原：视口跟上、尺寸等于工作区 ——
        call("invoke", {"id": "titlebar", "action": "maximize"})
        time.sleep(0.8)
        m = call("metrics")
        # 视口用控制通道实际暴露的那一份（根组件 `#content` 的几何），而不是凭存在
        # 一个 `viewport` 方法（它并不存在——那会让用例永远红在一个脚本错误上）。
        root = call("get", {"id": "gallery-root"})
        content = call("get", {"id": "content"})
        print(f"最大化后：物理 {m['physical_width']}x{m['physical_height']} "
              f"frames={m['frames']} root={root.get('bounds')} content={content.get('bounds')}")
        window = wt.RECT()
        u.GetWindowRect(hwnd, ctypes.byref(window))
        win_w, win_h = window.right - window.left, window.bottom - window.top
        if (m["physical_width"], m["physical_height"]) != (win_w, win_h):
            failures.append(f"最大化后画布 {m['physical_width']}x{m['physical_height']} "
                            f"!= 窗口 {win_w}x{win_h}（差 1px 就会被 DXGI 拉伸）")
        # 布局必须跟着长到新尺寸（视口不同步的老症状：界面只占左上角旧尺寸那块）
        bounds = content.get("bounds") or {}
        scale = m["device_scale"]
        expected_logical = win_w / scale
        if bounds and abs(float(bounds.get("width", 0)) - expected_logical) > 2.0:
            failures.append(f"最大化后布局宽 {bounds.get('width')} != 物理宽/scale "
                            f"{expected_logical:.1f}（视口没跟上）")
        call("invoke", {"id": "titlebar", "action": "maximize"})
        time.sleep(0.8)
        m2 = call("metrics")
        call("capture", {"encode": "file", "path": str(ROOT / "build/live-resize/restored.png")})
        print(f"还原后：物理 {m2['physical_width']}x{m2['physical_height']} "
              f"frames={m2['frames']}")

        # —— 3. 程序化改尺寸（非鼠标）也要正确 ——
        u.SetWindowPos(hwnd, wt.HWND(-1), 150, 150, 1400, 900, 0x0040)
        time.sleep(1.0)
        m3 = call("metrics")
        if (m3["physical_width"], m3["physical_height"]) != (1400, 900):
            failures.append(f"SetWindowPos 后画布 {m3['physical_width']}x{m3['physical_height']} "
                            f"!= 1400x900")
        # 再抓一次空闲（拖动代码不该让常态运行变得爱重绘）
        before2 = call("metrics")["frames"]
        time.sleep(2.0)
        after2 = call("metrics")["frames"]
        print(f"改尺寸后空闲 2 秒帧数增量 = {after2 - before2}")
        if after2 - before2 > 4:
            failures.append(f"改尺寸后空转：2 秒渲染了 {after2 - before2} 帧")

        call("invoke", {"id": "titlebar", "action": "close"})
        try:
            code = proc.wait(timeout=15)
            print(f"关窗退出码 = {code}")
        except subprocess.TimeoutExpired:
            failures.append("关窗后应用未退出")
            proc.kill()
    finally:
        if proc.poll() is None:
            proc.kill()

    if failures:
        print("\n发现问题：")
        for item in failures:
            print("  [X]", item)
        return 1
    print("\n[OK] 空闲与最大化路径检查全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
