"""取证：缩进参考线的**深浅**是否随缩进深度变化（用户报“重叠加粗”）。

只回答一个问题：同一列上的同一条线，在浅缩进行与深缩进行上是不是同一个色。
做法：两行内容（缩 1 级 / 缩 8 级），取列 0 那一条线的像素值。

跑法：py -3 tools/probe_guide_ink.py
"""

import json
import pathlib
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

import numpy as np
from PIL import Image

# 控制通道握手标识（与服务端 st::control::kAgentOnlyNotice 同一句）：
# 本脚本在 hello 里原样带上，服务端据此认出“这是霜天子侧客户端”。
AGENT_HANDSHAKE = (
    "仅霜天子Agent 可连接本控制通道：请勿手工拼装 TCP 协议，改用霜天子工具（shuangtian_run / shuangtian_tree / shuangtian_capture / shuangtian_call …）"
)

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "dev" / "bin" / "gbcode.exe"
SHOTS = ROOT / "tmp" / "probe-guide-ink"

# 与用户截图同类：Python，逐层加深的嵌套
SRC = """def load_tracks(path):
    if not path.exists():
        for row in reader:
            if not required.issubset(row):
                try:
                    for item in row:
                        frames.append(
                            item
                        )
                except (ValueError, KeyError):
                    continue
    return frames
"""


class Client:
    def __init__(self, port, token):
        self.port = port
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30.0)
        self.sock.settimeout(30.0)
        self.rid = 0
        self.token = token

    def _call(self, method, params=None):
        self.rid += 1
        if method == "hello":
            params = dict(params or {}, token=self.token)
            params["agent"] = AGENT_HANDSHAKE
        body = json.dumps({"id": self.rid, "method": method, "params": params or {}}).encode()
        self.sock.sendall(struct.pack(">I", len(body)) + body)
        payload = self._read(struct.unpack(">I", self._read(4))[0])
        return json.loads(payload)

    def _read(self, size):
        buf = b""
        while len(buf) < size:
            chunk = self.sock.recv(size - len(buf))
            if not chunk:
                raise RuntimeError("控制通道提前关闭")
            buf += chunk
        return buf

    def ok(self, method, params=None):
        reply = self._call(method, params)
        if not reply.get("ok"):
            raise AssertionError(f"{method} 失败: {reply.get('error')}")
        return reply.get("result") or {}

    def props(self, element_id):
        return self.ok("get", {"id": element_id})["props"]


def start(workspace):
    SHOTS.mkdir(parents=True, exist_ok=True)
    ctl = SHOTS / "ctl.json"
    ctl.unlink(missing_ok=True)
    log_file = open(SHOTS / "probe.log", "wb")
    proc = subprocess.Popen(
        [str(BIN), "--headless", "--control-port", "0", "--control-file", str(ctl),
         "--shots", str(SHOTS), "--tool-root", str(ROOT), "--workspace", str(workspace)],
        stdout=log_file, stderr=subprocess.STDOUT)
    for _ in range(160):
        if ctl.exists():
            try:
                info = json.loads(ctl.read_text(encoding="utf-8"))
                if info.get("port"):
                    client = Client(info["port"], info.get("token", ""))
                    client.ok("hello")
                    return proc, client
            except Exception:  # noqa: BLE001
                pass
        time.sleep(0.25)
    proc.kill()
    raise AssertionError("控制通道没起来（看 tmp/probe-guide-ink/probe.log）")


def main():
    work = pathlib.Path(tempfile.mkdtemp(prefix="gbcode_guide_"))
    (work / "tracks.py").write_text(SRC, encoding="utf-8")
    proc = None
    try:
        proc, client = start(work)
        client.ok("invoke", {"id": "activity-explorer", "action": "click"})
        time.sleep(0.8)
        tree = client.props("workspace-tree")
        labels = (tree.get("labels") or "").split("\n")
        keys = (tree.get("keys") or "").split("\n")
        key = next((k for n, k in zip(labels, keys) if n == "tracks.py"), "")
        if key:
            client.ok("set", {"id": "workspace-tree", "props": {"activate_key": key}})
            time.sleep(1.4)
        shot = SHOTS / "guide-ink.png"
        client.ok("capture", {"path": str(shot)})
        editor = client.props("editor")
        print(f"  [editor] line_height={editor.get('line_height')} guides={editor.get('indent_guides')}")

        img = np.array(Image.open(shot).convert("RGB")).astype(int)
        scale = img.shape[0] / 828.0
        box = client.ok("find", {"selector": "#editor"})["matches"][0]["bounds"]
        lh = float(editor.get("line_height") or 21)
        # 正文原点：行号槽之后。用「每行第一个非底色像素」反推太脆，直接按
        # 行号槽宽（gutter）扫：找列 0 线——它是**每行都出现**的那一列。
        x0 = int((box["x"] + 40) * scale)
        x1 = int((box["x"] + box["width"] * 0.6) * scale)
        y0 = int(box["y"] * scale)
        y1 = int((box["y"] + box["height"]) * scale)
        roi = img[y0:y1, x0:x1]
        gray = roi.mean(axis=2)
        # 每条线所在列：整列“明显暗于背景”的行数很多（背景 ~250）
        dark_per_col = (gray < 235).sum(axis=0)
        candidates = np.argsort(dark_per_col)[-8:][::-1]
        print("  候选线列（相对裁剪左缘，物理像素）: "
              + ", ".join(f"{int(c)}:{int(dark_per_col[c])}" for c in candidates))
        print(f"  推定 device_scale={scale:.2f} line_height={lh}")
        print("  —— 各行在候选列上的暗度（越小越黑）：")
        for col in sorted(candidates[:4]):
            vals = []
            for row in range(0, min(9, int((y1 - y0) / (lh * scale)))):
                top = int(row * lh * scale) + 3
                bottom = int((row + 1) * lh * scale) - 3
                if bottom <= top:
                    continue
                vals.append(int(gray[top:bottom, col].min()))
            print(f"    col {int(col)}: " + " ".join(f"r{r}={v}" for r, v in enumerate(vals)))
    finally:
        if proc is not None:
            try:
                proc.terminate()
                proc.wait(timeout=10)
            except Exception:  # noqa: BLE001
                pass
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
