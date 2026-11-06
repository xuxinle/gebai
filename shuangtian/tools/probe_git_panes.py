"""Git 工具窗三项观感回归（截图 + 属性面）：

  ① 左栏默认打开 **分支** 分组（`git-tab-branches` 是 Soft，且 `#git-pane-branches` 存在）；
  ② 日志栏**只有一个**刷新钮（`log-refresh`），标题行不再重复一个；
  ③ 提交详情：提交信息**多行折行**（长 title 不被省略）、整栏有 `ScrollView`、文件表不被裁。

跑法：py -3 tools/probe_git_panes.py
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
BIN = ROOT / "build" / "dev" / "bin" / "gbcode.exe"
SHOTS = ROOT / "tmp" / "probe-git-panes"

LONG_SUBJECT = ("refactor(raster): 统一三维着色内核并适配 bpy（删旧模型，补 IBL 与多灯光）"
                "——并把光照常量整段改为主题令牌驱动")


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

    def count(self, selector):
        return self.ok("find", {"selector": selector})["count"]


def git(cwd, *args):
    subprocess.run(["git", *args], cwd=cwd, capture_output=True)


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
    raise AssertionError("控制通道没起来（看 tmp/probe-git-panes/probe.log）")


def main():
    work = pathlib.Path(tempfile.mkdtemp(prefix="gbcode_panes_"))
    repo = work
    git(repo, "init", "-q", "-b", "master")
    git(repo, "config", "user.email", "e2e@local")
    git(repo, "config", "user.name", "E2E")
    (repo / "a.txt").write_text("one\n", encoding="utf-8")
    (repo / "b.txt").write_text("alpha\n", encoding="utf-8")
    git(repo, "add", ".")
    git(repo, "commit", "-qm", "first")
    for index in range(6):
        (repo / "a.txt").write_text(f"one {index}\n", encoding="utf-8")
        if index == 3:
            (repo / "b.txt").write_text("alpha beta\n", encoding="utf-8")
            git(repo, "add", "b.txt")
        git(repo, "add", "a.txt")
        git(repo, "commit", "-qm", LONG_SUBJECT if index == 3 else f"tweak {index}")

    failures = []
    proc = None

    def check(ok, label):
        print(("  PASS " if ok else "  FAIL ") + label)
        if not ok:
            failures.append(label)

    try:
        proc, client = start(work)
        # 打开底部 Git 工具窗
        client.ok("invoke", {"id": "activity-git", "action": "click"})
        time.sleep(1.6)
        check(client.count("#git-view") == 1, "Git 工具窗已打开")
        check(client.count("#git-pane-branches") == 1, "左栏默认是「分支」分组")
        check(client.count("#git-tab-branches") == 1 and
              client.props("git-tab-branches").get("variant") == "soft",
              f"「分支」标签是选中态（variant={client.props('git-tab-branches').get('variant')}）")
        # 刷新钮唯一性
        check(client.count("#log-refresh") == 1, "日志栏只有 1 个刷新钮")
        check(client.count("#git-log-head-refresh") == 0, "标题行不再有第二个刷新钮")

        # 选中那笔长信息的提交：表格没有 `click_row` 动作，用真实鼠标点该行（
        # 表头 36 + 行高 24 × 行号 + 行中）。
        table_node = client.ok("find", {"selector": "#git-log-table"})
        box = table_node["matches"][0]["bounds"]
        client.ok("input.mouse", {"kind": "click",
                                  "x": box["x"] + 120,
                                  "y": box["y"] + 36 + 24 * 2 + 12})
        time.sleep(1.5)
        detail = client.props("git-detail-body") if client.count("#git-detail-body") else {}
        print(f"  [detail] body bounds={detail.get('bounds')}")
        check(client.count("#git-detail-scroll") == 1, "提交内容栏有 ScrollView（可垂直滚动）")
        summary = client.props("git-commit-summary")
        print(f"  [detail] summary_bounds={summary.get('bounds')}")
        check(summary.get("multiline") in ("true", "1", True),
              f"提交信息是多行文本（multiline={summary.get('multiline')}）")
        shot = SHOTS / "git-panes.png"
        client.ok("capture", {"path": str(shot)})
        print(f"  [shot] {shot}")
    finally:
        if proc is not None:
            try:
                proc.terminate()
                proc.wait(timeout=10)
            except Exception:  # noqa: BLE001
                pass
        shutil.rmtree(work, ignore_errors=True)

    print(f"\n失败 {len(failures)} 项")
    for item in failures:
        print(" -", item)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
