"""gbcode Git 能力 E2E（无头 + 控制通道）：五视图 / 提交面板 / 危险操作确认 / 多步控制。

跑法：py -3 tools/gbcode_git_e2e.py
自带一个真 git 仓库（临时，用完即删）与一个真 merge 冲突，走控制通道驱动界面。

与 `gbcode_compare_e2e.py` 的分工：那份只覆盖「对比」一条链路；本脚本覆盖
**写面与五个回顾视图**（含“危险操作必须弹确认”“冲突时给中止入口”这两条安全契约）。
"""

import base64
import json
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

# 非 UTF-8 控制台自愈：GBK 代码页下 print 中文/符号（如 '⇄'）会 UnicodeEncodeError 崩整跑；
# 已是 UTF-8 则不动，残余不可编字符以 '?' 兜底（宁可错印不崩）。
if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "dev" / "bin" / ("gbcode.exe" if __import__("os").name == "nt" else "gbcode")


def log(msg):
    print(msg, flush=True)


def git(cwd, *args):
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)


class Client:
    """控制通道客户端（长度前缀 JSON + 先 hello，与另两份 e2e 同一套）。"""

    def __init__(self, port, token):
        self.port = port
        self.token = token
        self.rid = 0
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30.0)
        self.sock.settimeout(30.0)

    def _read(self, size):
        buf = b""
        while len(buf) < size:
            chunk = self.sock.recv(size - len(buf))
            if not chunk:
                raise RuntimeError("控制通道提前关闭")
            buf += chunk
        return buf

    def _call(self, method, params=None):
        self.rid += 1
        if method == "hello":
            params = dict(params or {}, token=self.token)
        body = json.dumps({"id": self.rid, "method": method, "params": params or {}}).encode()
        self.sock.sendall(struct.pack(">I", len(body)) + body)
        return json.loads(self._read(struct.unpack(">I", self._read(4))[0]))

    def call(self, method, params=None):
        try:
            return self._call(method, params)
        except (ConnectionAbortedError, ConnectionResetError):
            # 重组过渡帧里可能当场断连——重连再试一次（不把偶发断连当成功能坏了）。
            self.sock.close()
            self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=30.0)
            self.sock.settimeout(30.0)
            self.rid = 0
            self._call("hello")
            return self._call(method, params)

    def ok(self, method, params=None):
        reply = self.call(method, params)
        if not reply.get("ok"):
            raise AssertionError(f"{method} 失败: {reply.get('error')}")
        return reply.get("result") or {}

    def count(self, selector):
        return self.ok("find", {"selector": selector})["count"]

    def props(self, element_id):
        return self.ok("get", {"id": element_id})["props"]

    def click(self, selector):
        matches = self.ok("find", {"selector": selector}).get("matches") or []
        if not matches:
            raise AssertionError(f"找不到元素 {selector}")
        bounds = matches[0]["bounds"]
        self.ok("input.mouse", {"kind": "click", "x": bounds["x"] + bounds["width"] / 2.0,
                                "y": bounds["y"] + bounds["height"] / 2.0, "button": 1})
        time.sleep(0.45)

    def screenshot(self, path):
        data = self.ok("capture", {}).get("base64")
        if not data:
            raise AssertionError("截图没有图像数据")
        pathlib.Path(path).write_bytes(base64.b64decode(data))


def wait_until(fn, timeout=25.0, what="条件"):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            last = fn()
            if last:
                return last
        except Exception as exc:  # noqa: BLE001
            last = exc
        time.sleep(0.25)
    raise AssertionError(f"等待超时：{what}（最后：{last!r}）")


def start(workspace):
    shots = ROOT / "tmp" / "gbcode-git-shots"
    shots.mkdir(parents=True, exist_ok=True)
    ctl = shots / "gbcode-ctl.json"
    ctl.unlink(missing_ok=True)
    handle = open(shots / "gbcode-git.log", "wb")
    proc = subprocess.Popen(
        [str(BIN), "--headless", "--control-port", "0", "--control-file", str(ctl),
         "--shots", str(shots), "--tool-root", str(ROOT), "--workspace", str(workspace)],
        stdout=handle, stderr=subprocess.STDOUT)
    for _ in range(160):
        if ctl.exists():
            try:
                info = json.loads(ctl.read_text(encoding="utf-8"))
                if info.get("port"):
                    client = Client(info["port"], info.get("token", ""))
                    client.ok("hello")
                    return proc, client, shots
            except Exception:  # noqa: BLE001
                pass
        time.sleep(0.25)
    proc.kill()
    raise AssertionError("控制通道没起来（看 tmp/gbcode-git-shots/gbcode-git.log）")


