#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""启动 gbcode（无头）并通过控制通道截图。

为什么要走控制通道：`--shots` 只是**设置截图输出目录**，不会自动截图；
截图必须由控制通道的 `capture` 方法触发（见 src/app/app.cpp 的
`capture_to_file` 只在 RPC 调用时执行）。

协议：4 字节大端长度 + JSON 体；控制信息由 `--control-file` 落盘
（含 port 与 token）。

用法：
    python tools/gbcode_shot.py --out build/gbcode-shots/app.png \
        --scale 1.5 --theme dark [--frames 90] [--open <文件>]
"""
import argparse
import json
import pathlib
import socket
import struct
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
GBCODE = ROOT / "build" / "dev" / "bin" / "gbcode.exe"


def read_control(path: pathlib.Path, timeout=20.0):
    """等控制信息文件出现并解析出 port/token。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if path.exists():
            try:
                doc = json.loads(path.read_text(encoding="utf-8"))
                port = doc.get("port") or doc.get("control_port")
                token = doc.get("token")
                if port:
                    return int(port), token
            except Exception:
                pass
        time.sleep(0.2)
    return None, None


class Client:
    def __init__(self, port, token):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30)
        self.token = token
        self.seq = 0

    def call(self, method, params=None):
        self.seq += 1
        body = json.dumps({"id": self.seq, "method": method,
                           "params": params or {}}).encode("utf-8")
        if self.token:
            # 多数方法需要 token；由服务端校验，这里统一带上
            pass
        self.sock.sendall(struct.pack(">I", len(body)) + body)
        # 读回包（4 字节长度 + JSON）
        head = self._read_exact(4)
        if not head:
            raise RuntimeError("连接已关闭")
        n = struct.unpack(">I", head)[0]
        payload = self._read_exact(n)
        return json.loads(payload.decode("utf-8"))

    def _read_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--scale", default="1.5")
    ap.add_argument("--theme", default="dark")
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--region", default="", help="x,y,w,h（物理像素），留空=全窗口")
    ap.add_argument("--boot-wait", type=float, default=1.2)
    a = ap.parse_args()

    if not GBCODE.exists():
        raise SystemExit(f"gbcode 未构建：{GBCODE}\n先跑 st build gbcode --profile=dev")

    out = pathlib.Path(a.out)
    if not out.is_absolute():
        out = (ROOT / out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)

    ctl = ROOT / "build" / "gbcode-control.json"
    if ctl.exists():
        ctl.unlink()

    # 参数用**空格分隔**（`--x=v` 形式在部分参数上不被接受，实测 --control-port=0
    # 会让进程直接退出，什么都不打印）。
    args = [str(GBCODE), "--headless", "--scale", str(a.scale), "--theme", a.theme,
            "--control-file", str(ctl), "--frames", str(a.frames)]
    print("启动:", " ".join(args))
    # 编码必须显式 utf-8：应用日志含中文，Windows 默认 GBK 会 UnicodeDecodeError。
    proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace")
    client = None
    try:
        port, token = read_control(ctl)
        if not port:
            print("⚠ 没等到控制端口", file=sys.stderr)
            return 1
        print(f"控制通道 :{port}")
        client = Client(port, token)
        # **必须先 hello**（服务端强制）：不带 token 的调用一律回
        # `{"error":{"code":"invalid","message":"必须先 hello（携带 token）"}}`。
        hello = client.call("hello", {"token": token} if token else {})
        if isinstance(hello, dict) and hello.get("error"):
            print("hello 失败:", hello["error"], file=sys.stderr)
            return 1
        time.sleep(a.boot_wait)   # 等首帧画完（会话标签恢复在首帧后落地）

        params = {}
        if a.region:
            x, y, w, h = (int(v) for v in a.region.split(","))
            params["region"] = {"x": x, "y": y, "width": w, "height": h}
        params["path"] = str(out)
        r = client.call("capture", params)
        ok = isinstance(r, dict) and not r.get("error")
        print("capture 回包:", json.dumps(r, ensure_ascii=False)[:300])
        if not ok:
            return 1
        print("截图:", out, out.stat().st_size if out.exists() else "(缺失)")
        return 0 if out.exists() else 1
    finally:
        if client:
            client.close()
        try:
            proc.terminate()
            proc.wait(timeout=8)
        except Exception:
            proc.kill()
        # 打印应用日志尾部，便于排查
        if proc.stdout:
            tail = proc.stdout.read()[-1500:]
            if tail.strip():
                print("--- app 日志尾部 ---")
                print(tail)


if __name__ == "__main__":
    sys.exit(main())
