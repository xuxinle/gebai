"""gbcode：特殊标签（差异/合并/空白对比）关掉后必须回到有内容的编辑器。

### 缺陷（用户报「打开对比标签页，关闭后回到之前的标签页变成空白了，重新打开才有内容」）

内容区是**按 key 互斥选分支**的：差异/合并/对比视图占位时，`CodeEditor` 根本
不在树上。关掉特殊标签 ⇒ 编辑器**被重建**（新实例 `text_` 为空），而
`build()` 末尾的灌入逻辑靠 `loaded_key_` 判断「要不要灌」——它此刻仍等于
当前文件 key（装载那个文件时写下的），于是**灌入被跳过**：
标签栏回到了 `DESIGN.md`，正文却一片空白。

### 本脚本钉的是什么

关掉对比标签之后：标签是那个文件 **且** 编辑器实例里有那个文件的**真实内容**。
只断言“标签回来了”会漏掉这个缺陷（标签一直是对的，空白的是正文）。

跑法：py -3 tools/gbcode_special_tab_close_e2e.py
"""

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
SHOTS = ROOT / "tmp" / "gbcode-special-tab-shots"


class Client:
    """控制通道客户端（4 字节大端长度 + JSON，与其它 gbcode e2e 同一套）。"""

    def __init__(self, port, token):
        self.port = port
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=60.0)
        self.sock.settimeout(60.0)
        self.rid = 0
        self.token = token
        self.call("hello", {"token": token})

    def call(self, method, params=None):
        self.rid += 1
        body = json.dumps({"id": self.rid, "method": method, "params": params or {}}).encode()
        self.sock.sendall(struct.pack(">I", len(body)) + body)
        n = struct.unpack(">I", self._read(4))[0]
        reply = json.loads(self._read(n))
        if not reply.get("ok"):
            raise AssertionError(f"{method} 失败: {reply.get('error')}")
        return reply.get("result") or {}

    def _read(self, size):
        buf = b""
        while len(buf) < size:
            chunk = self.sock.recv(size - len(buf))
            if not chunk:
                raise RuntimeError("控制通道提前关闭")
            buf += chunk
        return buf

    def count(self, selector):
        return self.call("find", {"selector": selector}).get("count", 0)

    def props(self, element_id):
        return (self.call("get", {"id": element_id}).get("props") or {})

    def key(self, key, **mods):
        self.call("input.key", {"key": key, **mods})

    def text(self, text):
        self.call("input.text", {"text": text})


def start(workspace):
    SHOTS.mkdir(parents=True, exist_ok=True)
    ctl = SHOTS / "gbcode-ctl-special.json"
    ctl.unlink(missing_ok=True)
    log_file = open(SHOTS / "gbcode-special-tab.log", "wb")
    proc = subprocess.Popen(
        [str(BIN), "--headless", "--control-port", "0", "--control-file", str(ctl),
         "--shots", str(SHOTS), "--tool-root", str(ROOT), "--workspace", str(workspace)],
        stdout=log_file, stderr=subprocess.STDOUT)
    for _ in range(200):
        if ctl.exists():
            try:
                info = json.loads(ctl.read_text(encoding="utf-8"))
                if info.get("port"):
                    return proc, Client(info["port"], info.get("token", ""))
            except Exception:  # noqa: BLE001
                pass
        time.sleep(0.25)
    proc.kill()
    raise AssertionError("gbcode 控制通道没起来（看 tmp/gbcode-special-tab-shots/）")


def main():
    if not BIN.exists():
        print(f"找不到 {BIN}；先 `st build gbcode --profile dev`")
        return 2

    work = pathlib.Path(tempfile.mkdtemp(prefix="gbcode_special_"))
    marker = "SPECIAL_TAB_MARKER_0123456789"
    (work / "alpha.txt").write_text(f"{marker}\nsecond line\nthird line\n", encoding="utf-8")
    failures = []
    proc = None

    def check(ok, label, extra=""):
        print(("  PASS " if ok else "  FAIL ") + label + (("  " + extra) if extra else ""))
        if not ok:
            failures.append(label)

    try:
        proc, client = start(work)
        # 打开文件（快速打开）
        client.key("p", ctrl=True)
        time.sleep(0.6)
        client.text("alpha.txt")
        time.sleep(0.6)
        client.key("Enter")
        time.sleep(1.0)

        editor = client.props("editor")
        check(marker in (editor.get("text") or ""), "文件已打开且编辑器有内容",
              f"首行={(editor.get('text') or '')[:24]!r}")

        # 打开空白对比标签（特殊标签 ⇒ 编辑器离开树）
        client.key("p", ctrl=True, shift=True)
        time.sleep(0.6)
        client.text("两段文本")
        time.sleep(0.6)
        client.key("Enter")
        time.sleep(1.0)
        tabs = client.props("editor-tabs").get("options") or ""
        check("文本对比" in tabs, "对比标签已打开", f"tabs={tabs}")
        # 编辑器与对比视图在内容区**互斥**（谁在树上取决于实现）：不预设哪一种，
        # 只要求“此刻看得见的是对比视图”。
        check(client.count("DiffView") >= 1, "对比视图占位（内容区在对比视图）")

        # 关掉它（Ctrl+W → close_active 的特殊标签分支）
        client.key("w", ctrl=True)
        time.sleep(1.0)

        tabs = client.props("editor-tabs").get("options") or ""
        check("文本对比" not in tabs, "对比标签已关闭", f"tabs={tabs}")
        check(client.count("CodeEditor") == 1, "编辑器回到了树上")
        if client.count("CodeEditor") == 1:
            text = client.props("editor").get("text") or ""
            check(len(text) > 0, "编辑器不是空白（核心断言：关标签后正文还在）",
                  f"len={len(text)}")
            check(marker in text, "内容就是关对比前的那个文件（没灌成别的/空）",
                  f"首行={text[:24]!r}")

        # 另一条路径：从菜单/动作面关（这里用 Ctrl+W 之外的「文件 → 关闭编辑器」等价路径：
        # 直接调 `close_active` 的对外入口 `close`，与标签 × 同一条链路）。
        client.key("p", ctrl=True, shift=True)
        time.sleep(0.6)
        client.text("两段文本")
        time.sleep(0.6)
        client.key("Enter")
        time.sleep(1.0)
        client.call("invoke", {"id": "editor-tabs", "action": "close", "argument": "1"})
        time.sleep(1.0)
        tabs = client.props("editor-tabs").get("options") or ""
        check("文本对比" not in tabs, "从标签 × 关掉对比后标签已消失", f"tabs={tabs}")
        check(client.count("CodeEditor") == 1, "从标签 × 关掉对比后编辑器在树上")
        if client.count("CodeEditor") == 1:
            text = client.props("editor").get("text") or ""
            check(marker in text, "从标签 × 关掉对比后正文仍然在", f"len={len(text)}")

        shot = SHOTS / "after-close.png"
        try:
            data = client.call("capture", {"encode": "file", "path": str(shot)})
            print(f"  [截图] {data.get('path', shot)}")
        except Exception as exc:  # noqa: BLE001
            print(f"  （截图失败：{exc}）")
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
