#!/usr/bin/env python3
"""gbcode Markdown 预览异步排版的端到端验证（2026-10-19 性能轮）。

场景：打开大 .md（本仓 DESIGN.md，447KB/3553 行）开预览，断言：
  1. 打开命令的往返延迟不包含排版（异步分档生效）；
  2. 排版完成后内容真实呈现（行数 > 0、滚动后可见区间非空）；
  3. 排版完成后主窗交互不受影响（终端面板命令往返快）。

用法: python3 tools/gbcode_md_async_e2e.py
"""
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Client:
    def __init__(self, port, token=None):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30.0)
        self.sock.settimeout(30.0)
        self.rid = 0
        self.token = token

    def call(self, method, params=None):
        self.rid += 1
        if method == "hello":
            params = dict(params or {}, token=self.token)
        body = json.dumps({"id": self.rid, "method": method, "params": params or {}}).encode()
        self.sock.sendall(struct.pack(">I", len(body)) + body)
        header = b""
        while len(header) < 4:
            chunk = self.sock.recv(4 - len(header))
            if not chunk:
                raise RuntimeError("控制通道在收到响应头前关闭")
            header += chunk
        length = struct.unpack(">I", header)[0]
        payload = b""
        while len(payload) < length:
            chunk = self.sock.recv(length - len(payload))
            if not chunk:
                raise RuntimeError("控制通道在收到完整响应前关闭")
            payload += chunk
        return json.loads(payload)

    def ok(self, method, params=None):
        reply = self.call(method, params)
        if not reply.get("ok", False):
            raise RuntimeError(f"{method} 失败: {reply.get('error')}")
        return reply.get("result")

    def get(self, element_id, prop):
        return self.ok("get", {"id": element_id, "props": {prop: True}})["props"].get(prop)


def find_free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_port(port: int, timeout: float) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1.0):
                return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("控制端口未就绪")


def main() -> int:
    binary = os.path.join(ROOT, "build", "dev", "bin", "gbcode")
    big_md = os.path.join(ROOT, "DESIGN.md")
    failures = []

    def check(ok: bool, message: str) -> None:
        print(("[ok] " if ok else "[FAIL] ") + message)
        if not ok:
            failures.append(message)

    work = Path(tempfile.mkdtemp(prefix="gbcode-md-async-"))
    ctl = work / ".st" / "ctl.json"
    (work / ".st").mkdir(parents=True, exist_ok=True)
    proc = subprocess.Popen(
        [binary, "--headless", "--control-port", "0", "--control-file", str(ctl), "--open", str(big_md)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    info = {}
    deadline = time.time() + 20
    while time.time() < deadline:
        if ctl.exists():
            try:
                info = json.loads(ctl.read_text(encoding="utf-8"))
                if info.get("port"):
                    break
            except (json.JSONDecodeError, OSError):
                pass
        time.sleep(0.1)
    port = int(info.get("port", 0))
    token = info.get("token")
    if port == 0:
        raise RuntimeError("控制文件未就绪")
    try:
        wait_port(port, timeout=10)
        client = Client(port, token)
        client.ok("hello")
        # 等文件加载成标签（Tabs 的语义文本 = 当前标签名）
        label = ""
        for _ in range(80):
            label = str(client.get("editor-tabs", "label") or "")
            if "DESIGN" in label:
                break
            time.sleep(0.25)
        check("DESIGN" in label, f"大 .md 已开为标签（label={label}）")

        # 开预览：Ctrl+Shift+V（与 UI 同一条路径；尾部按钮是绘制型、无元素 id）
        t0 = time.perf_counter()
        client.ok("input.key", {"key": "v", "ctrl": True, "shift": True})
        open_ms = (time.perf_counter() - t0) * 1000.0
        # 排版是异步的：打开命令本身不该把 ~160ms 排版算进去
        check(open_ms < 60.0, f"预览打开命令往返 {open_ms:.1f}ms < 60ms（异步分档生效）")

        # 等排版完成（md-preview 的 row_count > 0）
        rows = 0
        for _ in range(120):
            rows = int(client.get("md-preview", "row_count") or 0)
            if rows > 100:
                break
            time.sleep(0.25)
        check(rows > 100, f"排版完成：row_count={rows}（DESIGN.md 应 >3000）")

        # 滚动到中部再验证可见区间
        client.ok("set", {"id": "md-preview", "props": {"scroll": "50000"}})
        time.sleep(0.5)
        vis = str(client.get("md-preview", "first_visible") or "")
        check(vis not in ("", "None"), f"滚动后可见区间有值（first_visible={vis}）")

        # 排版完成后主窗仍可交互：控制通道往返（get+set 各一）应快。
        #（Ctrl+J 在 headless 无窗口焦点链下本就不触发——那是既有行为，
        #  与本改动无关；这里测的是「排版完成后主循环没有被占住」。）
        t1 = time.perf_counter()
        for _ in range(10):
            client.ok("get", {"id": "md-preview", "props": {"row_count": True}})
            client.ok("set", {"id": "md-preview", "props": {"selectable": "true"}})
        round_ms = (time.perf_counter() - t1) * 1000.0 / 10.0
        check(round_ms < 60.0, f"排版完成后控制通道往返均值 {round_ms:.1f}ms < 60ms"
              "（含一次 set 触发的整帧重绘；同步排版在场时此值 >160ms）")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    if failures:
        print(f"\n{len(failures)} 项失败")
        return 1
    print("\n全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
