"""空白文本对比（一体视图）视觉核验：出图给人看，并断言两栏确实在同一个差异视图里。

为何要这个脚本：旧形态是「上面两块输入框 + 下面只读差异」，改成一体后
**只有截图能确认“长得和文件差异一样”**——属性面只能证明“可编辑 / 有差异块”，
证明不了“没有第二套输入区、没有上下分家”。

跑法：py -3 tools/gbcode_compare_blank_e2e.py
"""

import base64
import json
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import time

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "dev" / "bin" / ("gbcode.exe" if __import__("os").name == "nt" else "gbcode")
SHOTS = ROOT / "tmp" / "gbcode-compare-shots"

sys.path.insert(0, str(ROOT / "tools"))


class Client:
    """控制通道客户端（长度前缀 JSON，与 `gbcode_compare_e2e.py` 同一套）。"""

    def __init__(self, port, token):
        self.port = port
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30.0)
        self.sock.settimeout(30.0)
        self.rid = 0
        self.token = token

    def call(self, method, params=None):
        try:
            return self._call(method, params)
        except ConnectionAbortedError:
            self.sock.close()
            self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=30.0)
            self.sock.settimeout(30.0)
            self.rid = 0
            self._call("hello")
            return self._call(method, params)

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
        reply = self.call(method, params)
        if not reply.get("ok"):
            raise AssertionError(f"{method} 失败: {reply.get('error')}")
        return reply.get("result") or {}

    def count(self, selector):
        return self.ok("find", {"selector": selector})["count"]

    def props(self, element_id):
        return self.ok("get", {"id": element_id})["props"]

    def set_prop(self, element_id, name, value):
        return self.ok("set", {"id": element_id, "props": {name: value}})

    def key(self, key, **mods):
        self.ok("input.key", {"key": key, **mods})

    def text(self, element_id):
        return "".join(part.get("text", "") for part in
                       self.ok("get", {"id": element_id})["props"].get("lines", []))

    def screenshot(self, path):
        result = self.ok("capture", {})
        data = result.get("base64") or result.get("png_base64") or result.get("data")
        pathlib.Path(path).write_bytes(base64.b64decode(data))


def start(workspace):
    SHOTS.mkdir(parents=True, exist_ok=True)
    ctl = SHOTS / "gbcode-ctl-blank.json"
    ctl.unlink(missing_ok=True)
    log_file = open(SHOTS / "gbcode-compare-blank.log", "wb")
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
    raise AssertionError("gbcode 控制通道没起来（看 tmp/gbcode-compare-shots/gbcode-compare-blank.log）")


def main():
    if not BIN.exists():
        print(f"找不到 {BIN}；先 `st build gbcode --profile dev`")
        return 2

    work = pathlib.Path(tempfile.mkdtemp(prefix="gbcode_blank_"))
    # 需要**一个真工作区**：空目录下命令面板里那些“需要工作区”的命令可能不列出来，
    # 而本脚本要跑通的正是其中一条（比较两段文本）。这不是测试照顾：真实用户也总是
    # 在工作区里用。
    (work / "alpha.txt").write_text("one\ntwo\nthree\n", encoding="utf-8")
    failures = []
    proc = None

    def check(ok, label):
        print(("  PASS " if ok else "  FAIL ") + label)
        if not ok:
            failures.append(label)

    try:
        proc, client = start(work)
        # 命令面板 → 「比较两段文本」
        # ⚠ 快捷键是小写 `p`（`Ctrl+Shift+P` 与 `Ctrl+Shift+p` 在事件里是同一个键，
        # 但历史脚本一律写小写——别发明大写）。
        client.key("p", ctrl=True, shift=True)
        time.sleep(0.7)
        check(client.count("#command-palette") == 1, "命令面板未打开")
        client.ok("input.text", {"text": "两段文本"})
        time.sleep(0.6)
        palette = client.props("command-palette")
        check(int(palette.get("match_count") or 0) >= 1,
              f"命令面板搜到「比较两段文本」（match_count={palette.get('match_count')}）")
        client.key("Enter")
        time.sleep(1.0)

        # —— 形态：一体视图，没有第二套输入区 ——
        check(client.count("#compare-view") == 1, "空白对比用差异视图本体（#compare-view）")
        check(client.count("#compare-inputs") == 0, "不再有单独的输入框区（#compare-inputs）")
        check(client.count("#diff-left") == 1 and client.count("#diff-right") == 1,
              "两栏都在差异视图里（#diff-left / #diff-right）")
        left_props = client.props("diff-left")
        right_props = client.props("diff-right")
        check(left_props.get("read_only") == "false", "左栏可编辑")
        check(right_props.get("read_only") == "false", "右栏可编辑")

        # —— 内容：两侧各键入不同文本 → 差异块与摘要跟着变 ——
        client.set_prop("diff-left", "text", "alpha\nbeta\ngamma\ndelta")
        time.sleep(0.5)
        client.set_prop("diff-right", "text", "alpha\nBETA\ngamma\ndelta\nEPSILON")
        time.sleep(1.0)
        view = client.props("compare-view")
        check(int(view.get("hunks") or 0) >= 1, f"出现差异块（hunks={view.get('hunks')}）")
        check("EPSILON" in (view.get("logical_right") or ""),
              "右栏逻辑文本含新键入的内容（填充行不会吞掉真实内容）")
        check("beta" in (view.get("logical_left") or ""),
              "左栏逻辑文本含原内容")
        print(f"  [摘要] {view.get('summary')}")

        shot = SHOTS / "compare-blank.png"
        client.screenshot(shot)
        print(f"  [截图] {shot}")
    finally:
        if proc is not None:
            try:
                proc.terminate()
                proc.wait(timeout=10)
            except Exception:  # noqa: BLE001
                pass
        import shutil
        shutil.rmtree(work, ignore_errors=True)

    print(f"\n失败 {len(failures)} 项")
    for item in failures:
        print(" -", item)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
