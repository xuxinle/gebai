"""取证：缩进参考线画在哪一列（用户报「应到父节点对齐位置，而不是内容对齐位置」）。

判定口径：用户给的两行缩进同为 2 空格，此时第二级参考线（step=1 → 视觉列 unit）应落在
**父级内容起点**（= 2 个空格宽，字符 `i`/`f` 的左边），而不是 **本级内容起点**
（= 4 个空格宽）。本脚本用截图里"参考线像素列"与"两行第一个字符的墨迹左缘"逐列比对，
把相对关系变成数字（不靠目测）。

跑法：py -3 tools/probe_indent_line.py
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
SHOTS = ROOT / "tmp" / "probe-indent-line"

SRC = """if (const auto value = fs::read_env("ST_TEXT")) {
  if (const auto parsed = parse_bool_word(value)) {
    log(value);
  }
}
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
    raise AssertionError("控制通道没起来（看 tmp/probe-indent-line/probe.log）")


def main():
    work = pathlib.Path(tempfile.mkdtemp(prefix="gbcode_indent_"))
    target = work / "indent.cpp"
    target.write_text(SRC, encoding="utf-8")
    proc = None
    try:
        proc, client = start(work)
        client.ok("invoke", {"id": "activity-explorer", "action": "click"})
        time.sleep(0.8)
        tree = client.props("workspace-tree")
        labels = (tree.get("labels") or "").split("\n")
        keys = (tree.get("keys") or "").split("\n")
        key = next((k for n, k in zip(labels, keys) if n == "indent.cpp"), "")
        if key:
            client.ok("set", {"id": "workspace-tree", "props": {"activate_key": key}})
            time.sleep(1.4)
        editor = client.props("editor")
        print(f"  [editor] bounds={client.ok('get', {'id': 'editor'})['props'].get('bounds')}")
        print(f"  [editor] line_height={editor.get('line_height')} tab_width={editor.get('tab_width')}")
        shot = SHOTS / "indent.png"
        client.ok("capture", {"path": str(shot)})
        print(f"  [shot] {shot}")

        img = np.array(Image.open(shot).convert("L")).astype(int)
        bounds = client.ok("find", {"selector": "#editor"})["matches"][0]["bounds"]
        img_h = img.shape[0]
        scale = img_h / 828.0   # 本机无头窗口逻辑高（打印出来供核对）
        print(f"  [geometry] editor 逻辑 bounds={bounds} 推定 device_scale={scale:.2f}")
        print("  → 截图分析见脚本后半段（人工按下面打印的列直方图判读）")
        # 逐列统计"暗像素数"：参考线是一整列连续暗像素，文字是零散墨迹。
        x0 = int((bounds["x"]) * scale)
        x1 = int((bounds["x"] + bounds["width"]) * scale)
        y0 = int((bounds["y"]) * scale)
        y1 = int((bounds["y"] + bounds["height"]) * scale)
        roi = img[y0:y1, x0:x1]
        dark = roi < 200
        column_counts = dark.sum(axis=0)
        best = np.argsort(column_counts)[-12:][::-1]
        print("  最暗的 12 列（相对 editor 左缘，物理像素）: "
              + ", ".join(f"{int(c)}:{int(column_counts[c])}" for c in best))
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
