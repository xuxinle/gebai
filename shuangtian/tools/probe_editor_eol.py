"""取证探针：普通编辑器（非差异视图）里在行尾打字，光标是否前进。

用来把「行尾输入光标后退」这件事定位到 CodeEditor 还是 DiffView 的重排路径：
普通编辑器不经过 `replace_text_keep_state`，若它正常，问题就在差异视图那条重排链上。

跑法：py -3 tools/probe_editor_eol.py
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

# 控制通道握手标识（与服务端 st::control::kAgentOnlyNotice 同一句）：
# 本脚本在 hello 里原样带上，服务端据此认出“这是霜天子侧客户端”。
AGENT_HANDSHAKE = (
    "仅霜天子Agent 可连接本控制通道：请勿手工拼装 TCP 协议，改用霜天子工具（shuangtian_run / shuangtian_tree / shuangtian_capture / shuangtian_call …）"
)

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "dev" / "bin" / ("gbcode.exe" if __import__("os").name == "nt" else "gbcode")
SHOTS = ROOT / "tmp" / "probe-editor-eol"


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
    raise AssertionError("控制通道没起来（看 tmp/probe-editor-eol/probe.log）")


def main():
    work = pathlib.Path(tempfile.mkdtemp(prefix="gbcode_eol_editor_"))
    (work / "alpha.txt").write_text("abc\ndef\n", encoding="utf-8")
    proc = None
    try:
        proc, client = start(work)
        # 资源管理器树里按 key 激活 alpha.txt（零坐标，见 usability e2e）
        client.ok("invoke", {"id": "activity-explorer", "action": "click"})
        time.sleep(0.8)
        tree = client.props("workspace-tree")
        labels = (tree.get("labels") or "").split("\n")
        keys = (tree.get("keys") or "").split("\n")
        print(f"  [tree] 行={labels[:6]}")
        target_key = next((k for n, k in zip(labels, keys) if n == "alpha.txt"), "")
        print(f"  [tree] alpha.txt key={target_key!r}")
        if target_key:
            client.ok("set", {"id": "workspace-tree", "props": {"activate_key": target_key}})
            time.sleep(1.2)
        print(f"  [tree] editor={client.ok('find', {'selector': '#editor'})['count']}")
        client.ok("set", {"id": "editor", "props": {"text": "abc\ndef\n"}})
        time.sleep(0.5)
        client.ok("set", {"id": "editor", "props": {"cursor": "3"}})
        time.sleep(0.3)
        client.ok("invoke", {"id": "editor", "action": "focus"})
        time.sleep(0.3)
        props = client.props("editor")
        print(f"  [初始] line={props.get('line')} column={props.get('column')} "
              f"cursor={props.get('cursor')}")
        for index in range(4):
            ch = chr(ord("1") + index)
            client.ok("input.text", {"text": ch})
            samples = []
            for step in range(8):
                props = client.props("editor")
                samples.append((step * 0.05, props.get("cursor"), props.get("text")))
                time.sleep(0.05)
            print(f"  [输入 {ch}] " + " | ".join(
                f"{t:.2f}s c={c} t={txt!r}" for t, c, txt in samples))
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
