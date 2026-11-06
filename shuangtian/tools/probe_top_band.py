"""像素量：窗口上沿到第一个面板内容之间各段的高度到底是多少。

用户报「标题栏到下方面板距离太宽，显得上方空条有点宽，看看是标题栏的高度还是间隙
还是面板边框要调」。三处都在同一段竖直区间里，只看树数据分不清"哪一段占了宽度"，
所以这里**按行统计颜色**，把区间切成可命名的色带：

  行 0..a      标题栏（Surface::Alt 底 + 居中菜单文字）
  a..b         画布 padding（kPanelGap，透出外壳色）
  b..c         卡片圆角/边框 + 标签条内部上内边距
  c..          标签 pill 与正文

输出：每一段的起止行（**逻辑像素**）与主导色。跑法：py -3 tools/probe_top_band.py
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

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "dev" / "bin" / "gbcode.exe"
SHOTS = ROOT / "tmp" / "probe-top-band"


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
    raise AssertionError("控制通道没起来（看 tmp/probe-top-band/probe.log）")


def main():
    work = pathlib.Path(tempfile.mkdtemp(prefix="gbcode_band_"))
    (work / "a.txt").write_text("hi\n", encoding="utf-8")
    proc = None
    try:
        proc, client = start(work)
        shot = SHOTS / "top.png"
        client.ok("capture", {"path": str(shot)})
        bounds = client.ok("find", {"selector": "#editor"})
        print(f"  editor count={bounds['count']}")
        tree = client.ok("tree", {}).get("tree") or {}

        def walk(node, out=None):
            out = out if out is not None else {}
            out[node.get("id") or node.get("type")] = node
            for child in node.get("children") or []:
                walk(child, out)
            return out

        index = walk(tree)
        for name in ("titlebar", "menu-block", "editor-page", "workbench-row",
                     "workbench-cards", "editor-upper", "sidebar", "sidebar-explorer",
                     "tabbar", "statusbar"):
            node = index.get(name)
            if node:
                b = node["bounds"]
                print(f"  [tree] {name}: y={b['y']} h={b['height']} x={b['x']} w={b['width']}")

        img = np.array(Image.open(shot).convert("RGB")).astype(int)
        h, w, _ = img.shape
        scale = h / 828.0   # 逻辑高 828（与本机窗口一致时才成立；打印出来供核对）
        print(f"  [shot] {w}x{h}  推定 device_scale={scale:.2f}")

        # 沿一条**不出图标/文字**的竖直切片统计（x 取窗口右侧 1/4 处，
        # 那里只有底色，没有菜单字、没有标签 pill）。
        column = img[:, int(w * 0.75)]
        runs = []
        prev = None
        start_row = 0
        for row in range(min(h, int(180 * scale))):
            key = tuple((column[row] // 6 * 6).tolist())   # 6 级量化，吸收抗锯齿
            if prev is None:
                prev, start_row = key, row
            elif key != prev:
                runs.append((start_row, row, prev))
                prev, start_row = key, row
        runs.append((start_row, min(h, int(180 * scale)), prev))
        print("  竖直色带（逻辑 y 起 ~ 止, 高度, RGB）：")
        for a, b, color in runs:
            if b - a < 1:
                continue
            print(f"    {a / scale:6.1f} ~ {b / scale:6.1f}  h={(b - a) / scale:5.1f}  "
                  f"rgb={color}")
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
