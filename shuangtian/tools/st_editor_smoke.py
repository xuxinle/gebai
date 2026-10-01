#!/usr/bin/env python3
"""编辑器形态冒烟：焦点链路 + 文本送达 + 模态浮层铺满视口（控制通道，无头）。

为什么单列一个脚本：这三条都是"只能靠**真实控制通道往返**证明"的约定，组件级单测
证明不了——
  - **点击即聚焦**：点击路径是 `hit_test → focusable() && set_focus`，而键盘激活
    （`activate()`）又要求先有焦点。默认不可聚焦时这是个死循环，单测直接调
    `set_focused` 看不出来。
  - **文本送到焦点元素**：`input.text` 若目标不可聚焦会静默落到旧焦点元素上
    （写错元素比报错危险）——必须按 id 断到文本真的进了编辑器。
  - **模态浮层铺满视口**：`OverlayLayout::FillViewport` 之前，应用只能每帧注入视口
    尺寸（workaround）；本脚本断的是"应用没注入也能铺满"。

用法: python3 tools/st_editor_smoke.py [codeeditor|gallery|all] [--shots DIR]
退出码 0 = 全部通过。
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import socket
import struct
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROFILE = os.environ.get("ST_PROFILE", "debug")

failures: list[str] = []


def check(condition: bool, message: str) -> bool:
    print(("  OK   " if condition else "  FAIL ") + message, flush=True)
    if not condition:
        failures.append(message)
    return condition


class Client:
    """**单连接**会话：控制协议要求每条连接先 `hello` 握手，后续请求复用同一连接。"""

    def __init__(self, port: int, token: str) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30.0)
        self.sock.settimeout(30.0)
        self.counter = 0
        hello = self.call("hello", {"token": token})
        if not hello.get("ok"):
            raise RuntimeError(f"hello 失败: {hello.get('error')}")
        self.screen = (hello.get("result") or {}).get("screen", {})

    def call(self, method: str, params: dict | None = None) -> dict:
        self.counter += 1
        body = json.dumps({"id": self.counter, "method": method, "params": params or {}}).encode()
        self.sock.sendall(struct.pack(">I", len(body)) + body)
        length = struct.unpack(">I", self._read_exact(4))[0]
        return json.loads(self._read_exact(length).decode())

    def ok(self, method: str, params: dict | None = None) -> dict:
        reply = self.call(method, params)
        if not reply.get("ok"):
            raise RuntimeError(f"{method} 失败: {reply.get('error')}")
        return reply.get("result") or {}

    def _read_exact(self, size: int) -> bytes:
        buffer = b""
        while len(buffer) < size:
            chunk = self.sock.recv(size - len(buffer))
            if not chunk:
                raise RuntimeError("连接被关闭")
            buffer += chunk
        return buffer

    def close(self) -> None:
        self.sock.close()


def start(app: str, shots: pathlib.Path) -> tuple[subprocess.Popen, Client]:
    control = shots / f"editor-smoke-{app}-control.json"
    control.unlink(missing_ok=True)
    executable = ROOT / "build" / PROFILE / "bin" / app
    if not executable.exists():
        raise RuntimeError(f"未找到 {executable}（先 st build {app}）")
    log = open(shots / f"editor-smoke-{app}.log", "wb")
    process = subprocess.Popen(
        [str(executable), "--headless", "--control-port", "0", "--control-file", str(control),
         "--shots", str(shots)],
        stdout=log, stderr=log, stdin=subprocess.DEVNULL, start_new_session=True,
    )
    for _ in range(120):
        if control.exists():
            info = json.loads(control.read_text())
            if info.get("port"):
                try:
                    return process, Client(int(info["port"]), info.get("token", ""))
                except (OSError, RuntimeError):
                    pass
        time.sleep(0.25)
    raise RuntimeError(f"{app} 未在 30 秒内就绪")


def stop(process: subprocess.Popen) -> None:
    process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()


def smoke_codeeditor(shots: pathlib.Path) -> None:
    print("[codeeditor] 焦点链路 + 文本送达", flush=True)
    process, client = start("codeeditor", shots)
    try:
        state = client.ok("get", {"id": "editor"})
        props = state.get("props", {})
        # 示例启动即 `root->set_focus(editor_ptr)`：初始应当是已聚焦
        check(props.get("focused") is True, "示例启动即聚焦编辑器（root.set_focus）")
        bounds = state["bounds"]
        center = {"x": bounds["x"] + bounds["width"] / 2, "y": bounds["y"] + bounds["height"] / 2}

        # 先显式失焦：让下面的点击成为"从无焦点到有焦点"的真实路径
        client.ok("invoke", {"id": "editor", "action": "blur"})
        check(client.ok("get", {"id": "editor"}).get("props", {}).get("focused") is False,
              "invoke blur 后失焦（focused=false）")

        client.ok("input.mouse", {"kind": "click", **center, "button": 1})
        check(client.ok("get", {"id": "editor"}).get("props", {}).get("focused") is True,
              "鼠标点击编辑区即聚焦（点击路径不依赖键盘激活）")

        before = client.ok("get", {"id": "editor"}).get("props", {}).get("text", "")
        client.ok("input.text", {"id": "editor", "text": "Z"})
        after = client.ok("get", {"id": "editor"}).get("props", {}).get("text", "")
        check(after != before and "Z" in after, "聚焦后 input.text 插入字符（文本送达焦点元素）")

        client.ok("input.key", {"kind": "press", "key": "Backspace"})
        restored = client.ok("get", {"id": "editor"}).get("props", {}).get("text", "")
        check(restored == before, "Backspace 复原（插入-删除闭环）")

        shot = client.ok("capture", {"id": "editor", "encode": "file",
                                     "path": str(shots / "editor-smoke-cursor.png")})
        check(bool(shot.get("path")), f"编辑区截图已落盘: {shot.get('path')}（人工看光标竖线）")

        # 只读视图不得成为键盘陷阱：Tab 必须能把焦点送出去
        # （只读组件在焦点环里走得到，若仍把 Tab 报成“已消费”就再也出不来了）
        client.ok("invoke", {"id": "viewer", "action": "focus"})
        focused_before = client.ok("input.key", {"kind": "press", "key": "Shift"}).get("focused")
        check(focused_before == "viewer", f"只读视图可聚焦: {focused_before}")
        after_tab = client.ok("input.key", {"kind": "press", "key": "Tab"}).get("focused")
        check(after_tab not in ("", "viewer"), f"只读视图上 Tab 逃出（焦点移到 {after_tab}）")
        client.close()
    finally:
        stop(process)


def smoke_gallery(shots: pathlib.Path) -> None:
    print("[gallery] 模态浮层（FillViewport）+ Esc 关闭", flush=True)
    process, client = start("gallery", shots)
    try:
        screen = client.screen
        client.ok("invoke", {"id": "btn-open-dialog", "action": "click"})
        dialog = client.ok("get", {"id": "demo-dialog"})
        bounds = dialog.get("bounds", {})
        check(bounds.get("width") == screen.get("width") and bounds.get("height") == screen.get("height"),
              f"对话框遮罩铺满视口（{bounds.get('width')}x{bounds.get('height')} == "
              f"{screen.get('width')}x{screen.get('height')}；应用侧未注入视口尺寸）")
        card = dialog.get("props", {}).get("card", "")
        check(card not in ("", "0,0,0,0"), f"卡片已定位: {card}")

        client.ok("input.key", {"kind": "press", "key": "Escape"})
        found = client.ok("find", {"selector": "Dialog", "limit": 5})
        check(not found.get("matches", found.get("items", [])),
              "Esc 关闭对话框（选择器已查不到 Dialog）")
        client.close()
    finally:
        stop(process)


def main() -> int:
    parser = argparse.ArgumentParser(description="编辑器形态冒烟（控制通道，无头）")
    parser.add_argument("target", nargs="?", default="all",
                        choices=["codeeditor", "gallery", "all"])
    parser.add_argument("--shots", default=None, help="截图与日志目录（默认 build/editor-smoke）")
    args = parser.parse_args()
    shots = pathlib.Path(args.shots) if args.shots else ROOT / "build" / "editor-smoke"
    shots.mkdir(parents=True, exist_ok=True)

    targets = ["codeeditor", "gallery"] if args.target == "all" else [args.target]
    try:
        for target in targets:
            if target == "codeeditor":
                smoke_codeeditor(shots)
            else:
                smoke_gallery(shots)
    except RuntimeError as error:
        check(False, str(error))
    print(("冒烟通过" if not failures else f"冒烟失败 {len(failures)} 项"), flush=True)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
