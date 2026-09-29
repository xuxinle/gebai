#!/usr/bin/env python3
"""Windows 窗口后端的可重复验证（在 Linux 上跑：wine + Xvfb）。

为什么需要：Win32 窗口路径**在 Linux 上编译得到、但跑不起来**——
而它的正确性恰恰只在"真的创建窗口、真的收到鼠标键盘消息"时才能确认。
本轮实现 Win32 后端时，两个真实缺陷都是靠这个闭环发现的：

  - 只发 `MouseDown/MouseUp` 而漏了 `Click` → 按钮"有焦点、有按压效果，点了没反应"；
  - 像素从画布搬到 DIB 时需要重排通道（否则红蓝互换，截图看着"色调不对"）。

因此把这条验证路径固化成脚本：启动 → 断言后端/视口 → **真实鼠标点击** →
断言主题变化 → **真实键盘输入** → 断言输入值 → 缩放窗口 → 断言视口跟随 →
截图留证 → 优雅退出。

依赖：`wine`（`apt install wine64`）、`Xvfb`、`xdotool`、`imagemagick`（`import`）。
用法：`python3 tools/st_win_check.py [--exe PATH] [--keep-shots DIR]`
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_EXE = ROOT / "build/dev-mingw/bin/gallery.exe"
SHOT_DIR = ROOT / "build/win-check"


def log(message: str) -> None:
    print(message, flush=True)


def find_wine() -> str | None:
    for candidate in ("wine64", "wine", "/usr/lib/wine/wine64"):
        found = shutil.which(candidate) or (candidate if pathlib.Path(candidate).exists() else None)
        if found:
            return found
    return None


def call(port: int, method: str, params: dict | None = None) -> dict:
    connection = socket.create_connection(("127.0.0.1", port), timeout=15)
    payload = json.dumps({"id": 1, "method": method, "params": params or {}}).encode()
    connection.sendall(struct.pack(">I", len(payload)) + payload)
    header = b""
    while len(header) < 4:
        header += connection.recv(4 - len(header))
    (length,) = struct.unpack(">I", header)
    body = b""
    while len(body) < length:
        body += connection.recv(length - len(body))
    connection.close()
    return json.loads(body)


def wait_for_port(port: int, timeout: float = 90.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            connection = socket.create_connection(("127.0.0.1", port), timeout=2)
            connection.close()
            return True
        except OSError:
            time.sleep(0.5)
    return False


def window_id(title: str) -> str | None:
    result = subprocess.run(["xdotool", "search", "--name", title], capture_output=True, text=True)
    ids = [line for line in result.stdout.split() if line.strip()]
    return ids[0] if ids else None


def screenshot(path: pathlib.Path, crop: tuple[int, int, int, int] | None = None) -> bool:
    raw = path.with_suffix(".raw.png")
    if subprocess.run(["import", "-window", "root", str(raw)], capture_output=True).returncode != 0:
        return False
    if crop is not None and shutil.which("python3"):
        script = (
            "from PIL import Image;"
            f"im=Image.open({str(raw)!r}).convert('RGB');"
            f"im.crop({crop!r}).save({str(path)!r})"
        )
        if subprocess.run(["python3", "-c", script], capture_output=True).returncode == 0:
            raw.unlink(missing_ok=True)
            return True
    raw.replace(path)
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=str(DEFAULT_EXE))
    parser.add_argument("--port", type=int, default=45777)
    parser.add_argument("--display", default=":99")
    args = parser.parse_args()

    failures: list[str] = []
    checks = 0

    def check(condition: bool, description: str, detail: str = "") -> None:
        nonlocal checks
        checks += 1
        if condition:
            log(f"  [ok]   {description}")
        else:
            failures.append(description)
            log(f"  [FAIL] {description}{(' · ' + detail) if detail else ''}")

    wine = find_wine()
    if wine is None:
        log("跳过：未安装 wine（apt install wine64）")
        return 0
    if not pathlib.Path(args.exe).exists():
        log(f"跳过：可执行文件不存在 {args.exe}（先 st build <target> --toolchain=mingw）")
        return 0
    for tool in ("Xvfb", "xdotool", "import"):
        if shutil.which(tool) is None:
            log(f"跳过：缺少 {tool}")
            return 0

    SHOT_DIR.mkdir(parents=True, exist_ok=True)
    os.environ["DISPLAY"] = args.display
    os.environ["WINEDEBUG"] = "-all"
    os.environ.setdefault("WINEPREFIX", "/tmp/wineprefix")

    # Xvfb（已有就复用）
    xvfb = subprocess.Popen(
        ["Xvfb", args.display, "-screen", "0", "1600x1000x24"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(3)

    log(f"启动 {pathlib.Path(args.exe).name}（wine + Xvfb）")
    process = subprocess.Popen(
        [wine, args.exe, "--control-port", str(args.port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )
    try:
        if not wait_for_port(args.port):
            log("  [FAIL] 控制通道未就绪（应用可能没起来——请看下方输出）")
            if process.stdout is not None:
                log(process.stdout.read()[-2000:])
            return 1

        log("① 后端与视口")
        hello = call(args.port, "hello")["result"]
        check(hello["backend"] == "win32", f"后端为 win32（实际 {hello['backend']}）")
        check(hello["headless"] is False, "非无头模式")
        check(hello["screen"]["width"] > 0, f"视口宽度 {hello['screen']['width']}")

        wid = window_id("霜天")
        check(wid is not None, "X 窗口已创建（wine 侧可见）")
        screenshot(SHOT_DIR / "01-initial.png")
        log(f"  截图 → {SHOT_DIR / '01-initial.png'}")

        if wid is not None:
            log("② 真实鼠标点击：主题切换按钮")
            theme_button = call(args.port, "find", {"selector": "Button[text~=色]"})["result"]
            if theme_button["count"] > 0:
                bounds = theme_button["matches"][0]["bounds"]
                before = call(args.port, "theme")["result"]["mode"]
                # 逻辑坐标 → 屏幕坐标（窗口客户区左上在 (4,32)——无窗口管理器时的定位）
                point = (int(bounds["x"] + bounds["width"] / 2) + 4, int(bounds["y"] + bounds["height"] / 2) + 32)
                subprocess.run(["xdotool", "windowactivate", wid], capture_output=True)
                subprocess.run(["xdotool", "mousemove", "--sync", str(point[0]), str(point[1])], check=False)
                time.sleep(0.6)
                subprocess.run(["xdotool", "click", "1"], check=False)
                time.sleep(1.5)
                after = call(args.port, "theme")["result"]["mode"]
                check(after != before, f"点击改变了主题（{before} → {after}）",
                      "按钮未激活：检查事件序列是否发全（Down/Up/**Click**）")
                screenshot(SHOT_DIR / "02-after-click.png")
                log(f"  截图 → {SHOT_DIR / '02-after-click.png'}")
            else:
                check(False, "找到主题切换按钮", "选择器未命中")

            log("③ 真实键盘输入")
            inputs = call(args.port, "find", {"selector": "Input"})["result"]
            if inputs["count"] > 0:
                bounds = inputs["matches"][0]["bounds"]
                point = (int(bounds["x"] + bounds["width"] / 2) + 4, int(bounds["y"] + bounds["height"] / 2) + 32)
                subprocess.run(["xdotool", "mousemove", "--sync", str(point[0]), str(point[1])], check=False)
                time.sleep(0.5)
                subprocess.run(["xdotool", "click", "1"], check=False)
                time.sleep(0.8)
                subprocess.run(["xdotool", "type", "--delay", "60", "win-check"], check=False)
                time.sleep(1.2)
                focused = call(args.port, "find", {"selector": "Input:focused"})["result"]
                value = ""
                if focused["count"] > 0:
                    props = call(args.port, "get", {"id": focused["matches"][0]["id"], "props": ["value"]})
                    value = props["result"]["props"].get("value", "")
                check("win-check" in value, f"真实键盘输入进入输入框（value={value!r}）",
                      "检查 WM_CHAR → TextInput 的映射")
                screenshot(SHOT_DIR / "03-after-typing.png")
            else:
                check(False, "找到输入框", "选择器未命中")

            log("④ 窗口缩放跟随")
            subprocess.run(["xdotool", "windowsize", wid, "900", "600"], check=False)
            time.sleep(2.0)
            resized = call(args.port, "hello")["result"]["screen"]
            check(abs(resized["width"] - 900) < 4 and abs(resized["height"] - 600) < 4,
                  f"视口跟随窗口（{resized['width']}x{resized['height']}）",
                  "检查 WM_SIZE → 帧缓冲重建 + Application 视口同步")
            screenshot(SHOT_DIR / "04-resized.png")

        log("⑤ 优雅退出")
        call(args.port, "shutdown")
        # 等应用自己收尾：wine 的加载器进程可能比应用多存活一会儿（这是 wine 的行为，不是缺陷），
        # 因此以"应用自己打印了退出日志"为准，进程退出只作为附加确认。
        app_exited = False
        for _ in range(16):
            time.sleep(0.5)
            if process.poll() is not None:
                app_exited = True
                break
        output = ""
        if process.stdout is not None and process.poll() is not None:
            output = process.stdout.read()
        logged_exit = "应用退出" in output or "退出：" in output
        check(logged_exit or app_exited, "应用已完成收尾并退出（控制通道 shutdown）",
              "应用未响应 shutdown")
        check("win32" in output, "退出日志确认 win32 后端")
    finally:
        if process.poll() is None:
            process.terminate()
            time.sleep(1)
            if process.poll() is None:
                process.kill()
        xvfb.terminate()

    log(f"\n检查 {checks} 项 · 失败 {len(failures)} 项")
    for item in failures:
        log(f"  - {item}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