def main():
    if not BIN.exists():
        log(f"找不到 {BIN}；先 `st build gbcode --profile dev`")
        return 2

    work = pathlib.Path(tempfile.mkdtemp(prefix="gbcode_git_"))
    repo = work / "repo"
    repo.mkdir()
    git(repo, "init", "-q", "-b", "main")
    git(repo, "config", "user.email", "e2e@local")
    git(repo, "config", "user.name", "E2E")
    (repo / "alpha.txt").write_text("one\ntwo\nthree\n", encoding="utf-8")
    (repo / "beta.txt").write_text("bee\n", encoding="utf-8")
    git(repo, "add", ".")
    git(repo, "commit", "-qm", "first")
    git(repo, "tag", "v0.1")
    # 造冲突的另一半：分支改 alpha.txt。
    git(repo, "checkout", "-q", "-b", "side")
    (repo / "alpha.txt").write_text("one\nSIDE\nthree\n", encoding="utf-8")
    git(repo, "commit", "-qam", "side change")
    git(repo, "checkout", "-q", "main")
    (repo / "alpha.txt").write_text("one\nMAIN\nthree\n", encoding="utf-8")
    git(repo, "commit", "-qam", "main change")
    # 工作区再改一个未暂存文件（变更面板要有内容）。
    (repo / "beta.txt").write_text("bee\nunstaged\n", encoding="utf-8")
    git(repo, "merge", "side")   # 预期冲突

    failures = []

    def check(ok, label):
        log(("  PASS " if ok else "  FAIL ") + label)
        if not ok:
            failures.append(label)

    proc = None
    try:
        proc, client, shots = start(repo)
        # ① 变更视图：工具条 + 分组 + 冲突分组可见。
        client.click("#activity-scm")
        wait_until(lambda: client.count("#scm-toolbar") == 1, what="变更工具条出现")
        check(client.count("#scm-toolbar") == 1, "变更视图有工具条")
        check(client.count("#scm-stage-all") == 1, "工具条有「全部暂存」")
        # 「与…比较」入口已随比较视图下线（1014 提交，用户要求删除比较侧栏）。
        # 工具条本体仍在：改验冲突分组（后续步骤依赖它，顺带钉住工具条在场）。
        check(client.count("#git-group-冲突") == 1, "冲突分组出现（merge 冲突中；比较入口已下线）")

        # ② 底部 Git 工具窗：**三栏**（引用 | 日志 | 提交内容）。
        #    三栏是三个**同时在场**的面板（是一条动线，不是互斥标签）。
        client.click("#activity-git")
        wait_until(lambda: client.count("#git-view-split") == 1, what="Git 三栏出现")
        check(client.count("#git-refs-panel") == 1, "左栏（引用）在场")
        check(client.count("#git-log-panel") == 1, "中栏（日志）在场")
        check(client.count("#git-detail-panel") == 1, "右栏（提交内容）在场")
        check(client.count("#git-log-split") == 1, "中/右栏之间还有一层分栏（嵌套 SplitView）")
        # 三栏的横向次序：引用 < 日志 < 提交内容（同一个 row 里从左到右）。
        refs_box = client.ok("find", {"selector": "#git-refs-panel"})["matches"][0]["bounds"]
        log_box = client.ok("find", {"selector": "#git-log-panel"})["matches"][0]["bounds"]
        detail_box = client.ok("find", {"selector": "#git-detail-panel"})["matches"][0]["bounds"]
        check(refs_box["x"] < log_box["x"] < detail_box["x"],
              f"三栏从左到右是 引用({refs_box['x']:.0f}) < 日志({log_box['x']:.0f}) "
              f"< 提交内容({detail_box['x']:.0f})")

        # 左栏的四个引用分组（日志已占中栏，不再是标签）。
        for tab, label in [("branches", "分支"), ("tags", "标签"),
                           ("stashes", "暂存"), ("remotes", "远程")]:
            check(client.count(f"#git-tab-{tab}") == 1, f"左栏有「{label}」分组")
        check(client.count("#git-tab-log") == 0, "日志不再是左栏标签（已占中栏）")
        check(client.count("#git-log-head") == 1, "中栏有标题行")
        check(client.count("#git-detail-head") == 1, "右栏有标题行")
        check(client.count("#git-detail-empty") == 1, "未选中提交时右栏给空态提示")

        # ③ 多步操作横幅：merge 中必须给「继续/跳过/中止」。
        wait_until(lambda: client.count("#git-operation-banner") == 1, what="多步操作横幅")
        for button in ["git-op-continue", "git-op-skip", "git-op-abort"]:
            check(client.count(f"#{button}") == 1, f"横幅有 #{button}")

        # ④ 标签分组：读到 v0.1（它现在在**左栏**的「标签」分组里）。
        #
        # ⚠ 选择器要写成 `[id="…"]` 而**不是** `#git-tag-v0.1`：标签名里的点号
        # 在 CSS 里是类分隔符，`#git-tag-v0.1` 会被解成 `#git-tag-v0` + `.1`，
        # 永远匹配不到（实测：等 25s 超时，而元素就在那儿）。
        client.click("#git-tab-tags")
        wait_until(lambda: client.count('[id="git-tag-v0.1"]') == 1, what="标签列表")
        check(client.count('[id="git-tag-v0.1"]') == 1, "左栏「标签」分组列出 v0.1")

        # ⑤ 分支分组：当前分支标记 + 新建/删除入口。
        client.click("#git-tab-branches")
        wait_until(lambda: client.count("#git-branch-main") == 1, what="分支列表")
        check(client.count("#git-branch-main") == 1, "左栏「分支」分组列出 main")
        check(client.count("#branch-new") == 1, "分支分组有「新建分支」")

        # ⑥ 新建分支浮层：取消可关（不真的动仓库）。
        client.click("#branch-new")
        wait_until(lambda: client.count("#branch-new-name") == 1, what="新建分支浮层")
        check(client.count("#branch-new-go") == 1, "浮层有「新建并切换」")
        client.click("#branch-new-cancel")
        wait_until(lambda: client.count("#branch-new-name") == 0, what="浮层关闭")
        check(client.count("#branch-new-name") == 0, "取消后浮层关闭")

        # ⑦ 危险操作**必须**先确认：强制推送点下去只弹确认，不执行。
        client.click("#git-tab-remotes")
        wait_until(lambda: client.count("#remote-force-push") == 1, what="远程视图")
        client.click("#remote-force-push")
        wait_until(lambda: client.count("#git-confirm") == 1, what="强推确认浮层")
        check(client.count("#git-confirm") == 1, "强制推送先弹确认（未直接执行）")
        # 对话框要有**可预测的按钮 id**（否则自动化点不到确认/取消）。
        check(client.count("#git-confirm-action-0") == 1, "确认框的取消钮有稳定 id")
        check(client.count("#git-confirm-action-primary") == 1, "确认框的主按钮有稳定 id")
        client.click("#git-confirm-action-0")   # 取消：不要真的强推
        wait_until(lambda: client.count("#git-confirm") == 0, what="确认框关闭")
        check(client.count("#git-confirm") == 0, "取消后确认框关闭，未执行强推")

        shot = shots / "git-views.png"
        client.screenshot(shot)
        check(shot.exists() and shot.stat().st_size > 2000, f"截图落盘 {shot}")

        # ⑧ 中止 merge（走横幅的中止入口）：一步把仓库拉回干净状态。
        client.click("#git-op-abort")
        wait_until(lambda: not (repo / ".git" / "MERGE_HEAD").exists(), timeout=15.0,
                   what="MERGE_HEAD 被清除")
        state = git(repo, "status", "--porcelain").stdout
        check("UU" not in state and "AA" not in state, "中止后不再有冲突文件")
        check(not (repo / ".git" / "MERGE_HEAD").exists(), "MERGE_HEAD 已清除")
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(work, ignore_errors=True)

    log("")
    if failures:
        log(f"失败 {len(failures)} 项：")
        for item in failures:
            log("  - " + item)
        return 1
    log("全部通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
