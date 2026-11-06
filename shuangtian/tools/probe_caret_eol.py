"""取证探针：**文本对比（行内）视图里在行尾打字，光标跑到哪**。

用户报的现象：「对比视图编辑时，每当光标在行尾输入时，光标会自动跳转到最后一个字符前面」。

这个探针只做一件事：把「行尾输入」这条路径每次输入后的光标行列打出来，
用来判断是哪一层把列号算错了（CodeEditor::replace_text_keep_state 的
locate/offset_of，还是 DiffView 推文本时给的 old_real 表，还是宿主重组时机）。

跑法：py -3 tools/probe_caret_eol.py [--split]
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

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "dev" / "bin" / ("gbcode.exe" if __import__("os").name == "nt" else "gbcode")
SHOTS = ROOT / "tmp" / "probe-caret-eol"


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
    raise AssertionError("控制通道没起来（看 tmp/probe-caret-eol/probe.log）")


def main():
    split = "--split" in sys.argv
    work = pathlib.Path(tempfile.mkdtemp(prefix="gbcode_eol_"))
    (work / "alpha.txt").write_text("one\n", encoding="utf-8")
    proc = None
    try:
        proc, client = start(work)
        client.ok("input.key", {"key": "p", "ctrl": True, "shift": True})
        time.sleep(0.8)
        client.ok("input.text", {"text": "两段文本"})
        time.sleep(0.4)
        # 无头下 `input.text` 不一定落在面板过滤框（焦点时序），直接经属性面写查询词。
        client.ok("set", {"id": "command-palette", "props": {"query": "比较两段文本"}})
        time.sleep(0.6)
        print(f"  [palette] query={client.props('command-palette').get('query')!r} "
              f"match_count={client.props('command-palette').get('match_count')}")
        print(f"  [palette] match_count={client.props('command-palette').get('match_count')}")
        client.ok("input.key", {"key": "Enter"})
        time.sleep(1.2)
        print(f"  [tree] compare-view={client.ok('find', {'selector': '#compare-view'})['count']}")
        if split:
            client.ok("invoke", {"id": "compare-layout", "action": "click"})
            time.sleep(0.8)
        for props in client.ok("get", {"id": "compare-view"}).get("props", {}).items():
            if props[0] in ("layout", "hunks", "logical_left", "logical_right", "read_only"):
                print(f"  [view] {props[0]} = {props[1]!r}")
        left_read_only = client.props("diff-left").get("read_only")
        right_read_only = client.props("diff-right").get("read_only")
        print(f"  [view] left read_only={left_read_only} right read_only={right_read_only}")

        # 用「谁可写」决定往哪一栏输入（行内对比时右栏被清空、不可见）。
        side = "diff-left" if left_read_only == "false" else "diff-right"
        print(f"  [view] 输入目标 = {side}")

        # 行尾场景：内容 "abc" 没有换行，光标放末尾（行内第 3 列之后）。
        client.ok("set", {"id": side, "props": {"text": "abc"}})
        time.sleep(0.6)
        client.ok("set", {"id": side, "props": {"cursor": "3"}})
        time.sleep(0.4)
        client.ok("invoke", {"id": side, "action": "focus"})
        time.sleep(0.3)
        for label in ("初始", ):
            props = client.props(side)
            print(f"  [{label}] line={props.get('line')} column={props.get('column')} "
                  f"cursor={props.get('cursor')} text={props.get('text')!r}")

        for index in range(4):
            ch = chr(ord("1") + index)
            client.ok("input.text", {"text": ch})
            # 细粒度采样：看光标是“一开始就错”还是“先对后被拽回去”。
            samples = []
            for step in range(14):
                props = client.props(side)
                samples.append((step * 0.06, props.get("cursor"), props.get("text")))
                time.sleep(0.06)
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
